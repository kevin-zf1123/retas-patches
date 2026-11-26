using System;
using System.IO;
using System.Linq;
using System.Collections.Generic;
using System.IO.Compression;
using System.Web.Script.Serialization;
using Retas.DevKit;

internal class Site {
    public string owner {get;set;}
    public string blob {get;set;}
    public int rva {get;set;}
}
internal class Segment : PatchSectionHeader {
    public string owner {get;set;}
    public string blob {get;set;}
}
internal class Fixup {
    public string owner {get;set;}
    public string region {get;set;}
    public int address {get;set;}
    public string @base {get;set;}
    public int count {get;set;}
    public int factor {get;set;}
}
internal class Fragment {
    public Site[] sites {get;set;}
    public Site[] header {get;set;}
    public Segment[] segments {get;set;}
    public Fixup[] fixups {get;set;}
}

internal class Variants {
    public int bit {get;set;}
    public int mask {get;set;}
    public Dictionary<string,string> variants {get;set;}
}

internal class PatchCatalog {
    public int version {get;set;}
    public Dictionary<string,Dictionary<string,Variants>> groups {get;set;}
    public Dictionary<string,Fragment> fragments {get;set;}
    public string[] sectionOrder {get;set;}
    public Dictionary<string,string> blobs {get;set;}

    public static PatchCatalog Open(byte[] packed) {
        using (var gz = new GZipStream(new MemoryStream(packed), CompressionMode.Decompress))
        using (var reader = new StreamReader(gz)) {
            var catalog = new JavaScriptSerializer { MaxJsonLength = 16 * 1024 * 1024 }
                .Deserialize<PatchCatalog>(reader.ReadToEnd());
            if (catalog.version != 1) throw new InvalidDataException("Unknown patch layout version");
            return catalog;
        }
    }

    public byte[] Build(byte[] basis, string image, int enabled, string target, out PatchRegion[] allocations) {
        Dictionary<string,Variants> owners;
        if (!groups.TryGetValue(Engine.Hash(basis) + "/" + image, out owners))
            throw new InvalidDataException("Unsupported Stylos patch layout");
        var active = new List<Fragment>();
        foreach (var owner in owners.OrderBy(p => p.Key, StringComparer.Ordinal)) {
            var choices = owner.Value;
            if (choices.bit != 0 && (choices.bit & enabled) == 0) continue;
            string key;
            if (!choices.variants.TryGetValue((choices.mask & enabled).ToString(), out key))
                throw new InvalidDataException("Unsupported patch combination for " + owner.Key);
            active.Add(fragments[key]);
        }
        var segments = active.SelectMany(f => f.segments)
            .OrderBy(s => Array.IndexOf(sectionOrder, s.name)).ToArray();
        var map = new PatchMap(basis);
        var writes = new List<KeyValuePair<PatchRegion, byte[]>>();
        foreach (var site in active.SelectMany(f => f.sites)) {
            var value = Convert.FromBase64String(blobs[site.blob]);
            map.Reserve(site.owner, site.rva, value.Length);
            writes.Add(new KeyValuePair<PatchRegion, byte[]>(
                map.Resolve(site.owner, "host", site.rva, value.Length), value));
        }
        foreach (var segment in segments) {
            var value = Convert.FromBase64String(blobs[segment.blob]);
            map.Allocate(segment.owner, segment.name, segment.virtualSize, value.Length);
            writes.Add(new KeyValuePair<PatchRegion, byte[]>(
                map.Resolve(segment.owner, segment.name, 0, value.Length), value));
        }
        var output = new byte[map.Length];
        basis.CopyTo(output, 0);
        foreach (var site in active.SelectMany(f => f.header))
            map.WriteHeader(output, site.rva, Convert.FromBase64String(blobs[site.blob]));
        map.SetHeaders(output, segments);
        foreach (var write in writes)
            write.Value.CopyTo(output, write.Key.fileOffset);
        foreach (var fix in active.SelectMany(f => f.fixups)) {
            if (fix.count < 1 || (fix.factor != 1 && fix.factor != -1))
                throw new InvalidDataException("Invalid patch relocation");
            var dest = map.Resolve(fix.owner, fix.region, fix.address, checked(fix.count * 4));
            int rva = map.Resolve(fix.owner, fix.@base, 0, 1).rva;
            for (int i = 0; i < fix.count; ++i) {
                int pos = dest.fileOffset + 4 * i;
                int value = unchecked(BitConverter.ToInt32(output, pos) + fix.factor * rva);
                BitConverter.GetBytes(value).CopyTo(output, pos);
            }
        }
        if (Engine.Hash(output) != target) throw new InvalidDataException("Stylos patch layout hash mismatch");
        allocations = map.Regions;
        return output;
    }
}
