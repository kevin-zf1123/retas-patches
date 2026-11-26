using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Diagnostics;
using System.Collections.Generic;
using System.Reflection;
using System.Runtime.InteropServices;
using Retas.DevKit;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;

public class Plugin { public string id,name,package,version,currentSha256,v0Sha256,legacyPackageSha256;
    [ScriptIgnore] public string importedHash;
    [ScriptIgnore] public byte[] archive;
    public Plugin Copy(){return (Plugin)MemberwiseClone();}
    public int bit,page,conflicts; }
internal class ComponentManifest {public string id {get;set;} public string host {get;set;} public string version {get;set;} public int schema {get;set;}}
public class HostState {public string family;public int flags;}
public class Catalog {
    public string managerVersion; public Dictionary<string,string> packageVersions;
    public int schema,hostMask; public Plugin[] plugins;
    public Dictionary<string,HostState> hosts;
    public Dictionary<string,string> targets,packages;
    public Dictionary<string,Dictionary<string,string>> workers;
    public Dictionary<string,string[]> known;
}
public class Selection {
    public int installed, enabled, managerShortcut;
    public Dictionary<string,string> sources;
    [ScriptIgnore] public Dictionary<string,byte[]> imports=new Dictionary<string,byte[]>();
    [ScriptIgnore] public string revision;
    public Selection Duplicate(){return new Selection{installed=installed,enabled=enabled,managerShortcut=managerShortcut,revision=revision,
        sources=new Dictionary<string,string>(sources),imports=new Dictionary<string,byte[]>(imports)};}
    public bool Same(Selection other){
        return installed==other.installed && enabled==other.enabled && sources.Count==other.sources.Count &&
            sources.All(p=>other.sources.ContainsKey(p.Key) && other.sources[p.Key]==p.Value);
    }
    public string Label(Plugin p){
        if((installed&p.bit)==0)return "—";
        string hash,version,id;
        return sources.TryGetValue(p.id,out hash) && !String.IsNullOrEmpty(hash) &&
            Engine.Catalog.packages.TryGetValue(hash,out id) && id==p.id &&
            Engine.Catalog.packageVersions.TryGetValue(hash,out version) && version!="" ? version : "无注册版本";
    }
}
public class Change {public string key, prior, next;}
internal sealed partial class Engine {
    static readonly JavaScriptSerializer Json=new JavaScriptSerializer{MaxJsonLength=16*1024*1024};
    public static readonly Catalog Catalog=Json.Deserialize<Catalog>(Encoding.UTF8.GetString(Resource("catalog")));
    static readonly PatchCatalog Layouts=PatchCatalog.Open(Resource("layouts"));
    public readonly string Dir, Store;
    readonly string exe,profile;
    readonly Dictionary<string,string> paths;
    readonly Dictionary<string,byte[]> assets=new Dictionary<string,byte[]>();
    public Engine(string folder) : this(folder,Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),@"CELSYS\RETAS STUDIO\Stylos\2.0\Preference\shortcut.rac")) {}
    internal Engine(string folder,string shortcutProfile) {
        Dir=Path.GetFullPath(folder);Store=Path.Combine(Dir,".stylos-plugins");exe=Path.Combine(Dir,"Stylos.exe");
        profile=shortcutProfile;
        if(!File.Exists(exe))throw new IOException("请选择包含 Stylos.exe 的目录。");
        paths=new Dictionary<string,string>{{"exe",exe},{"profile",profile},{"state",Path.Combine(Store,"state.json")},{"layout",Path.Combine(Store,"layout.json")},{"input",Path.Combine(Store,"input.ini")},{"manager-hash",Path.Combine(Store,"manager.sha256")}};
        foreach(var plugin in Catalog.plugins)paths["package-"+plugin.id]=Path.Combine(Store,"packages",plugin.id+".zip");
        foreach(string f in new[]{"StylosPluginManager.exe","StylosCanvasFilter.dll","MotionRender.dll","StylosRender64.exe","StylosRenderA.exe","StylosRenderB.exe","wintab32.dll","StylosInk.dll","Stylos-Wacom.cmd"})paths[f]=Path.Combine(Dir,f);
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
    [DllImport("kernel32.dll",SetLastError=true)]
    static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode,SetLastError=true)]
    static extern bool QueryFullProcessImageName(IntPtr process,uint flags,StringBuilder path,ref int length);
    [DllImport("kernel32.dll")] static extern bool CloseHandle(IntPtr handle);
    public bool Running(){
        foreach(string name in new[]{"Stylos","StylosRenderA","StylosRenderB","StylosRender64"})
            foreach(var p in Process.GetProcessesByName(name))using(p){
                var handle=OpenProcess(0x1000,false,p.Id); // PROCESS_QUERY_LIMITED_INFORMATION also works across bitness.
                if(handle==IntPtr.Zero)continue;
                try{
                    int length=32768;var path=new StringBuilder(length);
                    if(QueryFullProcessImageName(handle,0,path,ref length)&&
                       String.Equals(Path.GetDirectoryName(path.ToString()),Dir,StringComparison.OrdinalIgnoreCase))return true;
                }finally{CloseHandle(handle);}
            }
        return false;
    }
    public Selection Read(){
        HostState h;
        string hostHash=Digest(exe);
        if(!Catalog.hosts.TryGetValue(hostHash,out h))throw new IOException("主程序不是注册表支持的 Stylos 2.6.0 构建。");
        byte[] savedState=Bytes(paths["state"]);
        var s=savedState!=null?Json.Deserialize<Selection>(Encoding.UTF8.GetString(savedState)):new Selection();
        int input=0;string wt=Digest(paths["wintab32.dll"]);
        if(Catalog.known["native"].Contains(wt))input=16;
        if(Catalog.known["inktab"].Contains(wt))input=32;
        int controls=savedState!=null?s.enabled&192:(input==16?192:0);
        int recorded=s.installed;
        s.enabled=h.flags|input|controls;s.installed|=s.enabled;
        if(s.sources==null){
            s.sources=new Dictionary<string,string>();
            bool v0=savedState!=null && File.Exists(paths["layout"]) &&
                File.Exists(paths["manager-hash"]) && File.ReadAllText(paths["manager-hash"])==Digest(paths["StylosPluginManager.exe"]);
            if(v0){
                var prior=Json.Deserialize<Dictionary<string,object>>(File.ReadAllText(paths["layout"]));
                v0=prior.ContainsKey("host") && Equals(prior["host"],hostHash);
            }
            foreach(var p in Catalog.plugins)
                if(v0 && p.v0Sha256!=null && (recorded&p.bit)!=0)s.sources[p.id]=p.v0Sha256;
        }
        foreach(var p in Catalog.plugins){
            if((s.installed&p.bit)==0){s.sources.Remove(p.id);continue;}
            if(!s.sources.ContainsKey(p.id))s.sources[p.id]="";
        }
        s.revision=Hash(savedState);
        return s;
    }
    public Plugin Import(string path){
        if(new FileInfo(path).Length>32*1024*1024)throw new IOException("组件包过大。");
        var bytes=File.ReadAllBytes(path);
        var plugin=IdentifyPackage(bytes);
        plugin.archive=bytes;
        return plugin;
    }
    static Plugin IdentifyPackage(byte[] bytes){
        string source=Hash(bytes),id;
        if(!Catalog.packages.TryGetValue(source,out id)){
            using(var memory=new MemoryStream(bytes))using(var zip=new ZipArchive(memory,ZipArchiveMode.Read)){
                if(zip.GetEntry("plugin.json")!=null){
                    var manifest=Json.Deserialize<ComponentManifest>(Encoding.UTF8.GetString(ZipMember(zip,"plugin.json",65536)));
                    if(manifest==null || manifest.host!="stylos-2.6.0-x86" || (manifest.schema!=1 && manifest.schema!=2))
                        throw new InvalidDataException("组件清单与本版 Stylos 不兼容。");
                    var component=Catalog.plugins.SingleOrDefault(p=>p.id==manifest.id);
                    if(component==null)throw new IOException("未知组件："+manifest.id);
                    id=component.id;
                    if(manifest.version==component.version)source=component.currentSha256;
                    else if(manifest.version=="v0" || (manifest.schema==2 && String.IsNullOrEmpty(manifest.version)))source=component.v0Sha256;
                    else source=Catalog.packages.Keys.FirstOrDefault(k=>Catalog.packages[k]==id &&
                        Catalog.packageVersions[k]==manifest.version)??component.legacyPackageSha256;
                }else{
                    var markers=new Dictionary<string,string>{
                        {"Apply.ps1","high-dpi"},{"MotionSetup.ps1","motion-check"},{"StylosCanvasSetup.exe","anti-alias"},
                        {"StylosInbetweenSetup.exe","inbetween"},{"Stylos-Wacom.cmd","tablet-native"},{"StylosInk.dll","windows-ink"}};
                    var matches=markers.Where(pair=>zip.GetEntry(pair.Key)!=null).ToArray();
                    if(matches.Length!=1)throw new InvalidDataException("请选择单个 Stylos 组件 ZIP。");
                    id=matches[0].Value;
                    source=Catalog.plugins.Single(p=>p.id==id).legacyPackageSha256;
                }
            }
        }
        if(String.IsNullOrEmpty(source))throw new InvalidDataException("未登记的组件版本："+id);
        var result=Catalog.plugins.Single(p=>p.id==id).Copy();
        result.importedHash=source;
        return result;
    }
    static bool MatchesSource(Plugin identified,Plugin expected,string source){
        return identified.id==expected.id && (identified.importedHash==source ||
            (source!="" && Catalog.packageVersions[identified.importedHash]==Catalog.packageVersions[source]));
    }
    public void Edit(Selection s,Plugin p,string operation){
        switch(operation){
        case "install":
            s.sources[p.id]=p.importedHash??p.currentSha256;
            if(p.archive!=null)s.imports[p.id]=p.archive;
            s.installed|=p.bit;s.enabled=(s.enabled&~p.conflicts)|p.bit;break;
        case "uninstall":s.sources.Remove(p.id);s.imports.Remove(p.id);s.installed&=~p.bit;s.enabled&=~p.bit;break;
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
        string tmp=Path.Combine(Path.GetDirectoryName(path),Guid.NewGuid().ToString("N")+".tmp");
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
            if(s.revision!=null && s.revision!=Digest(paths["state"]))throw new IOException("插件状态已经改变，请刷新后重试。");
            if((s.enabled&~s.installed)!=0 || (s.installed&~Catalog.plugins.Sum(p=>p.bit))!=0 || (s.enabled&48)==48)throw new IOException("组件状态冲突。");
            byte[] oldExe=File.ReadAllBytes(exe);HostState current;
            if(!Catalog.hosts.TryGetValue(Hash(oldExe),out current))throw new IOException("未识别主程序。");
            string target;
            if(!Catalog.targets.TryGetValue(current.family+":"+(s.enabled&Catalog.hostMask),out target))throw new IOException("该基线不支持此组合（高 DPI 需要中文 4 GiB 基线）。");
            byte[] basis=Delta(oldExe,current.family);
            var desired=new Dictionary<string,byte[]>();
            desired["StylosCanvasFilter.dll"]=Asset("filter");
            var motion=Catalog.plugins.Single(p=>p.id=="motion-check");
            bool motionEnabled=(s.enabled&motion.bit)!=0;
            string motionVersion=s.Label(motion);
            bool standalone=motionEnabled && motionVersion==motion.version;
            var workers=motionEnabled && !standalone?Catalog.workers[current.family+":"+(s.enabled&3)]:null;
            var layout=new Dictionary<string,PatchRegion[]>();
            PatchRegion[] regions;
            foreach(string k in new[]{"StylosRenderA.exe","StylosRenderB.exe"}){
                if(workers==null){desired[k]=null;continue;}
                desired[k]=Layouts.Build(basis,k,s.enabled,workers[k],out regions);
                layout[k]=regions;
            }
            string engine=standalone?"motion":motionVersion=="0.1.1"?"motion-0.1.1":"motion-previous";
            desired["MotionRender.dll"]=motionEnabled?Asset(engine):null;
            desired["StylosRender64.exe"]=standalone?Asset("renderer64"):null;
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
            if(!Object.ReferenceEquals(keys,previous))s.managerShortcut=key;
            if((s.enabled&8)!=0)keys=ShortcutBinding.Bind(keys,out key);
            desired["profile"]=keys;
            desired["input"]=Encoding.ASCII.GetBytes("[Input]\r\nWheel="+((s.enabled&128)!=0?"1":"0")+"\r\nRotation="+((s.enabled&64)!=0?"1":"0")+"\r\n");
            desired["exe"]=Layouts.Build(basis,"Stylos.exe",s.enabled,target,out regions);
            layout["Stylos.exe"]=regions;
            desired["layout"]=Encoding.UTF8.GetBytes(Json.Serialize(new {version=1,host=target,images=layout}));
            foreach(var p in Catalog.plugins){
                string k="package-"+p.id;
                if((s.installed&p.bit)==0){desired[k]=null;continue;}
                string source=s.sources[p.id];
                if(source!="" && (!Catalog.packages.ContainsKey(source) || Catalog.packages[source]!=p.id))
                    throw new IOException("插件版本记录无效："+p.name);
                if(s.imports.ContainsKey(p.id)){
                    if(!MatchesSource(IdentifyPackage(s.imports[p.id]),p,source))throw new IOException("组件包与所选版本不符。");
                    desired[k]=s.imports[p.id];
                }else if(File.Exists(paths[k]) && !MatchesSource(IdentifyPackage(File.ReadAllBytes(paths[k])),p,source))desired[k]=null;
            }
            desired["state"]=Encoding.UTF8.GetBytes(Json.Serialize(s));
            CommitFiles(desired);
            s.revision=Hash(desired["state"]);
            s.imports.Clear();
        }
    }
    void CommitFiles(Dictionary<string,byte[]> desired){
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
