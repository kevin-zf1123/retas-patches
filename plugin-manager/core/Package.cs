using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Collections.Generic;
using System.Security.Cryptography;
using System.Text;
using System.Text.RegularExpressions;
using System.Web.Script.Serialization;

internal sealed class Shortcut {public string command,candidates;public int modifiers;}
internal sealed class Plugin {
    public int schema, order, legacyPage;
    public string id,name,version,host,module,settings,exclusive;
    public string[] files,assets,conflicts;
    public Shortcut[] shortcuts;
    [ScriptIgnore] public string hash;
    [ScriptIgnore] public byte[] archive;
    [ScriptIgnore] public Dictionary<string,byte[]> members;
}
internal sealed class Installed {
    public string id,hash;
    public bool enabled;
}
internal sealed class PackageSelection {
    public readonly List<Plugin> plugins=new List<Plugin>();
    public readonly List<string> skipped=new List<string>();
    public string managerUpdate;
    public string Notice {get{return skipped.Count==0?"":"已跳过："+String.Join("、",skipped)+"。";}}
}
internal sealed class Selection {
    public Dictionary<string,Plugin> plugins=new Dictionary<string,Plugin>(StringComparer.Ordinal);
    public HashSet<string> enabled=new HashSet<string>(StringComparer.Ordinal);
    public string revision;
    public Selection Duplicate(){return new Selection{plugins=new Dictionary<string,Plugin>(plugins),enabled=new HashSet<string>(enabled),revision=revision};}
    public bool Same(Selection b){return enabled.SetEquals(b.enabled) && plugins.Count==b.plugins.Count &&
        plugins.All(x=>b.plugins.ContainsKey(x.Key) && b.plugins[x.Key].hash==x.Value.hash);}
    public string Label(Plugin p){return plugins.ContainsKey(p.id)?plugins[p.id].version:"—";}
}
internal static class Packages {
    internal static PackageSelection FromFiles(string[] paths,bool allowManagerUpdate){
        var result=new PackageSelection();
        foreach(string path in paths){
            string name=Path.GetFileName(path);
            try{
                if(new FileInfo(path).Length>64*1024*1024)throw new InvalidDataException("插件包过大。");
                byte[] bytes=File.ReadAllBytes(path);
                using(var stream=new MemoryStream(bytes))using(var zip=new ZipArchive(stream,ZipArchiveMode.Read)){
                    if(zip.GetEntry("plugin.json")==null){
                        if(zip.GetEntry("update.json")!=null && zip.GetEntry("StylosPluginManager.exe")!=null){
                            if(allowManagerUpdate && paths.Length==1)result.managerUpdate=path;
                            else result.skipped.Add(name+"（管理器）");
                            continue;
                        }
                        if(zip.GetEntry("plugin.h")!=null && zip.GetEntry("retas.h")!=null && zip.GetEntry("pack.py")!=null){
                            result.skipped.Add(name+"（开发工具包）");
                            continue;
                        }
                        if(zip.Entries.Count>0 && zip.Entries.All(e=>e.FullName.EndsWith(".zip",StringComparison.OrdinalIgnoreCase)))
                            throw new InvalidDataException("这是合集。请解压，再选择其中的功能插件 ZIP。");
                    }
                }
                result.plugins.Add(Open(bytes));
            }catch(InvalidDataException e){throw new InvalidDataException(name+"："+e.Message,e);}
        }
        if(paths.Length!=0 && result.plugins.Count==0 && result.managerUpdate==null)
            throw new InvalidDataException(result.Notice+"请选择要安装的功能插件 ZIP。");
        return result;
    }
    internal static readonly JavaScriptSerializer Json=new JavaScriptSerializer{MaxJsonLength=32*1024*1024};
    internal static string Hash(byte[] b){if(b==null)return "-";using(var sha=SHA256.Create())return BitConverter.ToString(sha.ComputeHash(b)).Replace("-","").ToLowerInvariant();}
    internal static bool Leaf(string s){
        return !String.IsNullOrWhiteSpace(s) && s.Length<=128 && s[0]!='.' && s[0]!='@' &&
            s.IndexOfAny(Path.GetInvalidFileNameChars())<0 && !s.EndsWith(".") && !s.EndsWith(" ") &&
            !Regex.IsMatch(s,@"^(con|prn|aux|nul|com[0-9¹²³]|lpt[0-9¹²³])(\.|$)",RegexOptions.IgnoreCase);
    }
    internal static bool Id(string s){return Leaf(s) && Regex.IsMatch(s,@"\A[a-z][a-z0-9-]{0,63}\z");}
    static bool HasEntry(byte[] dll,int nt){
        int opt=nt+24;
        if(opt+104>dll.Length)return false;
        int table=opt+BitConverter.ToUInt16(dll,nt+20),sections=BitConverter.ToUInt16(dll,nt+6);
        if(table+40L*sections>dll.Length)return false;
        Func<uint,int,int> offset=(rva,size)=>{
            for(int i=0;i<sections;i++){
                int h=table+40*i;
                uint start=BitConverter.ToUInt32(dll,h+12),length=BitConverter.ToUInt32(dll,h+16);
                long pos=(long)BitConverter.ToUInt32(dll,h+20)+rva-start;
                if(rva>=start && (long)rva+size<=(long)start+length && pos>=0 && pos+size<=dll.Length)return (int)pos;
            }
            return -1;
        };
        int exp=offset(BitConverter.ToUInt32(dll,opt+96),40);
        if(exp<0)return false;
        uint count=BitConverter.ToUInt32(dll,exp+24);
        if(count>dll.Length/4)return false;
        int names=offset(BitConverter.ToUInt32(dll,exp+32),(int)count*4);
        if(names<0)return false;
        byte[] symbol=Encoding.ASCII.GetBytes("RetasPluginInit\0");
        for(int i=0;i<count;i++){
            int pos=offset(BitConverter.ToUInt32(dll,names+4*i),symbol.Length);
            if(pos>=0 && symbol.SequenceEqual(new ArraySegment<byte>(dll,pos,symbol.Length)))return true;
        }
        return false;
    }
    internal static byte[] Member(ZipArchiveEntry entry,int limit){
        if(entry==null || entry.Length>limit)throw new InvalidDataException("包成员缺失或过大。");
        using(var stream=entry.Open())using(var r=new BinaryReader(stream)){
            byte[] data=r.ReadBytes((int)entry.Length);
            if(data.Length!=entry.Length || stream.ReadByte()!=-1)throw new InvalidDataException("包成员长度错误。");
            return data;
        }
    }
    internal static Plugin Open(byte[] bytes){
        if(bytes==null || bytes.Length>64*1024*1024)throw new InvalidDataException("插件包过大。");
        var members=new Dictionary<string,byte[]>(StringComparer.OrdinalIgnoreCase);
        using(var mem=new MemoryStream(bytes))using(var zip=new ZipArchive(mem,ZipArchiveMode.Read)){
            long total=0;
            foreach(var e in zip.Entries){
                if(e.FullName.EndsWith("/") && e.Length==0)continue;
                if(!e.FullName.Split('/').All(Leaf) || members.ContainsKey(e.FullName) ||
                    (total+=e.Length)>96*1024*1024 || members.Count>=4096)
                    throw new InvalidDataException("插件包的文件名、大小或重复条目无效。");
                members.Add(e.FullName,Member(e,64*1024*1024));
            }
        }
        byte[] meta;
        if(!members.TryGetValue("plugin.json",out meta) || meta.Length>65536)throw new InvalidDataException("缺少 plugin.json。");
        var p=Json.Deserialize<Plugin>(Encoding.UTF8.GetString(meta));
        if(p==null || p.schema!=3 || p.host!="stylos-2.6.0-x86")throw new InvalidDataException("请选择包含实现的 DevKit v2 插件包（清单 schema 3）。");
        if(!Id(p.id) || String.IsNullOrWhiteSpace(p.name) || p.name.Length>120 || p.name.Any(Char.IsControl) ||
           String.IsNullOrEmpty(p.version) || !Regex.IsMatch(p.version,@"\A\d+\.\d+\.\d+\z") ||
           (p.settings!=null && (p.settings.Length>120 || p.settings.Any(Char.IsControl))) ||
           (p.exclusive!=null && !Id(p.exclusive)))throw new InvalidDataException("插件标识、名称或版本无效。");
        if(p.module!=null){
            byte[] dll;
            if(!members.TryGetValue(p.module,out dll) || !Leaf(p.module) || !p.module.EndsWith(".dll",StringComparison.OrdinalIgnoreCase))
                throw new InvalidDataException("缺少声明的 x86 插件 DLL。");
            int nt=dll.Length>=64?BitConverter.ToInt32(dll,60):-1;
            if(nt<0 || nt>dll.Length-26 || BitConverter.ToUInt16(dll,0)!=0x5a4d ||
               BitConverter.ToUInt32(dll,nt)!=0x4550 || BitConverter.ToUInt16(dll,nt+4)!=0x14c ||
               BitConverter.ToUInt16(dll,nt+24)!=0x10b || (BitConverter.ToUInt16(dll,nt+22)&0x2000)==0)
                throw new InvalidDataException("插件模块必须为 Windows x86 DLL。");
            if(!HasEntry(dll,nt))throw new InvalidDataException("插件 DLL 缺少 RetasPluginInit 导出。");
        }
        p.files=p.files??new string[0];p.assets=p.assets??new string[0];p.conflicts=p.conflicts??new string[0];
        foreach(string asset in p.assets)
            if(asset==null || !asset.Split('/').All(Leaf) || asset.Equals("module.dll",StringComparison.OrdinalIgnoreCase) ||
               !members.ContainsKey("payload/"+asset))throw new InvalidDataException("插件私有文件声明无效。");
        foreach(var s in p.shortcuts??new Shortcut[0])
            if(s==null || s.command==null || !Regex.IsMatch(s.command,@"\A[A-Z0-9_]{1,96}\z") ||
                s.candidates==null || !Regex.IsMatch(s.candidates,@"\A[A-Z0-9]{1,36}\z") ||
                s.modifiers<0 || s.modifiers>7)throw new InvalidDataException("快捷键声明无效。");
        if(!p.files.All(n=>Leaf(n) && members.ContainsKey("files/"+n)) || !p.conflicts.All(Id))
            throw new InvalidDataException("插件文件或冲突声明无效。");
        if(p.module==null && p.files.Length==0 && !members.ContainsKey("layout.json"))
            throw new InvalidDataException("空插件包没有模块、文件或插入位置。");
        p.hash=Hash(bytes);p.archive=bytes;p.members=members;return p;
    }
}
