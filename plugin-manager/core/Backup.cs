using System;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;
using System.Collections.Generic;

internal sealed class BackupSet {public string format,scope;public Installed[] plugins;}
internal sealed partial class Engine {
    static byte[] ZipMember(ZipArchive zip,string name,int limit){return Packages.Member(zip.Entries.SingleOrDefault(e=>e.FullName==name),limit);}
    static void AddMember(ZipArchive zip,string name,byte[] bytes){
        using(var s=zip.CreateEntry(name,CompressionLevel.Optimal).Open())s.Write(bytes,0,bytes.Length);
    }
    byte[] MakeBackup(string only){
        var s=Read();
        if(only!=null && !s.plugins.ContainsKey(only))throw new InvalidDataException("插件未安装："+only);
        var items=s.plugins.Values.Where(p=>only==null || only==p.id).OrderBy(p=>p.id).ToArray();
        using(var buf=new MemoryStream()){
            using(var zip=new ZipArchive(buf,ZipArchiveMode.Create,true)){
                foreach(var p in items)AddMember(zip,"packages/"+p.hash+".zip",p.archive);
                AddMember(zip,"backup.json",Encode(new BackupSet{format="retas.packages/2",scope=only==null?"all":"plugin",
                    plugins=items.Select(p=>new Installed{id=p.id,enabled=s.enabled.Contains(p.id),hash=p.hash}).ToArray()}));
            }
            return buf.ToArray();
        }
    }
    public void Backup(string path){BackupPlugin(path,null);}
    public void BackupPlugin(string path,string id){
        string full=Path.GetFullPath(path);
        if(!full.EndsWith(".zip",StringComparison.OrdinalIgnoreCase) || full.StartsWith(Store+"\\",StringComparison.OrdinalIgnoreCase))
            throw new IOException("请选择状态目录之外的 ZIP 文件。");
        using(var guard=Lock()){if(File.Exists(Journal))throw new IOException("请先恢复事务。");Atomic(full,MakeBackup(id));}
    }
    public void RestoreBackup(string path){
        var s=Read();
        using(var file=File.OpenRead(path))using(var zip=new ZipArchive(file,ZipArchiveMode.Read)){
            var manifest=Json.Deserialize<BackupSet>(Encoding.UTF8.GetString(ZipMember(zip,"backup.json",1024*1024)));
            if(manifest.format!="retas.packages/2" || manifest.plugins==null ||
               (manifest.scope!="all" && !(manifest.scope=="plugin" && manifest.plugins.Length==1)))
                throw new InvalidDataException("备份格式无效。");
            if(manifest.scope=="all"){s.plugins.Clear();s.enabled.Clear();}
            var seen=new HashSet<string>();
            foreach(var row in manifest.plugins){
                if(!DigestName(row.hash) || !seen.Add(row.id))throw new InvalidDataException("备份清单无效。");
                var p=Packages.Open(ZipMember(zip,"packages/"+row.hash+".zip",64*1024*1024));
                if(p.hash!=row.hash || p.id!=row.id)throw new InvalidDataException("备份校验失败。");
                if(manifest.scope=="plugin" && row.enabled){s.plugins[p.id]=p;Edit(s,p,"enable");}
                else {s.plugins[p.id]=p;if(row.enabled)s.enabled.Add(p.id);else s.enabled.Remove(p.id);}
            }
        }
        Apply(s);
    }
}
