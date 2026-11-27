using System;
using System.Collections.Generic;
using System.IO;
using System.Linq;
using System.Text;

namespace Retas.DevKit {
    public sealed class PatchRegion {
        public string owner {get;internal set;}
        public string region {get;internal set;}
        public int address {get;internal set;}
        public int rva {get;internal set;}
        public int fileOffset {get;internal set;}
        public int size {get;internal set;}
    }

    public class PatchSectionHeader {
        public string name {get;set;}
        public int virtualSize {get;set;}
        public uint flags {get;set;}
    }

    // Addresses in "host" are RVAs. Each allocated section has its own zero-based space.
    public sealed class PatchMap {
        readonly List<PatchRegion> regions = new List<PatchRegion>();
        readonly byte[] basis;
        readonly int opt, table, count, headers, sectionAlignment, fileAlignment;
        readonly List<PatchRegion> sections = new List<PatchRegion>();
        int nextRva, nextRaw, nextHeader, added;

        public PatchRegion[] Regions { get { return regions.ToArray(); } }
        internal PatchRegion[] ImageSections { get { return sections.ToArray(); } }
        internal int Length { get { return nextRaw; } }

        internal void ValidateLinkedHeaders(PatchMap next, IEnumerable<string> movable) {
            if (opt!=next.opt || table!=next.table || headers!=next.headers || next.count<count ||
                sectionAlignment!=next.sectionAlignment || fileAlignment!=next.fileAlignment)
                throw new InvalidDataException("Incompatible linked image headers");
            var allowed=new bool[table+count*40];
            Action<int,int> allow=(at,n)=>{for(int i=at;i<at+n;i++)allowed[i]=true;};
            // DOS linker stamp, section count, code/data totals, image size, checksum
            // and import directory may change between verified native linked variants.
            allow(64,opt-24-64);allow(opt-18,2);allow(opt+4,12);allow(opt+56,4);allow(opt+64,4);allow(opt+104,8);
            var linkerSections=new HashSet<string>(movable,StringComparer.Ordinal);
            for(int i=0;i<count;i++){
                var section=sections[i];
                var newSection=next.sections.SingleOrDefault(s=>s.region==section.region);
                if(newSection==null)throw new InvalidDataException("Linked image lost "+section.region);
                int oldPos=table+40*i, newPos=table+40*next.sections.IndexOf(newSection);
                for(int j=0;j<40;j++){
                    if(basis[oldPos+j]==next.basis[newPos+j])continue;
                    if(linkerSections.Contains(section.region) && j>=8 && j<24)continue;
                    if(section.region==".text" && j>=8 && j<12 && I32(next.basis,newPos+8)<=newSection.size)continue;
                    throw new InvalidDataException("Unexpected section header edit: "+section.region);
                }
                allow(oldPos,40);
            }
            for(int i=0;i<allowed.Length;i++)if(!allowed[i] && basis[i]!=next.basis[i])
                throw new InvalidDataException("Unexpected PE header edit: "+i.ToString("x"));
        }

        static int Align(int value, int alignment) {
            if (alignment <= 0 || (alignment & (alignment - 1)) != 0)
                throw new InvalidDataException("Invalid Stylos PE alignment");
            return checked(value + alignment - 1) & -alignment;
        }
        static int I32(byte[] bytes, int pos) { return BitConverter.ToInt32(bytes, pos); }
        static void Put(byte[] bytes, int pos, int value) {
            Buffer.BlockCopy(BitConverter.GetBytes(value), 0, bytes, pos, 4);
        }

        public PatchMap(byte[] image) {
            basis = image;
            int pe = I32(image, 60);
            opt = checked(pe + 24);
            if (BitConverter.ToUInt16(image, 0) != 0x5a4d || I32(image, pe) != 0x4550 ||
                BitConverter.ToUInt16(image, pe + 4) != 0x14c || BitConverter.ToUInt16(image, opt) != 0x10b)
                throw new InvalidDataException("Expected Stylos PE32 x86 image");
            table = opt + BitConverter.ToUInt16(image, pe + 20);
            count = BitConverter.ToUInt16(image, pe + 6);
            headers = I32(image, opt + 60);
            sectionAlignment = I32(image, opt + 32);
            fileAlignment = I32(image, opt + 36);
            nextHeader = checked(table + count * 40);
            nextRaw = image.Length;
            if (headers > image.Length || nextHeader > headers) throw new InvalidDataException("Invalid PE headers");
            for (int i = 0; i < count; ++i) {
                int p = table + i * 40;
                var s = new PatchRegion { region = Encoding.ASCII.GetString(image, p, 8).TrimEnd('\0'),
                    rva = I32(image, p + 12), fileOffset = I32(image, p + 20), size = I32(image, p + 16) };
                if (s.fileOffset < 0 || s.size < 0 || checked(s.fileOffset + s.size) > image.Length)
                    throw new InvalidDataException("Invalid PE section");
                nextRva = Math.Max(nextRva, checked(s.rva + Math.Max(s.size, I32(image, p + 8))));
                sections.Add(s);
            }
        }

