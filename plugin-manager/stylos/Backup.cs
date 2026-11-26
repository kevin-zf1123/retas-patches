using System;
using System.Collections.Generic;
using System.IO;
using System.IO.Compression;
using System.Linq;
using System.Text;

internal sealed class BackupItem {
    public string id,source,archive;
    public bool enabled;
}
internal sealed class BackupManifest {
    public string format, manager, scope;
    public List<BackupItem> plugins;
}

internal sealed partial class Engine {
    internal static byte[] ZipMember(ZipArchive zip,string name,int limit){
        var entry=zip.Entries.SingleOrDefault(e=>e.FullName==name);
        if(entry==null || entry.Length<=0 || entry.Length>limit)throw new InvalidDataException("备份或安装包中的文件无效："+name);
        using(var stream=entry.Open())using(var reader=new BinaryReader(stream)){
            var data=reader.ReadBytes((int)entry.Length);
            if(data.Length!=entry.Length || stream.ReadByte()!=-1)throw new InvalidDataException(name);
            return data;
        }
    }
    static void AddMember(ZipArchive zip,string name,byte[] bytes){
        using(var entry=zip.CreateEntry(name,CompressionLevel.Optimal).Open())entry.Write(bytes,0,bytes.Length);
    }
    public void Backup(string destination){BackupPlugin(destination,null);}
    public void BackupPlugin(string destination,string onlyId){
        if(!String.Equals(Path.GetExtension(destination),".zip",StringComparison.OrdinalIgnoreCase))
            throw new IOException("请选择 ZIP 备份文件。");
        string full=Path.GetFullPath(destination);
        if(full.StartsWith(Store.TrimEnd(Path.DirectorySeparatorChar)+Path.DirectorySeparatorChar,StringComparison.OrdinalIgnoreCase))
            throw new IOException("请将导出的备份保存在管理器状态目录之外。");
        using(var guard=Lock()){
            if(File.Exists(Journal))throw new IOException("请先恢复未完成的操作。");
            Atomic(full,MakeBackup(onlyId));
        }
    }
    byte[] MakeBackup(string onlyId=null){
        var selection=Read();
        if(onlyId!=null && !Catalog.plugins.Any(p=>p.id==onlyId && (selection.installed&p.bit)!=0))
            throw new IOException("请选择已安装的插件。");
        var manifest=new BackupManifest{scope=onlyId==null?"all":"plugin",format="retas.stylos.plugins/1",manager=Catalog.managerVersion,plugins=new List<BackupItem>()};
        using(var buffer=new MemoryStream()){
            using(var zip=new ZipArchive(buffer,ZipArchiveMode.Create,true)){
                foreach(var p in Catalog.plugins.Where(p=>(selection.installed&p.bit)!=0 && (onlyId==null || p.id==onlyId))){
                    string source=selection.sources[p.id];
                    byte[] payload=Bytes(paths["package-"+p.id]);
                    if(payload!=null && !MatchesSource(IdentifyPackage(payload),p,source))throw new IOException("缓存组件与安装记录不匹配："+p.name);
                    if(payload==null){
                        // Registered releases have exact embedded archives. Adopted standalone installs have no receipt.
                        string release;
                        bool registered=source!="" && Catalog.packageVersions.TryGetValue(source,out release) && release!="";
                        string bundled=registered?source:(p.id=="motion-check"?p.v0Sha256:p.currentSha256);
                        payload=Asset("pkg."+bundled);
                    }
                    string archive=Hash(payload);
                    AddMember(zip,"components/"+p.id+".zip",payload);
                    manifest.plugins.Add(new BackupItem{id=p.id,source=source,archive=archive,enabled=(selection.enabled&p.bit)!=0});
                }
                AddMember(zip,"backup.json",Encoding.UTF8.GetBytes(Json.Serialize(manifest)));
            }
            return buffer.ToArray();
        }
    }
    public void RestoreBackup(string path){
        var state=Read();
        var seen=new HashSet<string>();
        using(var file=File.OpenRead(path))using(var zip=new ZipArchive(file,ZipArchiveMode.Read)){
            var manifest=Json.Deserialize<BackupManifest>(Encoding.UTF8.GetString(ZipMember(zip,"backup.json",65536)));
            if(manifest==null || manifest.format!="retas.stylos.plugins/1" || manifest.plugins==null ||
                manifest.plugins.Count>Catalog.plugins.Length || zip.Entries.Count!=manifest.plugins.Count+1)
                throw new InvalidDataException("无效的插件备份。");
            bool single=manifest.scope=="plugin";
            if(single && manifest.plugins.Count!=1 || !single && manifest.scope!="all" && manifest.scope!=null)
                throw new InvalidDataException("无效的备份范围。");
            if(!single){state.installed=state.enabled=0;state.sources.Clear();}
            foreach(var item in manifest.plugins){
                var p=Catalog.plugins.SingleOrDefault(x=>x.id==item.id);
                if(p==null || !seen.Add(p.id))throw new InvalidDataException("备份含未登记或重复的插件。");
                var bytes=ZipMember(zip,"components/"+p.id+".zip",32*1024*1024);
                string source=item.source,id;
                var component=IdentifyPackage(bytes);
                if(component.id!=p.id || source==null ||
                    (source!="" && (!Catalog.packages.TryGetValue(source,out id) || id!=p.id)))
                    throw new InvalidDataException("备份中的插件与记录不匹配："+p.name);
                bool exact=MatchesSource(component,p,source);
                if(!exact && source!="" && Catalog.packageVersions[source]!="")
                    throw new InvalidDataException("备份中的组件版本不匹配："+p.name);
                state.installed|=p.bit;
                if(single){state.enabled&=~p.bit;if(item.enabled)state.enabled&=~p.conflicts;}
                if(item.enabled)state.enabled|=p.bit;
                state.sources[p.id]=source;
                if(exact)state.imports[p.id]=bytes;
            }
        }
        Apply(state);
    }
}
