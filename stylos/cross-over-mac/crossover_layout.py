"""Link reference images for the two optional Stylos 2.6.0 CrossOver input fixes."""
import json
from pathlib import Path
import struct
import subprocess

from patch_layout import Image, align

HERE = Path(__file__).resolve().parent


def assemble(zig, work):
    plugins = []
    registry = json.loads((HERE.parents[1] / 'plugin-manager/stylos/registry.json').read_bytes())
    for directory, source in [('shortcuts', 'shortcuts.S'), ('pressuredriver', 'pressure.S')]:
        manifest = json.loads((HERE / directory / 'plugin.json').read_bytes())
        plugin = next(p for p in registry['plugins'] if p['id'] == manifest['id'])
        site, = manifest['sites']
        continuation = int(site['rva'], 0) + site['size']
        obj = work / (directory + '.obj')
        subprocess.run([zig, 'cc', '-target', 'x86-windows-gnu', '-c', '-g0',
                        f'-DSTYLOS_RESUME_RVA={continuation}', str(HERE / directory / source),
                        '-o', str(obj)], check=True)
        # These two assembly sources contain a single self-contained code section.
        coff = obj.read_bytes()
        machine, count = struct.unpack_from('<HH', coff)
        if machine != 0x14c or struct.unpack_from('<H', coff, 16)[0]:
            raise ValueError(f'Expected an i386 COFF object for {directory}')
        headers = [20 + 40 * i for i in range(count)]
        header, = [p for p in headers if coff[p:p + 8].rstrip(b'\0') == b'.text']
        size, offset = struct.unpack_from('<II', coff, header + 16)
        body = coff[offset:offset + size]
        if struct.unpack_from('<H', coff, header + 32)[0] or body[:5] != b'\xe9' + struct.pack('<I', continuation - 5):
            raise ValueError(f'Unexpected relocation or return instruction in {directory}')
        plugins.append((plugin['bit'], manifest, body))
    return plugins


def append_fix(linked, manifest, body):
    image = Image(linked)
    site, = manifest['sites']
    hook = int(site['rva'], 0)
    pos = image.offset(hook, site['size'])
    if linked[pos:pos + site['size']] != bytes.fromhex(site['expected']):
        raise ValueError('Unexpected Stylos input routine for ' + manifest['id'])
    header = image.table + len(image.sections) * 40
    if header + 40 > image.headers or any(linked[header:header + 40]):
        raise ValueError('No free Stylos section header')
    rva = align(max(r + max(v, s) for _, v, r, s, _ in image.sections), image.sa)
    raw = align(len(linked), image.fa)
    size = align(len(body), image.fa)
    data = bytearray(linked)
    data.extend(bytes(raw - len(data)) + body + bytes(size - len(body)))
    name, = manifest['segments']
    struct.pack_into('<8sIIII12xI', data, header, name.encode(), len(body), rva, size, raw, 0x60000020)
    struct.pack_into('<H', data, image.opt - 18, len(image.sections) + 1)
    struct.pack_into('<I', data, image.opt + 56, align(rva + max(size, len(body)), image.sa))
    # Entry follows the five-byte return jump. Both displacements are normalized
    # by LayoutCatalog.add and rebound by PatchCatalog.Build at installation.
    data[pos:pos + site['size']] = b'\xe9' + struct.pack('<i', rva - hook) + b'\x90' * (site['size'] - 5)
    struct.pack_into('<i', data, raw + 1, hook + site['size'] - rva - 5)
    return bytes(data)


def variants(linked, plugins):
    images = [(0, linked)]
    for bit, manifest, body in plugins:
        images += [(mask | bit, append_fix(image, manifest, body)) for mask, image in images]
    return images
