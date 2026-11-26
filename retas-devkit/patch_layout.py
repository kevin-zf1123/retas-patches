"""Compile declared Stylos patch regions from the build's linked reference images."""
import base64
import gzip
import hashlib
import json
import re
import struct
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


def align(value, boundary):
    if boundary <= 0 or boundary & (boundary - 1):
        raise ValueError("Invalid PE alignment")
    return (value + boundary - 1) & -boundary


def named_sites(manifest):
    declaration = json.loads((ROOT / manifest).read_text('utf8'))
    return {s['name']: int(s['rva'], 0) for s in declaration['sites'] if 'name' in s}


def fingerprint(value):
    return hashlib.sha256(value).hexdigest()


class Image:
    def __init__(self, data):
        pe = struct.unpack_from('<I', data, 60)[0]
        if data[:2] != b'MZ' or data[pe:pe + 6] != b'PE\0\0\x4c\x01':
            raise ValueError("Expected Stylos PE32 x86 image")
        self.opt = pe + 24
        self.table = self.opt + struct.unpack_from('<H', data, pe + 20)[0]
        self.count = struct.unpack_from('<H', data, pe + 6)[0]
        self.sa, self.fa = struct.unpack_from('<II', data, self.opt + 32)
        self.headers = struct.unpack_from('<I', data, self.opt + 60)[0]
        self.sections = []
        for i in range(self.count):
            pos = self.table + 40 * i
            name, size, rva, raw_size, raw = struct.unpack_from('<8sIIII', data, pos)
            self.sections.append((name.rstrip(b'\0').decode('ascii'), size, rva, raw_size, raw))

    def offset(self, rva, size):
        for _, _, start, length, pos in self.sections:
            if start <= rva and rva + size <= start + length:
                return pos + rva - start
        raise ValueError(f"Unmapped host region {rva:#x}+{size}")


