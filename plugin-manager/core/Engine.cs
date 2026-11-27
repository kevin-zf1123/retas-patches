using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Reflection;
using System.Diagnostics;
using System.Runtime.InteropServices;
using System.Collections.Generic;
using System.Web.Script.Serialization;
using Retas.DevKit;

internal sealed class State {
    public int schema=2;
    public string basis;
    public Installed[] plugins=new Installed[0];
    public Dictionary<string,string> managed=new Dictionary<string,string>(StringComparer.OrdinalIgnoreCase);
    public int managerShortcut;
}
internal sealed class Change {public string key,prior,next;}
internal sealed partial class Engine {
    internal const string Version="0.2.2";
    const string BridgeName="StylosCanvasFilter.dll", ExeName="Stylos.exe";
    const string StateName=".stylos-plugins/installed-v2.json";
    internal readonly string Dir,Store;
    readonly string profile;
    static readonly JavaScriptSerializer Json=Packages.Json;
    static readonly Assembly assembly=typeof(Engine).Assembly;
    internal static string Hash(byte[] b){return Packages.Hash(b);}
    static byte[] Resource(string name){
        using(var s=assembly.GetManifestResourceStream(name)){
            if(s==null)throw new InvalidDataException("Unsupported Stylos 2.6.0 baseline: "+name);
            using(var m=new MemoryStream()){s.CopyTo(m);return m.ToArray();}
        }
    }
    internal Engine(string directory):this(directory,Path.Combine(Environment.GetEnvironmentVariable("APPDATA")??Environment.GetFolderPath(Environment.SpecialFolder.ApplicationData),@"CELSYS\RETAS STUDIO\Stylos\2.0\Preference\shortcut.rac")){}
    internal Engine(string directory,string shortcutProfile){
        Dir=Path.GetFullPath(directory);Store=Path.Combine(Dir,".stylos-plugins");profile=Path.GetFullPath(shortcutProfile);
        if(!File.Exists(Path.Combine(Dir,ExeName)))throw new FileNotFoundException("请选择 Stylos.exe 所在目录。");
    }
    static bool DigestName(string s){return s!=null && s.Length==64 && s.All(c=>(c>='0'&&c<='9')||(c>='a'&&c<='f'));}
    static string PackageKey(string hash){if(!DigestName(hash))throw new InvalidDataException("包摘要无效。");return ".stylos-plugins/packages/"+hash+".zip";}
    string Resolve(string key){
        if(key=="@profile")return profile;
        string full=Dir;
        foreach(string part in key.Split('/')){
            if(part!=".stylos-plugins" && !Packages.Leaf(part))throw new InvalidDataException("无效路径："+key);
            full=Path.Combine(full,part);
            if((Directory.Exists(full)||File.Exists(full)) && (File.GetAttributes(full)&FileAttributes.ReparsePoint)!=0)
                throw new IOException("无法管理重解析路径："+full);
        }
        return full;
    }
    static byte[] Bytes(string file){return File.Exists(file)?File.ReadAllBytes(file):null;}
    static string Digest(string file){return Hash(Bytes(file));}
    static void Atomic(string file,byte[] value){
        if(value==null){if(File.Exists(file))File.Delete(file);return;}
        string parent=Path.GetDirectoryName(file);Directory.CreateDirectory(parent);
        string tmp=Path.Combine(parent,Guid.NewGuid().ToString("N")+".tmp");
        try{
            using(var s=new FileStream(tmp,FileMode.CreateNew,FileAccess.Write,FileShare.None)){s.Write(value,0,value.Length);s.Flush(true);}
            if(File.Exists(file))File.Replace(tmp,file,null);else File.Move(tmp,file);
        }finally{if(File.Exists(tmp))File.Delete(tmp);}
    }
    [DllImport("kernel32.dll")]
    static extern IntPtr OpenProcess(uint access,bool inherit,int pid);
    [DllImport("kernel32.dll",CharSet=CharSet.Unicode)]
    static extern bool QueryFullProcessImageName(IntPtr handle,uint flags,StringBuilder path,ref int length);
    [DllImport("kernel32.dll")]
    static extern bool CloseHandle(IntPtr handle);
    public bool Running(){
        foreach(var p in Process.GetProcesses())using(p){
            try{
                if(!p.ProcessName.StartsWith("Stylos",StringComparison.OrdinalIgnoreCase) ||
                   p.ProcessName.Equals("StylosPluginManager",StringComparison.OrdinalIgnoreCase))continue;
                // A 32-bit manager must also recognize a 64-bit renderer.
                IntPtr handle=OpenProcess(0x1000,false,p.Id);
                if(handle==IntPtr.Zero)continue;
                try{
                    int length=32768;var path=new StringBuilder(length);
                    if(QueryFullProcessImageName(handle,0,path,ref length) &&
                       String.Equals(Path.GetDirectoryName(path.ToString()),Dir,StringComparison.OrdinalIgnoreCase))return true;
                }finally{CloseHandle(handle);}
            }
            catch(System.ComponentModel.Win32Exception){}catch(InvalidOperationException){}
        }
        return false;
    }
    FileStream Lock(){Resolve(".stylos-plugins/apply.lock");Directory.CreateDirectory(Store);
        return new FileStream(Path.Combine(Store,"apply.lock"),FileMode.OpenOrCreate,FileAccess.ReadWrite,FileShare.None);}
    string Journal{get{return Resolve(".stylos-plugins/transaction-v2.json");}}
    string Saved(string hash){if(!DigestName(hash))throw new InvalidDataException("备份摘要无效。");return Resolve(".stylos-plugins/backups/"+hash);}
    void Restore(){
        if(!File.Exists(Journal))return;
        foreach(var c in Json.Deserialize<List<Change>>(File.ReadAllText(Journal)).AsEnumerable().Reverse()){
            string file=Resolve(c.key),now=Digest(file);
            if(now==c.prior)continue;
            if(now!=c.next)throw new IOException("恢复期间文件已变化："+file);
            byte[] prior=c.prior=="-"?null:File.ReadAllBytes(Saved(c.prior));
            if(Hash(prior)!=c.prior)throw new IOException("备份校验失败："+file);
            Atomic(file,prior);
        }
        File.Delete(Journal);
    }
    internal void Recover(){if(File.Exists(Journal))using(var guard=Lock()){
        if(Running())throw new IOException("请退出 Stylos 后恢复未完成的事务。");Restore();}}
    static byte[] Encode(object x){return Encoding.UTF8.GetBytes(Json.Serialize(x));}
    void CommitFiles(Dictionary<string,byte[]> desired){
        var changes=new List<Change>();
        foreach(var kv in desired){
            if(kv.Value==null)continue;
            string file=Resolve(kv.Key);
            if(Directory.Exists(file))throw new IOException("文件位置已有目录："+kv.Key);
            string ancestor=kv.Key;
            int slash;
            while((slash=ancestor.LastIndexOf('/'))>=0){
                ancestor=ancestor.Substring(0,slash);
                if(desired.ContainsKey(ancestor) || File.Exists(Resolve(ancestor)))
                    throw new IOException("父目录位置被文件占用："+ancestor);
            }
        }
        foreach(var kv in desired){
            string file=Resolve(kv.Key);byte[] old=Bytes(file);
            string prior=Hash(old),next=Hash(kv.Value);if(prior==next)continue;
            if(old!=null){
                string bak=Saved(prior);if(!File.Exists(bak))Atomic(bak,old);
                if(Digest(bak)!=prior)throw new IOException("备份损坏："+bak);
            }
            changes.Add(new Change{key=kv.Key,prior=prior,next=next});
        }
        Atomic(Journal,Encode(changes));
        try{
            foreach(var c in changes){
                string file=Resolve(c.key);
                if(Digest(file)!=c.prior)throw new IOException("文件已变化："+file);
                Atomic(file,desired[c.key]);
                if(Digest(file)!=c.next)throw new IOException("写入校验失败："+file);
            }
            File.Delete(Journal);
        }catch{Restore();throw;}
    }
    State Current(){
        string path=Resolve(StateName);
        if(File.Exists(path)){
            var s=Json.Deserialize<State>(File.ReadAllText(path));
            if(s.schema!=2 || !DigestName(s.basis))throw new IOException("安装状态无效。");
            return s;
        }
        if(File.Exists(Resolve(".stylos-plugins/state.json")))throw new IOException("检测到旧版安装。请从解压目录打开新版管理器选择该宿主，或使用 --migrate 并提供对应的完整插件包。");
        string basis=Digest(Resolve(ExeName));
        if(!assembly.GetManifestResourceNames().Contains("bootstrap."+basis))throw new IOException("不支持此 Stylos 基线。");
        return new State{basis=basis};
    }
    public Selection Read(){
        var state=Current();var s=new Selection{revision=Digest(Resolve(StateName))};
        foreach(var row in state.plugins){
            var p=Packages.Open(File.ReadAllBytes(Resolve(PackageKey(row.hash))));
            if(p.hash!=row.hash || p.id!=row.id)throw new InvalidDataException("已安装包校验失败："+row.id);
            s.plugins.Add(p.id,p);if(row.enabled)s.enabled.Add(p.id);
        }
        return s;
    }
    static bool Conflict(Plugin a,Plugin b){return a.id!=b.id &&
        (a.conflicts.Contains(b.id)||b.conflicts.Contains(a.id)||(a.exclusive!=null && a.exclusive==b.exclusive));}
    public void Edit(Selection s,Plugin p,string op){
        if(op=="uninstall"){s.plugins.Remove(p.id);s.enabled.Remove(p.id);return;}
        if(op=="disable"){s.enabled.Remove(p.id);return;}
        if(op=="install"){
            bool keepDisabled=s.plugins.ContainsKey(p.id) && !s.enabled.Contains(p.id);
            s.plugins[p.id]=p;
            if(keepDisabled)return;
        }
        else if(op!="enable" || !s.plugins.ContainsKey(p.id))throw new InvalidDataException("未知插件操作。");
        s.enabled.RemoveWhere(id=>Conflict(s.plugins[id],p));s.enabled.Add(p.id);
    }
    static string ModulePath(Plugin p){return ".stylos-plugins/active/"+p.id+"/module.dll";}
    static void RootFile(Dictionary<string,byte[]> map,string name,byte[] bytes){
        if(!Packages.Leaf(name) || new[]{ExeName,BridgeName,ManagerName}.Contains(name,StringComparer.OrdinalIgnoreCase))
            throw new InvalidDataException("受保护的宿主文件："+name);
        if(map.ContainsKey(name))throw new InvalidDataException("插件文件插入冲突："+name);
        map.Add(name,bytes);
    }
    byte[] Original(State state){string file=Resolve(ExeName);if(Digest(file)!=state.basis)file=Saved(state.basis);
        byte[] bytes=File.ReadAllBytes(file);if(Hash(bytes)!=state.basis)throw new IOException("宿主基线校验失败。");return bytes;}
    void ApplyLocked(Selection s,State old,bool setup){
        if(s.revision!=Digest(Resolve(StateName)))throw new IOException("安装状态已改变，请刷新。");
        var active=s.enabled.Select(id=>s.plugins[id]).OrderBy(p=>p.order).ThenBy(p=>p.id,StringComparer.Ordinal).ToList();
        if(active.Any(p=>active.Any(q=>Conflict(p,q))))throw new InvalidDataException("插件启用冲突。");
        var files=new Dictionary<string,byte[]>(StringComparer.OrdinalIgnoreCase);
        var workers=new Dictionary<string,byte[]>(StringComparer.OrdinalIgnoreCase);
        byte[] basis=Original(old),boot=Images.Delta(basis,Resource("bootstrap."+old.basis));
        var allocations=new Dictionary<string,PatchRegion[]>(StringComparer.OrdinalIgnoreCase);
        var adapter=Json.Deserialize<Layout>(Encoding.UTF8.GetString(Resource("adapter")));
        files[ExeName]=Images.Compose(basis,boot,adapter,active,workers,allocations);
        foreach(var kv in workers)RootFile(files,kv.Key,kv.Value);
        files[BridgeName]=Resource("bridge");
        var ini=new StringBuilder();var modules=active.Where(p=>p.module!=null).ToList();
        ini.AppendLine("[Modules]").AppendLine("Count="+modules.Count);
        for(int i=0;i<modules.Count;i++){
            var p=modules[i];string path=ModulePath(p);
            if(Resolve(path).Length>=260)throw new PathTooLongException("Stylos 模块路径过长："+p.id);
            files.Add(path,p.members[p.module]);
            ini.AppendLine("[Module"+i+"]").AppendLine("Id="+p.id).AppendLine("Title="+(p.settings??p.name))
                .AppendLine("Page="+p.legacyPage);
        }
        foreach(var p in active){
            foreach(var n in p.files)RootFile(files,n,p.members["files/"+n]);
            foreach(var n in p.assets)files.Add(".stylos-plugins/active/"+p.id+"/"+n,p.members["payload/"+n]);
        }
        files[".stylos-plugins/modules.ini"]=Encoding.Unicode.GetPreamble().Concat(Encoding.Unicode.GetBytes(ini.ToString())).ToArray();
        files[".stylos-plugins/layout.json"]=Encode(new {host=Hash(files[ExeName]),images=allocations});
        foreach(var p in s.plugins.Values)files.Add(PackageKey(p.hash),p.archive);
        var next=new State{basis=old.basis,managerShortcut=old.managerShortcut,
            plugins=s.plugins.Values.OrderBy(p=>p.id,StringComparer.Ordinal).Select(p=>new Installed{id=p.id,hash=p.hash,enabled=s.enabled.Contains(p.id)}).ToArray()};
        foreach(var kv in files)next.managed.Add(kv.Key,Hash(kv.Value));
        foreach(var kv in old.managed){
            if(Digest(Resolve(kv.Key))!=kv.Value)throw new IOException("已管理文件发生外部修改："+kv.Key);
            if(!files.ContainsKey(kv.Key))files.Add(kv.Key,null);
        }
        foreach(var kv in next.managed){
            if(kv.Key!=ExeName && !old.managed.ContainsKey(kv.Key) && File.Exists(Resolve(kv.Key)))
                throw new IOException("该文件已有其他所有者："+kv.Key);
        }
        if(setup){
            byte[] me=File.ReadAllBytes(assembly.Location);
            if(File.Exists(Resolve(ManagerName)) && Digest(Resolve(ManagerName))!=Hash(me))OwnedManager();
            files[ManagerName]=me;files[".stylos-plugins/manager.sha256"]=Encoding.ASCII.GetBytes(Hash(me));
        }
        if(File.Exists(profile)){
            char key;byte[] prior=Bytes(profile),bound=ShortcutBinding.Bind(prior,out key,"EDIT_PLUGIN_MANAGER",3,"PJM");
            if(!Object.ReferenceEquals(prior,bound))next.managerShortcut=key;
            foreach(var p in active)foreach(var b in p.shortcuts??new Shortcut[0])
                bound=ShortcutBinding.Bind(bound,out key,b.command,b.modifiers,b.candidates);
            files["@profile"]=bound;
        }
        files[StateName]=Encode(next);CommitFiles(files);s.revision=Digest(Resolve(StateName));
    }
    public void Apply(Selection s,bool setup=false){
        using(var guard=Lock()){
            if(Running())throw new IOException("请先退出此目录的 Stylos。");
            Restore();
            ApplyLocked(s,Current(),setup || !File.Exists(Resolve(StateName)));
        }
    }
    internal bool BridgeUpdatePending(){var state=Current();string hash;
        return state.managed.TryGetValue(BridgeName,out hash) && hash!=Hash(Resource("bridge"));}
    internal void UpgradeBridge(){Apply(Read());}
}
