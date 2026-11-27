"""Build an independent DevKit layout for one optional CrossOver input fix."""
import json
import struct
import subprocess


def prepare(folder, zig, work, members):
    spec = json.loads((folder / "host-layout.json").read_bytes())
    site = spec["site"]
    hook = int(site["rva"], 0)
    original = bytes.fromhex(site["expected"])
    if len(original) != site["size"] or len(original) < 6:
        raise ValueError("Expected a branch site with a trailing NOP")

    obj = work / "fix.obj"
    subprocess.run([zig, "cc", "-target", "x86-windows-gnu", "-c", "-g0",
                    folder / spec["source"], "-o", obj], check=True)
    coff = obj.read_bytes()
    machine, count = struct.unpack_from("<HH", coff)
    if machine != 0x14c or struct.unpack_from("<H", coff, 16)[0]:
        raise ValueError("Expected an i386 COFF object")
    headers = [20 + 40 * i for i in range(count)]
    header, = [p for p in headers if coff[p:p + 8].rstrip(bytes(1)) == b".text"]
    size, offset = struct.unpack_from("<II", coff, header + 16)
    body = coff[offset:offset + size]
    if struct.unpack_from("<H", coff, header + 32)[0] or body[:5] != bytes([0xe9, 0, 0, 0, 0]):
        raise ValueError("Unexpected relocation or return instruction")

    section = spec["section"]
    members["original.bin"] = original
    members["hook.bin"] = bytes([0xe9]) + bytes(4) + bytes([0x90]) * (len(original) - 5)
    members["code.bin"] = body
    # Returning through the last NOP keeps both branch endpoints in owned regions.
    return dict(edits=[dict(rva=hook, expected="original.bin", data="hook.bin")],
                sections=[dict(name=section, virtualSize=len(body), flags=0x60000020, data="code.bin")],
                relocations=[dict(region="host", address=hook+1, relativeTarget=section, addend=5),
                             dict(region=section, address=1, relativeTarget="host", addend=hook+len(original)-1)])
