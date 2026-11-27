using System;
using System.Collections;
using System.Collections.Generic;
using System.Diagnostics;
using System.IO;
using System.Linq;
using System.Reflection;

// This adapter only reads the user's installed 0.1 manager and its actual
// state. No list of historical plugin IDs or payloads is shipped in v2.
internal sealed partial class Engine {
    internal bool HasLegacy {get{return File.Exists(Resolve(".stylos-plugins/state.json")) && !File.Exists(Resolve(StateName));}}
    static object Field(object target,string name){return target.GetType().GetField(name).GetValue(target);}
    internal void Migrate(IEnumerable<Plugin> replacements){
        var packages=replacements.ToDictionary(p=>p.id);
        using(var guard=Lock()){
            if(Running())throw new IOException("请先退出 Stylos。");
            Restore();
            if(!HasLegacy)throw new IOException("没有可迁移的 0.1 安装。");
            string hash=OwnedManager();
            var version=FileVersionInfo.GetVersionInfo(ManagerPath);
            if(version.FileMajorPart!=0 || version.FileMinorPart!=1)throw new IOException("此迁移入口支持 0.1.x 管理器。");
            byte[] binary=File.ReadAllBytes(ManagerPath);
            if(Hash(binary)!=hash)throw new IOException("旧管理器已改变。");
            Type type=Assembly.Load(binary).GetType("Engine",true);
            object legacy=type.GetConstructor(BindingFlags.Instance|BindingFlags.NonPublic,null,new[]{typeof(string),typeof(string)},null)
                              .Invoke(new object[]{Dir,profile});
            var snapshot=type.GetMethod("Read").Invoke(legacy,null);
            int installed=(int)Field(snapshot,"installed"),enabled=(int)Field(snapshot,"enabled");
            object catalog=type.GetField("Catalog").GetValue(null);
            var hosts=(IDictionary)Field(catalog,"hosts");
            byte[] current=Bytes(Resolve(ExeName));
            object host=hosts[Hash(current)];
            if(host==null)throw new IOException("未知旧版宿主。");
            var old=new State{basis=(string)Field(host,"family"),managerShortcut=(int)Field(snapshot,"managerShortcut")};
            // Reject missing replacements before changing any managed file.
            var selection=new Selection{revision="-"};
            foreach(var entry in (IEnumerable)Field(catalog,"plugins")){
                int bit=(int)Field(entry,"bit");string id=(string)Field(entry,"id");
                if((installed&bit)==0)continue;
                Plugin p;
                if(!packages.TryGetValue(id,out p))throw new IOException("需要提供完整新版组件："+id);
                selection.plugins.Add(id,p);if((enabled&bit)!=0)selection.enabled.Add(id);
                string prior=".stylos-plugins/packages/"+id+".zip";
                if(File.Exists(Resolve(prior)))old.managed[prior]=Digest(Resolve(prior));
            }
            foreach(string key in new[]{ExeName,".stylos-plugins/state.json",".stylos-plugins/input.ini",".stylos-plugins/layout.json"})
                if(File.Exists(Resolve(key)))old.managed[key]=Digest(Resolve(key));
            foreach(DictionaryEntry entry in (IDictionary)Field(catalog,"known")){
                string name=(string)entry.Key;
                if(!Packages.Leaf(name) || !Path.HasExtension(name))continue;
                string present=Digest(Resolve(name));
                if(((IEnumerable<string>)entry.Value).Contains(present))old.managed[name]=present;
            }
            if(Digest(Saved(old.basis))!=old.basis){
                var delta=type.GetMethod("Delta",BindingFlags.Static|BindingFlags.NonPublic);
                byte[] basis=(byte[])delta.Invoke(null,new object[]{current,old.basis});
                if(Hash(basis)!=old.basis)throw new IOException("旧基线校验失败。");
                Atomic(Saved(old.basis),basis);
            }
            // The legacy backup API locks the v1 store itself. Use its read-only
            // archive builder while this manager owns the shared apply.lock.
            var backup=type.GetMethod("MakeBackup",BindingFlags.NonPublic|BindingFlags.Instance);
            byte[] archive=(byte[])backup.Invoke(legacy,new object[]{null});
            Atomic(Resolve(".stylos-plugins/backups/legacy-"+Hash(archive)+".zip"),archive);
            ApplyLocked(selection,old,true);
        }
    }
}
