using System;
using System.Windows.Forms;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Diagnostics;
using System.Threading;
using System.Threading.Tasks;

internal sealed class ManagerWindow:Form {
    readonly Engine engine;Selection saved,draft;
    readonly ListView list=new ListView();readonly Label status=new Label();
    readonly Button enable=new Button(),remove=new Button(),settings=new Button(),apply=new Button();
    readonly System.Windows.Forms.Timer timer=new System.Windows.Forms.Timer();
    bool waiting,bridgeUpdate;
    [DllImport("user32",CharSet=CharSet.Unicode)]static extern uint RegisterWindowMessage(string message);
    [DllImport("user32")]static extern IntPtr SendMessageTimeout(IntPtr w,uint m,IntPtr op,IntPtr arg,uint flags,uint timeout,out IntPtr result);
    public ManagerWindow(Engine e){
        engine=e;saved=e.Read();draft=saved.Duplicate();
        bridgeUpdate=e.BridgeUpdatePending();
        if(bridgeUpdate && !e.Running()){e.UpgradeBridge();bridgeUpdate=false;saved=e.Read();draft=saved.Duplicate();}
        Text="Stylos 插件管理器 "+Engine.Version;StartPosition=FormStartPosition.CenterScreen;MinimumSize=new Size(660,450);ClientSize=new Size(735,465);
        Font=new Font("Microsoft YaHei UI",9);AutoScaleMode=AutoScaleMode.Dpi;
        var root=new TableLayoutPanel{Dock=DockStyle.Fill,Padding=new Padding(12),ColumnCount=1,RowCount=5};
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));root.RowStyles.Add(new RowStyle(SizeType.Percent,100));
        root.RowStyles.Add(new RowStyle(SizeType.AutoSize));root.RowStyles.Add(new RowStyle(SizeType.AutoSize));root.RowStyles.Add(new RowStyle(SizeType.AutoSize));Controls.Add(root);
        var dir=new Label{Text=e.Dir,AutoEllipsis=true,Dock=DockStyle.Fill,AutoSize=true,Padding=new Padding(0,0,0,5)};root.Controls.Add(dir,0,0);
        list.View=View.Details;list.FullRowSelect=true;list.HideSelection=false;list.MultiSelect=false;list.Dock=DockStyle.Fill;
        list.Columns.Add("插件",270);list.Columns.Add("版本",110);list.Columns.Add("状态",108);list.Columns.Add("待应用",120);
        root.Controls.Add(list,0,1);
        var rowMenu=new ContextMenuStrip();
        rowMenu.Items.Add("备份此插件…",null,(s,a)=>Guard(delegate{
            RequireSaved();var p=Selected();
            using(var f=new SaveFileDialog{Filter="插件备份|*.zip",FileName="Stylos-"+p.id+"-Backup.zip"})
                if(f.ShowDialog(this)==DialogResult.OK)engine.BackupPlugin(f.FileName,p.id);
        }));
        rowMenu.Opening+=(s,a)=>{var p=Selected();a.Cancel=p==null || !saved.plugins.ContainsKey(p.id);};
        list.ContextMenuStrip=rowMenu;
        var bar=new FlowLayoutPanel{AutoSize=true,Dock=DockStyle.Fill,WrapContents=true};root.Controls.Add(bar,0,2);
        var add=new Button();Button(bar,add,"从安装包安装…",delegate{
            using(var f=new OpenFileDialog{Filter="Stylos 组件 ZIP|*.zip",Multiselect=true,Title="选择组件安装包"}){
                if(f.ShowDialog(this)!=DialogResult.OK)return;
                var input=Packages.FromFiles(f.FileNames,true);
                if(input.managerUpdate!=null){ConfirmUpdate(input.managerUpdate);return;}
                var next=draft.Duplicate();foreach(var plugin in input.plugins)engine.Edit(next,plugin,"install");
                draft=next;RefreshRows();
                if(input.skipped.Count>0)status.Text="已读取 "+input.plugins.Count+" 个插件。"+input.Notice;
            }
        });
        Button(bar,enable,"启用",delegate{var p=Selected();if(p!=null){engine.Edit(draft,p,draft.enabled.Contains(p.id)?"disable":"enable");RefreshRows();}});
        Button(bar,remove,"卸载",delegate{var p=Selected();if(p!=null){engine.Edit(draft,p,"uninstall");RefreshRows();}});
        Button(bar,settings,"打开设置",OpenSettings);
        Button(bar,apply,"应用更改",delegate{if(waiting){waiting=false;timer.Stop();RefreshRows();return;}if(e.Running()){waiting=true;timer.Start();RefreshRows();}else Commit();});
        status.AutoSize=true;status.Dock=DockStyle.Fill;status.Padding=new Padding(2,4,0,0);root.Controls.Add(status,0,4);
        var maintenance=new FlowLayoutPanel{AutoSize=true,Dock=DockStyle.Fill,WrapContents=true};root.Controls.Add(maintenance,0,3);
        Button(maintenance,new Button(),"在线检查更新…",CheckOnline);
        Button(maintenance,new Button(),"从包更新管理器…",delegate{Maintenance(false);});
        Button(maintenance,new Button(),"备份全部插件…",delegate{
            RequireSaved();
            using(var f=new SaveFileDialog{Filter="插件备份|*.zip",FileName="Stylos-Plugins-Backup.zip"}){
                if(f.ShowDialog(this)==DialogResult.OK){engine.Backup(f.FileName);status.Text="备份已保存。";}
            }
        });
        Button(maintenance,new Button(),"恢复备份…",delegate{
            RequireSaved();
            using(var f=new OpenFileDialog{Filter="插件备份|*.zip"}){
                if(f.ShowDialog(this)!=DialogResult.OK)return;
                if(MessageBox.Show(this,"恢复备份中的版本和启停状态？完整备份会替换插件列表，单插件备份只更新该项及其互斥项。",Text,MessageBoxButtons.YesNo)!=DialogResult.Yes)return;
                engine.RestoreBackup(f.FileName);saved=engine.Read();draft=saved.Duplicate();RefreshRows();status.Text="备份已恢复。";
            }
        });
        Button(maintenance,new Button(),"卸载管理器…",delegate{Maintenance(true);});
        list.SelectedIndexChanged+=(s,a)=>UpdateActions();
        list.DoubleClick+=(s,a)=>{if(settings.Enabled)Guard(OpenSettings);};
        timer.Interval=600;timer.Tick+=(s,a)=>{if(waiting && !engine.Running()){waiting=false;timer.Stop();Guard(Commit);}};
        FormClosing+=(s,a)=>{if(Dirty() && MessageBox.Show(this,"尚有未应用的更改。放弃更改并关闭？",Text,MessageBoxButtons.YesNo,MessageBoxIcon.Question)!=DialogResult.Yes)a.Cancel=true;};
        RefreshRows();
    }
    async void CheckOnline(){
        try{
            RequireSaved();Enabled=false;status.Text="正在检查官方发行包…";
            string package=await Task.Run(()=>engine.FetchLatest());
            ConfirmUpdate(package);
        }catch(Exception ex){if(!IsDisposed)MessageBox.Show(this,ex.Message,Text);}
        finally{if(!IsDisposed){Enabled=true;UpdateActions();}}
    }
    void RequireSaved(){if(waiting || Dirty())throw new IOException("请先应用当前更改，或关闭窗口放弃草稿。");}
    void ConfirmUpdate(string package){
        RequireSaved();
        var next=engine.CheckUpdate(package);
        if(MessageBox.Show(this,"更新管理器至 "+next.version+"？已安装的插件和启停状态会保留。",Text,MessageBoxButtons.YesNo)==DialogResult.Yes &&
            engine.StartMaintenance(package,true))Close();
    }
    void Maintenance(bool remove){
        RequireSaved();
        if(remove){
            if(MessageBox.Show(this,"卸载本目录的管理器和所有受管插件，并恢复基线主程序？插件备份和个人设置会保留。",Text,
                MessageBoxButtons.YesNo,MessageBoxIcon.Warning,MessageBoxDefaultButton.Button2)!=DialogResult.Yes)return;
            if(engine.StartMaintenance(null,true))Close();
        }else using(var f=new OpenFileDialog{Title="选择新版管理器发行包",Filter="管理器 ZIP|*.zip"}){
            if(f.ShowDialog(this)!=DialogResult.OK)return;
            ConfirmUpdate(f.FileName);
        }
    }
    void Button(FlowLayoutPanel panel,Button b,string title,Action action){b.Text=title;b.AutoSize=true;b.Click+=(s,a)=>Guard(action);panel.Controls.Add(b);}
    void Guard(Action action){try{action();}catch(Exception ex){MessageBox.Show(this,ex.Message,Text,MessageBoxButtons.OK,MessageBoxIcon.Warning);UpdateActions();}}
    Plugin Selected(){return list.SelectedItems.Count==0?null:(Plugin)list.SelectedItems[0].Tag;}
    static string State(Selection s,Plugin p){return s.enabled.Contains(p.id)?"已启用":s.plugins.ContainsKey(p.id)?"已停用":"未安装";}
    bool Dirty(){return bridgeUpdate || !saved.Same(draft);}
    void RefreshRows(){
        var selected=Selected();list.BeginUpdate();list.Items.Clear();
        foreach(var p in draft.plugins.Values.Concat(saved.plugins.Values.Where(p=>!draft.plugins.ContainsKey(p.id))).OrderBy(p=>p.name)){var item=new ListViewItem(p.name){Tag=p};item.SubItems.Add(saved.Label(p));item.SubItems.Add(State(saved,p));string want=State(draft,p);
            item.SubItems.Add(draft.Label(p)!=saved.Label(p) && draft.plugins.ContainsKey(p.id) ? want+" "+draft.Label(p) : want==State(saved,p)?"":want);list.Items.Add(item);if(selected!=null && selected.id==p.id)item.Selected=true;}
        list.EndUpdate();UpdateActions();
    }
    void UpdateActions(){
        var p=Selected();bool installed=p!=null && draft.plugins.ContainsKey(p.id);
        enable.Enabled=remove.Enabled=installed&&!waiting;enable.Text=p!=null && draft.enabled.Contains(p.id)?"停用":"启用";
        settings.Enabled=p!=null && !String.IsNullOrEmpty(p.settings) && saved.enabled.Contains(p.id);
        apply.Enabled=Dirty();apply.Text=waiting?"取消等待":"应用更改";
        status.Text=waiting?"等待 Stylos 退出。请保存画稿并正常关闭；管理器将自动应用。":bridgeUpdate?"管理器的画布模块已更新，请应用更改。":Dirty()?"更改尚未写入。":"";
    }
    void Commit(){var current=engine.Read();if(!current.Same(saved)||current.revision!=saved.revision){saved=current;draft=current.Duplicate();RefreshRows();throw new IOException("其他操作已改变插件状态。列表已刷新，请重新选择。");}engine.Apply(draft);bridgeUpdate=false;saved=engine.Read();draft=saved.Duplicate();RefreshRows();status.Text="已应用，重新打开 Stylos 后生效。";}
    void OpenSettings(){
        var p=Selected();if(p==null)return;
        foreach(var proc in Process.GetProcessesByName("Stylos"))using(proc){
            if(!String.Equals(Path.GetDirectoryName(proc.MainModule.FileName),engine.Dir,StringComparison.OrdinalIgnoreCase))continue;
            IntPtr result;if(SendMessageTimeout(proc.MainWindowHandle,RegisterWindowMessage("RETAS.Stylos.DevKit.v1"),(IntPtr)6,(IntPtr)RegisterWindowMessage("RETAS.Stylos.Settings."+p.id),2,1500,out result)!=IntPtr.Zero && result!=IntPtr.Zero)return;
        }
        MessageBox.Show(this,"请先启动此目录的 Stylos，并关闭已经打开的模态对话框。",Text);
    }
}
internal static class Program{
    [STAThread] static int Main(string[] args){
        bool gui=args.Length==0;
        try{
            if(args.Length==2 && args[0]=="--finish-maintenance")return Engine.FinishMaintenance(args[1]);
            
            string exe=System.Reflection.Assembly.GetExecutingAssembly().Location, dir=Path.GetDirectoryName(exe);
            Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
            if(gui && !File.Exists(Path.Combine(dir,"Stylos.exe"))){
                using(var f=new OpenFileDialog{Filter="Stylos.exe|Stylos.exe",Title="选择 Stylos 2.6.0 安装目录"}){
                    if(f.ShowDialog()!=DialogResult.OK)return 0;dir=Path.GetDirectoryName(f.FileName);}
                var target=new Engine(dir);target.Recover();
                if(target.HasLegacy){
                    if(MessageBox.Show("迁移当前 0.1 安装？请选择所有已安装插件对应的完整新版 ZIP。管理器会保存旧版备份，并保留各插件原有的启停状态。", "迁移旧版插件",MessageBoxButtons.YesNo,MessageBoxIcon.Question)!=DialogResult.Yes)return 0;
                    using(var packages=new OpenFileDialog{Title="选择对应的新版插件 ZIP（没有插件时可取消）",Filter="插件 ZIP|*.zip",Multiselect=true}){
                        var chosen=packages.ShowDialog()==DialogResult.OK?packages.FileNames:new string[0];
                        if(chosen.Length==0 && MessageBox.Show("未选择替换包。只迁移没有安装插件的旧版宿主？","迁移旧版插件",
                            MessageBoxButtons.YesNo,MessageBoxIcon.Question,MessageBoxDefaultButton.Button2)!=DialogResult.Yes)return 0;
                        var input=Packages.FromFiles(chosen,false);
                        target.Migrate(input.plugins);
                        if(input.skipped.Count>0)MessageBox.Show("迁移已完成。"+input.Notice,"迁移旧版插件");
                    }
                }else target.Apply(target.Read(),true);
            }
            if(gui){var target=new Engine(dir);target.Recover();using(var mutex=new Mutex(false,"Local\\StylosPluginManager-"+Engine.Hash(System.Text.Encoding.Unicode.GetBytes(dir.ToUpperInvariant())).Substring(0,16))){
                if(!mutex.WaitOne(0)){MessageBox.Show("此目录的插件管理器已经打开。","Stylos");return 0;}
                try{Application.Run(new ManagerWindow(target));}finally{mutex.ReleaseMutex();}
            }return 0;}
            if(args.Length<2)throw new ArgumentException("--setup|--list <folder> | --install <folder> <zip> | --enable|--disable|--uninstall <folder> <id>");
            dir=File.Exists(args[1])?Path.GetDirectoryName(Path.GetFullPath(args[1])):args[1];var e=new Engine(dir);e.Recover();
            if(args[0]=="--migrate"){var input=Packages.FromFiles(args.Skip(2).ToArray(),false);e.Migrate(input.plugins);if(input.skipped.Count>0)Console.WriteLine(input.Notice);return 0;}
            var state=e.Read();
            if(args.Length==4 && args[0]=="--backup"){e.BackupPlugin(args[2],args[3]);return 0;}
            if(args.Length==3 && args[0]=="--backup"){e.Backup(args[2]);return 0;}
            if(args.Length==3 && args[0]=="--restore"){e.RestoreBackup(args[2]);return 0;}
            if(args.Length==2 && args[0]=="--update"){e.StartMaintenance(e.FetchLatest(),false);return 0;}
            if(args.Length==3 && args[0]=="--update"){e.StartMaintenance(args[2],false);return 0;}
            if(args.Length==2 && args[0]=="--self-uninstall"){e.StartMaintenance(null,false);return 0;}
            if(args[0]=="--list"){foreach(var p in state.plugins.Values)Console.WriteLine(p.id+" "+(state.enabled.Contains(p.id)?"enabled":"disabled")+" "+p.version);return 0;}
            if(args[0]=="--setup" && args.Length==2){e.Apply(state,true);return 0;}
            if(args[0]=="--install" && args.Length>=3){
                var input=Packages.FromFiles(args.Skip(2).ToArray(),true);
                if(input.managerUpdate!=null){e.StartMaintenance(input.managerUpdate,false);return 0;}
                foreach(var p in input.plugins)e.Edit(state,p,"install");
                if(input.skipped.Count>0)Console.WriteLine(input.Notice);
            }else{
                if(args.Length!=3)throw new ArgumentException("缺少插件 ID 或组件 ZIP。");
                e.Edit(state,state.plugins[args[2]],args[0].TrimStart('-'));
            }
            e.Apply(state);return 0;
        }catch(Exception ex){if(gui)MessageBox.Show(ex.Message,"Stylos 插件管理器",MessageBoxButtons.OK,MessageBoxIcon.Warning);else Console.Error.WriteLine(ex.Message);return 1;}
    }
}