        public void Reserve(string owner, int rva, int size) {
            int end = checked(rva + size);
            var section = sections.SingleOrDefault(s => rva >= s.rva && end <= (long)s.rva + s.size);
            if (size <= 0 || section == null) throw new InvalidDataException("Unmapped host RVA for " + owner);
            foreach (var other in regions)
                if (rva < (long)other.rva + other.size && other.rva < end)
                    throw new InvalidDataException("Patch conflict: " + owner + " / " + other.owner);
            regions.Add(new PatchRegion { owner = owner, region = "host", address = rva, rva = rva,
                fileOffset = checked(section.fileOffset + (rva - section.rva)), size = size });
        }

        public PatchRegion Allocate(string owner, string name, int size) {
            return Allocate(owner, name, size, size);
        }

        public void ClaimSection(string owner,string name) {
            var s=sections.SingleOrDefault(part=>part.region==name);
            if(s==null)throw new InvalidDataException("Missing host section "+name);
            Reserve(owner,s.rva,s.size);
            var claim=regions[regions.Count-1];claim.region=name;claim.address=0;
        }

        public PatchRegion Allocate(string owner, string name, int virtualSize, int rawSize) {
            int raw = Align(nextRaw, fileAlignment), rva = Align(nextRva, sectionAlignment);
            if (string.IsNullOrEmpty(name) || name.Length > 8 || name == "host" ||
                name.Any(c => c < 33 || c > 126) || sections.Any(s => s.region == name) ||
                regions.Any(s => s.region == name) || virtualSize <= 0 || rawSize <= 0)
                throw new InvalidDataException("Invalid patch section " + name);
            if (nextHeader + 40 > headers || basis.Skip(nextHeader).Take(40).Any(b => b != 0))
                throw new InvalidDataException("No free PE section header");
            rawSize = Align(rawSize, fileAlignment);
            var section = new PatchRegion { owner = owner, region = name, address = 0, rva = rva,
                fileOffset = raw, size = rawSize };
            nextRva = checked(rva + Math.Max(virtualSize, rawSize));
            nextRaw = checked(raw + rawSize);
            nextHeader += 40;
            ++added;
            regions.Add(section);
            return section;
        }

        public PatchRegion Resolve(string owner, string region, int address, int size) {
            var slot = regions.SingleOrDefault(s => s.owner == owner && s.region == region &&
                address >= s.address && (long)address + size <= (long)s.address + s.size);
            if (slot == null || size <= 0) throw new InvalidDataException("Undeclared patch address for " + owner);
            int displacement = address - slot.address;
            return new PatchRegion { owner = owner, region = region, address = address, size = size,
                rva = checked(slot.rva + displacement), fileOffset = checked(slot.fileOffset + displacement) };
        }

        internal void SetHeaders(byte[] output, IEnumerable<PatchSectionHeader> segments) {
            Buffer.BlockCopy(BitConverter.GetBytes(checked((ushort)(count + added))), 0, output, opt - 18, 2);
            Put(output, opt + 56, Align(nextRva, sectionAlignment));
            int p = table + count * 40;
            foreach (var segment in segments) {
                var slot = regions.Single(s => s.region == segment.name);
                Encoding.ASCII.GetBytes(segment.name).CopyTo(output, p);
                Put(output, p + 8, segment.virtualSize);
                Put(output, p + 12, slot.rva);
                Put(output, p + 16, slot.size);
                Put(output, p + 20, slot.fileOffset);
                Put(output, p + 36, unchecked((int)segment.flags));
                p += 40;
            }
        }
        internal void WriteHeader(byte[] output, int offset, byte[] value) {
            if (offset < 0 || checked(offset + value.Length) > table + count * 40)
                throw new InvalidDataException("Header edit outside original PE headers");
            Buffer.BlockCopy(value, 0, output, offset, value.Length);
        }
    }

}