class LayoutCatalog:
    def __init__(self):
        registry = json.loads((ROOT / 'plugin-manager/stylos/registry.json').read_text('utf8'))
        self.order = registry['sectionOrder']
        self.manifests = {}
        self.owners = []
        for plugin in [registry['manager'], *registry['plugins']]:
            manifest = json.loads((ROOT / plugin['manifest']).read_text('utf8'))
            if (manifest['id'] != plugin['id'] or manifest['schema'] != 2
                    or manifest['host'] != 'stylos-2.6.0-x86'):
                raise ValueError('Invalid patch manifest for ' + plugin['id'])
            self.manifests[plugin['id']] = manifest
            self.owners.append((plugin.get('bit', 0), manifest))
        self.blobs = {}
        self.groups = {}
        self.outputs = {}
        self.fragments = {}

    def blob(self, data):
        key = fingerprint(data)
        if key not in self.blobs:
            self.blobs[key] = base64.b64encode(data).decode('ascii')
        return key

    def add(self, basis, linked, flags, image='Stylos.exe'):
        """Bind plugin-local sections and fixed host RVAs, rejecting every uncovered byte."""
        worker = image != 'Stylos.exe'
        source, target = Image(basis), Image(linked)
        if source.table != target.table or source.headers != target.headers:
            raise ValueError('Host header layout changed')
        rebuilt = bytearray(basis)
        sites, segments, header, fixups = [], [], [], []
        payloads = []
        claimed = []
        appended = {}
        for bit, owner in self.owners:
            if (bit and not flags & bit) or (worker and not bit):
                continue
            for name in owner['segments']:
                if name in appended:
                    raise ValueError('Multiple owners for ' + name)
                appended[name] = owner['id']
            for site in owner['sites'] + (owner.get('workerSites', []) if worker else []):
                rva, size = int(site['rva'], 0), site['size']
                if size <= 0 or rva < 0 or any(rva < end and start < rva + size for start, end in claimed):
                    raise ValueError(f"Invalid or conflicting site {owner['id']} {rva:#x}")
                claimed.append((rva, rva + size))
                pos = source.offset(rva, size)
                start = target.offset(rva, size)
                value = linked[start:start + size]
                rebuilt[pos:pos + size] = value
                site_record = dict(owner=owner['id'], rva=rva)
                sites.append(site_record)
                payloads.append((owner['id'], 'host', rva, bytearray(value), site_record))
            for fix in owner.get('fixups', []):
                fixups.append(dict(owner=owner['id'], region=fix['region'], address=int(fix['address'], 0),
                                   base=fix['base'], factor=fix['factor'], count=fix.get('count', 1)))

        next_rva = max(r + max(v, s) for _, v, r, s, _ in source.sections)
        end = source.table + 40 * source.count
        ordered = sorted(appended, key=self.order.index)
        if ordered != [s[0] for s in target.sections[source.count:]]:
            raise ValueError('Linked sections differ from declared allocation order')
        for i, name in enumerate(ordered, source.count):
            _, vs, rva, length, pos = target.sections[i]
            expected = (align(next_rva, source.sa), align(len(rebuilt), source.fa))
            if (rva, pos) != expected or length != align(length, source.fa):
                raise ValueError(f'{name} was linked outside its allocated region')
            desc = struct.pack('<8sIIII12xI', name.encode(), vs, rva, length, pos,
                               struct.unpack_from('<I', linked, end + 36)[0])
            if end + 40 > source.headers or any(rebuilt[end:end + 40]) or linked[end:end + 40] != desc:
                raise ValueError('Invalid new section header for ' + name)
            value = linked[pos:pos + length]
            segments.append(dict(owner=appended[name], name=name, virtualSize=vs,
                                 flags=struct.unpack_from('<I', desc, 36)[0]))
            payloads.append((appended[name], name, 0, bytearray(value), segments[-1]))
            rebuilt[end:end + 40] = desc
            rebuilt.extend(bytes(pos - len(rebuilt)) + value)
            next_rva = rva + max(vs, length)
            end += 40
        struct.pack_into('<H', rebuilt, source.opt - 18, target.count)
        struct.pack_into('<I', rebuilt, source.opt + 56, align(next_rva, source.sa))
        # Preserve the linked import directory, checksum and other original-header metadata.
        # Section placement, section count and SizeOfImage belong exclusively to the mapper.
        header_end = source.table + 40 * source.count
        diff = bytes(a ^ b for a, b in zip(rebuilt[:header_end], linked[:header_end]))
        for match in re.finditer(rb'[^\0]+', diff):
            lo, hi = match.span()
            # Legacy linkers update the DOS stub, code/data totals, checksum and import directory.
            allowed = [(64, source.opt - 24), (source.opt + 4, source.opt + 12),
                       (source.opt + 64, source.opt + 68), (source.opt + 104, source.opt + 112)]
            if '.motion' in appended:
                text_index = next(i for i, section in enumerate(source.sections) if section[0] == '.text')
                size_field = source.table + 40 * text_index + 8
                if target.sections[text_index][1] > source.sections[text_index][3]:
                    raise ValueError('Motion code exceeds the reserved text section')
                allowed.append((size_field, size_field + 4))
            if not any(start <= lo and hi <= end for start, end in allowed):
                raise ValueError(f'Undeclared PE header change at {lo:#x}')
            rebuilt[lo:hi] = linked[lo:hi]
            header.append(dict(rva=lo, blob=self.blob(linked[lo:hi])))
        if rebuilt != linked:
            first = next((i for i, (a, b) in enumerate(zip(rebuilt, linked)) if a != b), len(rebuilt))
            raise ValueError(f'Undeclared change at file offset {first:#x}, flags={flags}')
        addresses = {name: rva for name, _, rva, _, _ in target.sections}
        for fix in fixups:
            count, address = fix['count'], fix['address']
            if (count < 1 or fix['factor'] not in (1, -1)
                    or appended.get(fix['base']) != fix['owner']):
                raise ValueError('Invalid plugin relocation')
            for owner, region, start, value, _ in payloads:
                offset = address - start
                if (owner, region) == (fix['owner'], fix['region']) and 0 <= offset <= len(value) - 4 * count:
                    for pos in range(offset, offset + 4 * count, 4):
                        word, = struct.unpack_from('<I', value, pos)
                        struct.pack_into('<I', value, pos, (word - fix['factor'] * addresses[fix['base']]) & 0xffffffff)
                    break
            else:
                raise ValueError('Relocation outside declared region')
        for _, _, _, value, record in payloads:
            record['blob'] = self.blob(value)
        family = fingerprint(basis)
        self.outputs[fingerprint(linked)] = dict(family=family, image=image, flags=flags)
        group = self.groups.setdefault(family + '/' + image, {})
        owners = {'@pe': 0}
        owners.update((o['id'], bit) for bit, o in self.owners
                      if (not bit or flags & bit) and (bit or not worker))
        for owner, bit in owners.items():
            fragment = dict(sites=[s for s in sites if s['owner'] == owner],
                            segments=[s for s in segments if s['owner'] == owner],
                            fixups=[f for f in fixups if f['owner'] == owner],
                            header=header if owner == '@pe' else [])
            if not any(fragment.values()):
                continue
            key = fingerprint(json.dumps(fragment, sort_keys=True).encode('ascii'))
            self.fragments[key] = fragment
            group.setdefault(owner, (bit, {}))[1][flags] = key

    @staticmethod
    def variants(bit, cases):
        # Keep a dependency bit only when dropping it would select two different fragments.
        mask = 0
        for flags in cases:
            mask |= flags
        for shift in range(mask.bit_length()):
            candidate = mask & ~(1 << shift)
            if len({(f & candidate, k) for f, k in cases.items()}) == len({f & candidate for f in cases}):
                mask = candidate
        return dict(bit=bit, mask=mask, variants={f & mask: k for f, k in cases.items()})

    def save(self, path):
        groups = {name: {owner: self.variants(bit, cases) for owner, (bit, cases) in owners.items()}
                  for name, owners in self.groups.items()}
        data = json.dumps(dict(version=1, groups=groups, fragments=self.fragments, outputs=self.outputs,
                               blobs=self.blobs, manifests=self.manifests, sectionOrder=self.order),
                          separators=(',', ':'), sort_keys=True).encode('ascii')
        path.write_bytes(gzip.compress(data, mtime=0))
