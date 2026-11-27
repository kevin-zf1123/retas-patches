using System;
using System.IO;
using System.Linq;
using System.Text;
using System.Collections.Generic;
using Retas.DevKit;

internal sealed class DeltaImage {public string image,delta;}
internal sealed class Claim {public int rva,size;}
internal sealed class FixedEdit {public int rva;public string expected, data;}
internal sealed class Section {public string name,data;public int virtualSize;public uint flags;}
internal sealed class Relocation {public string region,target,relativeTarget;public int address,addend;}
internal sealed class Variant {public DeltaImage[] images;}
internal sealed class Layout {
    public Dictionary<string,Variant> variants;
    public Claim[] claims;
    public Claim[] workerClaims;
    public string[] ownedSections;
    public FixedEdit[] edits;
    public Section[] sections;
    public Relocation[] relocations;
}
internal static class Images {
    // Legacy linked adapters can move the import table when adding a section.
    // Check each original section by RVA, independent of its new file offset.
    static void ValidateLinked(byte[] before,byte[] after,Layout layout,Layout adapter,bool worker,IEnumerable<Claim> inherited){
        var original=new PatchMap(before);
        var linked=new PatchMap(after);
        var own=new HashSet<string>(layout.ownedSections??new string[0],StringComparer.Ordinal);
        var imports=new HashSet<string>(worker?new string[0]:adapter.ownedSections,StringComparer.Ordinal);
        original.ValidateLinkedHeaders(linked,imports);
        var claims=(layout.claims??new Claim[0]).Concat(worker?(layout.workerClaims??new Claim[0]):adapter.claims).Concat(inherited)
            .OrderBy(c=>c.rva).ToArray();
        var prior=original.ImageSections.ToDictionary(s=>s.region,StringComparer.Ordinal);
        foreach(var section in linked.ImageSections){
            PatchRegion old;
            if(!prior.TryGetValue(section.region,out old)){
                if(!own.Contains(section.region))throw new InvalidDataException("未声明新增节 "+section.region);
                continue;
            }
            prior.Remove(section.region);
            if(imports.Contains(section.region))continue;
            if(section.rva!=old.rva || section.size!=old.size)throw new InvalidDataException("外部节布局已改变："+section.region);
            int pos=0;
            foreach(var claim in claims){
                int lo=claim.rva-section.rva;
                if(lo<0 || lo>=section.size)continue;
                if(claim.size<=0 || (long)lo+claim.size>section.size)throw new InvalidDataException("固定位置越界。");
                for(;pos<lo;pos++)if(before[old.fileOffset+pos]!=after[section.fileOffset+pos])
                    throw new InvalidDataException("未声明的固定位置："+(old.rva+pos).ToString("x"));
                pos=Math.Max(pos,lo+claim.size);
            }
            for(;pos<old.size;pos++)if(before[old.fileOffset+pos]!=after[section.fileOffset+pos])
                throw new InvalidDataException("未声明的固定位置："+(old.rva+pos).ToString("x"));
        }
        if(prior.Count!=0)throw new InvalidDataException("插件移除了既有节。");
    }
    internal static byte[] Delta(byte[] basis,byte[] patch){
        using(var r=new BinaryReader(new MemoryStream(patch))){
            string from=BitConverter.ToString(r.ReadBytes(32)).Replace("-","").ToLowerInvariant();
            string to=BitConverter.ToString(r.ReadBytes(32)).Replace("-","").ToLowerInvariant();
            if(Packages.Hash(basis)!=from)throw new InvalidDataException("宿主版本与插入声明不匹配。");
            int length=r.ReadInt32(),count=r.ReadInt32();
            if(length<=0 || length>128*1024*1024 || count<0 || count>patch.Length/8)throw new InvalidDataException("补丁长度无效。");
            byte[] result=new byte[length];Buffer.BlockCopy(basis,0,result,0,Math.Min(length,basis.Length));
            int last=0;
            for(int i=0;i<count;i++){
                int pos=r.ReadInt32(),n=r.ReadInt32();
                if(pos<last || n<=0 || (long)pos+n>length || r.Read(result,pos,n)!=n)throw new InvalidDataException("补丁范围无效。");
                last=pos+n;
            }
            if(r.BaseStream.Position!=r.BaseStream.Length || Packages.Hash(result)!=to)throw new InvalidDataException("补丁结果校验失败。");
            return result;
        }
    }
    internal static byte[] Compose(byte[] pristine,byte[] bootstrap,Layout host,List<Plugin> active,Dictionary<string,byte[]> workers,Dictionary<string,PatchRegion[]> allocations){
        var layouts=new List<KeyValuePair<Plugin,Layout>>();
        byte[] main=bootstrap;
        foreach(var p in active){
            byte[] json;if(!p.members.TryGetValue("layout.json",out json))continue;
            var layout=Packages.Json.Deserialize<Layout>(Encoding.UTF8.GetString(json));
            layouts.Add(new KeyValuePair<Plugin,Layout>(p,layout));
            if(layout.variants==null)continue;
            Variant v;
            if(!layout.variants.TryGetValue(Packages.Hash(main),out v))throw new InvalidDataException("当前宿主组合不支持 "+p.name);
            foreach(var image in v.images){
                if(image.image=="Stylos.exe"){
                    byte[] next=Delta(main,p.members[image.delta]);
                    ValidateLinked(main,next,layout,host,false,new Claim[0]);main=next;
                }
                else{
                    if(!Packages.Leaf(image.image) || !image.image.EndsWith(".exe",StringComparison.OrdinalIgnoreCase))
                        throw new InvalidDataException("无效的工作进程名称。");
                    byte[] worker=Delta(pristine,p.members[image.delta]);
                    var inherited=layouts.Where(pair=>pair.Key!=p).SelectMany(pair=>pair.Value.claims??new Claim[0]);
                    ValidateLinked(pristine,worker,layout,host,true,inherited);
                    var workerMap=new PatchMap(worker);
                    foreach(var pair in layouts){
                        foreach(var c in pair.Value.claims??new Claim[0])workerMap.Reserve(pair.Key.id,c.rva,c.size);
                        foreach(var name in pair.Value.ownedSections??new string[0])workerMap.ClaimSection(pair.Key.id,name);
                    }
                    foreach(var c in layout.workerClaims??new Claim[0])workerMap.Reserve(p.id,c.rva,c.size);
                    allocations.Add(image.image,workerMap.Regions);
                    workers.Add(image.image,worker);
                }
            }
        }
        var map=new PatchMap(main);
        foreach(var c in host.claims)map.Reserve("@host",c.rva,c.size);
        foreach(string section in host.ownedSections)map.ClaimSection("@host",section);
        var writes=new List<KeyValuePair<PatchRegion,byte[]>>();
        var headers=new List<PatchSectionHeader>();
        foreach(var pair in layouts){
            var p=pair.Key;var l=pair.Value;
            foreach(var c in l.claims??new Claim[0])map.Reserve(p.id,c.rva,c.size);
            foreach(var name in l.ownedSections??new string[0])map.ClaimSection(p.id,name);
            foreach(var e in l.edits??new FixedEdit[0]){
                byte[] data=p.members[e.data],expected=p.members[e.expected];
                map.Reserve(p.id,e.rva,data.Length);
                var slot=map.Resolve(p.id,"host",e.rva,data.Length);
                if(expected.Length!=data.Length || !expected.SequenceEqual(main.Skip(slot.fileOffset).Take(data.Length)))
                    throw new InvalidDataException(p.id+" 的插入位置前置字节不匹配。");
                writes.Add(new KeyValuePair<PatchRegion,byte[]>(slot,data));
            }
            foreach(var s in l.sections??new Section[0]){
                byte[] data=p.members[s.data];
                var slot=map.Allocate(p.id,s.name,s.virtualSize,data.Length);
                headers.Add(new PatchSectionHeader{name=s.name,virtualSize=s.virtualSize,flags=s.flags});
                writes.Add(new KeyValuePair<PatchRegion,byte[]>(slot,data));
            }
        }
        byte[] result=new byte[map.Length];main.CopyTo(result,0);
        if(headers.Count!=0)map.SetHeaders(result,headers);
        foreach(var w in writes)w.Value.CopyTo(result,w.Key.fileOffset);
        foreach(var pair in layouts)foreach(var f in pair.Value.relocations??new Relocation[0]){
            var dst=map.Resolve(pair.Key.id,f.region,f.address,4);
            if((f.target==null)==(f.relativeTarget==null))throw new InvalidDataException("重定位必须指定一个目标区域。");
            var target=map.Resolve(pair.Key.id,f.relativeTarget??f.target,f.addend,1);
            int value=f.relativeTarget==null?target.rva:checked(target.rva-dst.rva-4);
            BitConverter.GetBytes(value).CopyTo(result,dst.fileOffset);
        }
        allocations.Add("Stylos.exe",map.Regions);return result;
    }
}
