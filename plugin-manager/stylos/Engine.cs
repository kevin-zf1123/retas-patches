using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Diagnostics;
using System.Collections.Generic;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

public class Plugin { public string id,name,package; public int bit,page,conflicts; }
public class HostState {public string family;public int flags;}
public class Catalog {
    public int schema; public Plugin[] plugins;
    public Dictionary<string,HostState> hosts;
    public Dictionary<string,string> targets,packages;
    public Dictionary<string,Dictionary<string,string>> workers;
    public Dictionary<string,string[]> known;
}
public class Selection {
    public int installed, enabled;
    public Selection Duplicate(){return new Selection{installed=installed,enabled=enabled};}
}
public class Change {public string key, prior, next;}
internal sealed class Engine {
    static readonly JavaScriptSerializer Json=new JavaScriptSerializer{MaxJsonLength=16*1024*1024};
    public static readonly Catalog Catalog=Json.Deserialize<Catalog>(Encoding.UTF8.GetString(Resource("catalog")));
    public readonly string Dir, Store;
    readonly string exe,profile;
    readonly Dictionary<string,string> paths;
    readonly Dictionary<string,byte[]> assets=new Dictionary<string,byte[]>();
    public Engine(string folder) {
        Dir=Path.GetFullPath(folder);Store=Path.Combine(Dir,".stylos-plugins");exe=Path.Combine(Dir,"Stylos.exe");
        profile=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),@"CELSYS\RETAS STUDIO\Stylos\2.0\Preference\shortcut.rac");
        if(!File.Exists(exe))throw new IOException("请选择包含 Stylos.exe 的目录。");
        paths=new Dictionary<string,string>{{"exe",exe},{"profile",profile},{"state",Path.Combine(Store,"state.json")},{"input",Path.Combine(Store,"input.ini")},{"manager-hash",Path.Combine(Store,"manager.sha256")}};
        foreach(string f in new[]{"StylosPluginManager.exe","StylosCanvasFilter.dll","MotionRender.dll","StylosRenderA.exe","StylosRenderB.exe","wintab32.dll","StylosInk.dll","Stylos-Wacom.cmd"})paths[f]=Path.Combine(Dir,f);
    }
    public static byte[] Resource(string n){using(var s=Assembly.GetExecutingAssembly().GetManifestResourceStream(n))using(var m=new MemoryStream()){if(s==null)throw new IOException("缺少资源 "+n);s.CopyTo(m);return m.ToArray();}}
    byte[] Asset(string n){if(!assets.ContainsKey(n))assets[n]=Resource(n);return assets[n];}
    public static string Hash(byte[] b){if(b==null)return "-";using(var h=SHA256.Create())return BitConverter.ToString(h.ComputeHash(b)).Replace("-","").ToLowerInvariant();}
    static byte[] Bytes(string p){return File.Exists(p)?File.ReadAllBytes(p):null;}
    public static string Digest(string p){return Hash(Bytes(p));}
    static string Hex(byte[] b){return BitConverter.ToString(b).Replace("-","").ToLowerInvariant();}
    static byte[] Delta(byte[] input,string target){
        string from=Hash(input);if(from==target)return input;
        using(var s=Assembly.GetExecutingAssembly().GetManifestResourceStream("recipes"))
        using(var gz=new GZipStream(s,CompressionMode.Decompress))
        using(var r=new BinaryReader(gz)){
            int count=r.ReadInt32();
            for(int i=0;i<count;i++){
                string a=Hex(r.ReadBytes(32)),b=Hex(r.ReadBytes(32));int size=r.ReadInt32(),runs=r.ReadInt32();
                byte[] result=null;if(a==from && b==target){result=new byte[size];Buffer.BlockCopy(input,0,result,0,Math.Min(input.Length,size));}
                for(int j=0;j<runs;j++){
                    int pos=r.ReadInt32(),n=r.ReadInt32();byte[] v=r.ReadBytes(n);
                    if(v.Length!=n)throw new EndOfStreamException();
                    if(result!=null)Buffer.BlockCopy(v,0,result,pos,n);
                }
                if(result!=null){if(Hash(result)!=b)throw new IOException("差分校验失败。");return result;}
            }
        }
        throw new IOException("目录中的构建缺少已知转换："+from+" → "+target);
    }
    public bool Running(){
        foreach(string name in new[]{"Stylos","StylosRenderA","StylosRenderB"})
            foreach(var p in Process.GetProcessesByName(name))using(p){
                try{if(String.Equals(Path.GetDirectoryName(p.MainModule.FileName),Dir,StringComparison.OrdinalIgnoreCase))return true;}
                catch(InvalidOperationException){}
            }
        return false;
    }
    public Selection Read(){
        HostState h;
        if(!Catalog.hosts.TryGetValue(Digest(exe),out h))throw new IOException("主程序不是注册表支持的 Stylos 2.6.0 构建。");
        var s=File.Exists(paths["state"])?Json.Deserialize<Selection>(File.ReadAllText(paths["state"])):new Selection();
        int input=0;string wt=Digest(paths["wintab32.dll"]);
        if(Catalog.known["native"].Contains(wt))input=16;
        if(Catalog.known["inktab"].Contains(wt))input=32;
        int controls=File.Exists(paths["state"])?s.enabled&192:(input==16?192:0);
        s.enabled=h.flags|input|controls;s.installed|=s.enabled;
        return s;
    }
    public Plugin Import(string path){
        string hash=Digest(path);string id;if(!Catalog.packages.TryGetValue(hash,out id))throw new IOException("安装包未登记或校验值不匹配。请解压合集并选择其中一个组件 ZIP。");
        return Catalog.plugins.Single(p=>p.id==id);
    }
    public void Edit(Selection s,Plugin p,string operation){
        switch(operation){
        case "install":s.installed|=p.bit;s.enabled=(s.enabled&~p.conflicts)|p.bit;break;
        case "uninstall":s.installed&=~p.bit;s.enabled&=~p.bit;break;
        case "enable":
            if((s.installed&p.bit)==0)throw new IOException("请先从安装包安装。");
            s.enabled=(s.enabled&~p.conflicts)|p.bit;break;
        case "disable":s.enabled&=~p.bit;break;
        default:throw new ArgumentException(operation);
        }
    }
    static void Atomic(string path,byte[] bytes){
        if(bytes==null){if(File.Exists(path))File.Delete(path);return;}
        Directory.CreateDirectory(Path.GetDirectoryName(path));
        string tmp=path+"."+Guid.NewGuid().ToString("N")+".tmp";
        try{
            using(var f=new FileStream(tmp,FileMode.CreateNew,FileAccess.Write,FileShare.None)){f.Write(bytes,0,bytes.Length);f.Flush(true);}
            if(File.Exists(path))File.Replace(tmp,path,null);else File.Move(tmp,path);
        }finally{if(File.Exists(tmp))File.Delete(tmp);}
    }
    string Journal {get{return Path.Combine(Store,"recovery.json");}}
    void Restore(){
        if(!File.Exists(Journal))return;
        var changes=Json.Deserialize<List<Change>>(File.ReadAllText(Journal));
        changes.Reverse();
        foreach(Change c in changes){
            string path=paths[c.key],current=Digest(path);
            if(current==c.prior)continue;
            if(current!=c.next)throw new IOException("检测到外部更改，保留备份："+path);
            byte[] old=c.prior=="-"?null:File.ReadAllBytes(Path.Combine(Store,"backups",c.prior));
            if(Hash(old)!=c.prior)throw new IOException("恢复副本校验失败。");
            Atomic(path,old);
        }
        File.Delete(Journal);
    }
    public void Recover(){
        if(!File.Exists(Journal))return;
        if(Running())throw new IOException("请先退出 Stylos 以恢复上次未完成的更改。");
        using(var guard=Lock()){Restore();}
    }
    FileStream Lock(){Directory.CreateDirectory(Store);return new FileStream(Path.Combine(Store,"apply.lock"),FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.None,1,FileOptions.DeleteOnClose);}
    public void Apply(Selection s,bool setup=false){
        using(var guard=Lock()){
            if(Running())throw new IOException("请先退出该目录的 Stylos 和渲染进程，然后应用。");
            Restore();
            if((s.enabled&~s.installed)!=0 || (s.installed&~255)!=0 || (s.enabled&48)==48)throw new IOException("组件状态冲突。");
            byte[] oldExe=File.ReadAllBytes(exe);HostState current;
            if(!Catalog.hosts.TryGetValue(Hash(oldExe),out current))throw new IOException("未识别主程序。");
            string target;
            if(!Catalog.targets.TryGetValue(current.family+":"+(s.enabled&15),out target))throw new IOException("该基线不支持此组合（高 DPI 需要中文 4 GiB 基线）。");
            byte[] basis=Delta(oldExe,current.family);
            var desired=new Dictionary<string,byte[]>();
            desired["StylosCanvasFilter.dll"]=Asset("filter");
            var workers=(s.enabled&2)!=0?Catalog.workers[current.family+":"+(s.enabled&3)]:null;
            foreach(string k in new[]{"StylosRenderA.exe","StylosRenderB.exe"})
                desired[k]=workers==null?null:Delta(basis,workers[k]);
            desired["MotionRender.dll"]=workers==null?null:Asset("motion");
            int provider=s.enabled&48;
            var existingProvider=Digest(paths["wintab32.dll"]);
            if(provider!=0 || Catalog.known["native"].Contains(existingProvider) || Catalog.known["inktab"].Contains(existingProvider)){
                desired["wintab32.dll"]=provider==16?Asset("native"):provider==32?Asset("inktab"):null;
                desired["StylosInk.dll"]=provider==32?Asset("ink"):null;
                desired["Stylos-Wacom.cmd"]=provider==16?Asset("wacom"):null;
            }
            foreach(var pair in desired){
                string hash=Digest(paths[pair.Key]);
                if(hash!="-" && hash!=Hash(pair.Value) && !Catalog.known[pair.Key].Contains(hash))
                    throw new IOException("已有其他版本的文件，已保留："+paths[pair.Key]);
            }
            if(setup){
                byte[] self=File.ReadAllBytes(Assembly.GetExecutingAssembly().Location);
                if(File.Exists(paths["StylosPluginManager.exe"]) && Digest(paths["StylosPluginManager.exe"])!=Hash(self) &&
                    (!File.Exists(paths["manager-hash"]) || File.ReadAllText(paths["manager-hash"])!=Digest(paths["StylosPluginManager.exe"])))
                    throw new IOException("目录中已有其他版本的管理器，已保留。");
                desired["StylosPluginManager.exe"]=self;
                desired["manager-hash"]=Encoding.ASCII.GetBytes(Hash(self));
            }
            byte[] previous=Bytes(profile);
            if(previous==null)throw new IOException("请先打开并退出一次 Stylos，生成快捷键配置。");
            char key;byte[] keys=ShortcutBinding.Bind(previous,out key,"EDIT_PLUGIN_MANAGER",3,"PJM");
            if((s.enabled&8)!=0)keys=ShortcutBinding.Bind(keys,out key);
            desired["profile"]=keys;
            desired["input"]=Encoding.ASCII.GetBytes("[Input]\r\nWheel="+((s.enabled&128)!=0?"1":"0")+"\r\nRotation="+((s.enabled&64)!=0?"1":"0")+"\r\n");
            desired["exe"]=Delta(basis,target);
            desired["state"]=Encoding.UTF8.GetBytes(Json.Serialize(s));
            var changes=new List<Change>();Directory.CreateDirectory(Path.Combine(Store,"backups"));
            foreach(var pair in desired){
                byte[] prior=Bytes(paths[pair.Key]);string h=Hash(prior),next=Hash(pair.Value);
                if(h==next)continue;
                if(prior!=null){
                    string backup=Path.Combine(Store,"backups",h);
                    if(!File.Exists(backup))Atomic(backup,prior);
                    if(Digest(backup)!=h)throw new IOException("备份校验失败。");
                }
                changes.Add(new Change{key=pair.Key,prior=h,next=next});
            }
            Atomic(Journal,Encoding.UTF8.GetBytes(Json.Serialize(changes)));
            try{
                foreach(var c in changes){
                    if(Digest(paths[c.key])!=c.prior)throw new IOException("应用期间文件已变化："+paths[c.key]);
                    Atomic(paths[c.key],desired[c.key]);
                    if(Digest(paths[c.key])!=c.next)throw new IOException("写入校验失败："+paths[c.key]);
                }
                File.Delete(Journal);
            }catch{Restore();throw;}
        }
    }
}
