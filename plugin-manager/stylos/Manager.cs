using System;
using System.Windows.Forms;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Diagnostics;
using System.Threading;

internal sealed class ManagerWindow:Form {
    readonly Engine engine;Selection saved,draft;
    readonly ListView list=new ListView();readonly Label status=new Label();
    readonly Button enable=new Button(),remove=new Button(),settings=new Button(),apply=new Button();
    readonly System.Windows.Forms.Timer timer=new System.Windows.Forms.Timer();
    bool waiting;
    [DllImport("user32",CharSet=CharSet.Unicode)]static extern uint RegisterWindowMessage(string message);
    [DllImport("user32")]static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr op,IntPtr arg,uint flags,uint timeout,out IntPtr result);
    public ManagerWindow(Engine e){
        engine=e;saved=e.Read();draft=saved.Duplicate();
        Text="Stylos 插件管理器";StartPosition=FormStartPosition.CenterScreen;MinimumSize=new Size(590,405);ClientSize=new Size(650,423);
        Font=new Font("Microsoft YaHei UI",9);AutoScaleMode=AutoScaleMode.Dpi;
        var root=new TableLayoutPanel{Dock=DockStyle.Fill,Padding=new Padding(12),ColumnCount=1,RowCount=4};
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));root.RowStyles.Add(new RowStyle(SizeType.Percent,100));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));root.RowStyles.Add(new RowStyle(SizeType.AutoSize));Controls.Add(root);
        var dir=new Label{Text=e.Dir,AutoEllipsis=true,Dock=DockStyle.Fill,AutoSize=true,Padding=new Padding(0,0,0,5)};root.Controls.Add(dir,0,0);
        list.View=View.Details;list.FullRowSelect=true;list.HideSelection=false;list.MultiSelect=false;list.Dock=DockStyle.Fill;
        list.Columns.Add("插件",310);list.Columns.Add("状态",108);list.Columns.Add("待应用",120);
        root.Controls.Add(list,0,1);
        var bar=new FlowLayoutPanel{AutoSize=true,Dock=DockStyle.Fill,WrapContents=true};root.Controls.Add(bar,0,2);
        var add=new Button();Button(bar,add,"从安装包安装…",delegate{
            using(var f=new OpenFileDialog{Filter="Stylos 组件 ZIP|*.zip",Multiselect=true,Title="选择组件安装包"}){
                if(f.ShowDialog(this)!=DialogResult.OK)return;
                var next=draft.Duplicate();foreach(var file in f.FileNames)engine.Edit(next,engine.Import(file),"install");
                draft=next;RefreshRows();
            }
        });
        Button(bar,enable,"启用",delegate{var p=Selected();if(p!=null){engine.Edit(draft,p,(draft.enabled&p.bit)!=0?"disable":"enable");RefreshRows();}});
        Button(bar,remove,"卸载",delegate{var p=Selected();if(p!=null){engine.Edit(draft,p,"uninstall");RefreshRows();}});
        Button(bar,settings,"打开设置",OpenSettings);
        Button(bar,apply,"应用更改",delegate{if(waiting){waiting=false;timer.Stop();RefreshRows();return;}if(e.Running()){waiting=true;timer.Start();RefreshRows();}else Commit();});
        status.AutoSize=true;status.Dock=DockStyle.Fill;status.Padding=new Padding(2,4,0,0);root.Controls.Add(status,0,3);
        list.SelectedIndexChanged+=(s,a)=>UpdateActions();
        list.DoubleClick+=(s,a)=>{if(settings.Enabled)Guard(OpenSettings);};
        timer.Interval=600;timer.Tick+=(s,a)=>{if(waiting && !engine.Running()){waiting=false;timer.Stop();Guard(Commit);}};
        FormClosing+=(s,a)=>{if(Dirty() && MessageBox.Show(this,"尚有未应用的更改。放弃更改并关闭？",Text,MessageBoxButtons.YesNo,MessageBoxIcon.Question)!=DialogResult.Yes)a.Cancel=true;};
        RefreshRows();
    }
    void Button(FlowLayoutPanel panel,Button b,string title,Action action){b.Text=title;b.AutoSize=true;b.Click+=(s,a)=>Guard(action);panel.Controls.Add(b);}
    void Guard(Action action){try{action();}catch(Exception ex){MessageBox.Show(this,ex.Message,Text,MessageBoxButtons.OK,MessageBoxIcon.Warning);UpdateActions();}}
    Plugin Selected(){return list.SelectedItems.Count==0?null:(Plugin)list.SelectedItems[0].Tag;}
    static string State(Selection s,Plugin p){return (s.enabled&p.bit)!=0?"已启用":(s.installed&p.bit)!=0?"已停用":"未安装";}
    bool Dirty(){return saved.installed!=draft.installed || saved.enabled!=draft.enabled;}
    void RefreshRows(){
        var selected=Selected();list.BeginUpdate();list.Items.Clear();
        foreach(var p in Engine.Catalog.plugins){var item=new ListViewItem(p.name){Tag=p};item.SubItems.Add(State(saved,p));string want=State(draft,p);
            item.SubItems.Add(want==State(saved,p)?"":want);list.Items.Add(item);if(selected!=null && selected.id==p.id)item.Selected=true;}
        list.EndUpdate();UpdateActions();
    }
    void UpdateActions(){
        var p=Selected();bool installed=p!=null && (draft.installed&p.bit)!=0;
        enable.Enabled=remove.Enabled=installed&&!waiting;enable.Text=p!=null && (draft.enabled&p.bit)!=0?"停用":"启用";
        settings.Enabled=p!=null && p.page>0 && (saved.enabled&p.bit)!=0;
        apply.Enabled=Dirty();apply.Text=waiting?"取消等待":"应用更改";
        status.Text=waiting?"等待 Stylos 退出。请保存画稿并正常关闭；管理器将自动应用。":Dirty()?"更改尚未写入。":"";
    }
    void Commit(){var current=engine.Read();if(current.installed!=saved.installed||current.enabled!=saved.enabled){saved=current;draft=current.Duplicate();RefreshRows();throw new IOException("其他操作已改变插件状态。列表已刷新，请重新选择。");}engine.Apply(draft);saved=engine.Read();draft=saved.Duplicate();RefreshRows();status.Text="已应用，重新打开 Stylos 后生效。";}
    void OpenSettings(){
        var p=Selected();if(p==null)return;
        foreach(var proc in Process.GetProcessesByName("Stylos"))using(proc){
            if(!String.Equals(Path.GetDirectoryName(proc.MainModule.FileName),engine.Dir,StringComparison.OrdinalIgnoreCase))continue;
            IntPtr result;if(SendMessageTimeout(proc.MainWindowHandle,RegisterWindowMessage("RETAS.Stylos.DevKit.v1"),(IntPtr)6,(IntPtr)p.page,2,1500,out result)!=IntPtr.Zero && result!=IntPtr.Zero)return;
        }
        MessageBox.Show(this,"请先启动此目录的 Stylos，并关闭已经打开的模态对话框。",Text);
    }
}
internal static class Program{
    [STAThread] static int Main(string[] args){
        bool gui=args.Length==0;
        try{
            if(Engine.Catalog.schema!=1)throw new IOException("不支持的注册表版本。");
            string exe=System.Reflection.Assembly.GetExecutingAssembly().Location, dir=Path.GetDirectoryName(exe);
            Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
            if(gui && !File.Exists(Path.Combine(dir,"Stylos.exe"))){
                using(var f=new OpenFileDialog{Filter="Stylos.exe|Stylos.exe",Title="选择 Stylos 2.6.0 安装目录"}){
                    if(f.ShowDialog()!=DialogResult.OK)return 0;dir=Path.GetDirectoryName(f.FileName);}
                var target=new Engine(dir);target.Recover();target.Apply(target.Read(),true);
            }
            if(gui){var target=new Engine(dir);target.Recover();using(var mutex=new Mutex(false,"Local\\StylosPluginManager-"+Engine.Hash(System.Text.Encoding.Unicode.GetBytes(dir.ToUpperInvariant())).Substring(0,16))){
                if(!mutex.WaitOne(0)){MessageBox.Show("此目录的插件管理器已经打开。","Stylos");return 0;}
                try{Application.Run(new ManagerWindow(target));}finally{mutex.ReleaseMutex();}
            }return 0;}
            if(args.Length<2)throw new ArgumentException("--setup|--list <folder> | --install <folder> <zip> | --enable|--disable|--uninstall <folder> <id>");
            dir=File.Exists(args[1])?Path.GetDirectoryName(Path.GetFullPath(args[1])):args[1];var e=new Engine(dir);e.Recover();var state=e.Read();
            if(args[0]=="--list"){foreach(var p in Engine.Catalog.plugins)Console.WriteLine(p.id+" "+((state.enabled&p.bit)!=0?"enabled":(state.installed&p.bit)!=0?"disabled":"absent"));return 0;}
            if(args[0]=="--setup" && args.Length==2){e.Apply(state,true);return 0;}
            if(args.Length!=3)throw new ArgumentException("缺少插件 ID 或组件 ZIP。");
            if(args[0]=="--install")e.Edit(state,e.Import(args[2]),"install");
            else e.Edit(state,Engine.Catalog.plugins.Single(p=>p.id==args[2]),args[0].TrimStart('-'));
            e.Apply(state);return 0;
        }catch(Exception ex){if(gui)MessageBox.Show(ex.Message,"Stylos 插件管理器",MessageBoxButtons.OK,MessageBoxIcon.Warning);else Console.Error.WriteLine(ex.Message);return 1;}
    }
}
