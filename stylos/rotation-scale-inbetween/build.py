"""Link the managed midpoint command and the floating-point row adapters."""
import argparse
import os
import tempfile
import zipfile
import json
from pathlib import Path
import shutil
import struct
import subprocess

import lief


FOLDER = Path(__file__).resolve().parent


def command(zig, work):
    resource = work / "dialogs.res"
    subprocess.run([zig, "rc", "/c65001", "/fo", resource, FOLDER / "progress.rc"], check=True)
    raw = resource.read_bytes()
    names = {1: "progress_dialog", 2: "settings_dialog", 3: "pick_dialog", 4: "preview_dialog"}
    data = [".section .rodata"]
    offset = 0
    while offset < len(raw):
        size, header = struct.unpack_from("<II", raw, offset)
        type_tag, kind, id_tag, rid = struct.unpack_from("<4H", raw, offset+8)
        if size and (type_tag, kind, id_tag) == (65535, 5, 65535):
            name = names.pop(rid)
            (work / (name+".bin")).write_bytes(raw[offset+header:offset+header+size])
            data.extend([".balign 4", ".global "+name, name+":", '.incbin "'+name+'.bin"'])
        offset = (offset+header+size+3) & ~3
    if names:
        raise ValueError("Missing midpoint dialog templates")
    data.extend([".balign 4", ".global stylos_links", "stylos_links:", ".zero 76"])
    (work / "data.S").write_text("\n".join(data)+"\n")
    zig_lib = Path(json.loads(subprocess.check_output([zig, "env"]))["lib_dir"])
    options = ["-target", "x86-freestanding-none", "-march=i386", "-mno-sse", "-mno-mmx",
               "-ffreestanding", "-fno-stack-protector", "-fPIC", "-Oz", "-g0", "-fshort-wchar",
               "-fms-extensions", "-fdeclspec", "-D_X86_=1", "-D_WIN32", "-D__MINGW32__",
               "-DWIN32_LEAN_AND_MEAN", "-Wno-ignored-attributes",
               "-isystem", str(zig_lib / "libc/include/any-windows-any")]
    objects = []
    for source in (FOLDER/"command.c", FOLDER/"ui.c", FOLDER/"fast_rotate.c", FOLDER/"precision.S", work/"data.S"):
        obj = source.stem+".o"
        speed = ["-O2"] if source.name=="fast_rotate.c" else []
        subprocess.run([zig, "cc", *options, *speed, "-c", source, "-o", obj], cwd=work, check=True)
        objects.append(obj)
    (work/"command.ld").write_text("SECTIONS { . = 0; .feature : { *(.text*) *(.rodata*) *(.got*) } }")
    subprocess.run([zig, "ld.lld", "-m", "elf_i386", "-T", "command.ld", "-e", "align_pair",
                    "--no-undefined", *objects, "-o", "command.elf"], cwd=work, check=True)
    elf = lief.ELF.parse(str(work/"command.elf"))
    return bytes(elf.get_section(".feature").content), {s.name: s.value for s in elf.symbols}



def build_installer(zig, work):
    blob, symbols = command(zig, work)
    payload = work / "alignment.bin"
    payload.write_bytes(struct.pack("<3I", *(symbols[n] for n in
                        ("align_pair","stylos_links","preferences_modal")))+blob)
    # The standalone installer and manager package verify the same native sites.
    plan = bytearray()
    for site in json.loads((FOLDER/"host-layout.json").read_bytes())["sites"]:
        if "expected" not in site:
            continue
        expected = bytes.fromhex(site["expected"])
        if len(expected) != site["size"]:
            raise ValueError("Invalid light-table claim "+site["rva"])
        target = symbols[site["call"]] if "call" in site else -1
        plan.extend(struct.pack("<IiB",int(site["rva"],0),target,len(expected)))
        plan.extend(expected)
        if target < 0:
            replacement = bytes.fromhex(site["bytes"])
            if len(replacement) != len(expected):
                raise ValueError("Invalid replacement at "+site["rva"])
            plan.extend(replacement)
    sites = work / "row-sites.bin"
    sites.write_bytes(plan)
    exe = work / "StylosInbetween.exe"
    compiler = shutil.which("mcs")
    if not compiler:
        compiler = Path(os.environ["WINDIR"])/"Microsoft.NET/Framework/v4.0.30319/csc.exe"
    subprocess.run([compiler, "-nologo", "-platform:x86", "-optimize+", "-r:System.Windows.Forms.dll",
                    "-resource:"+str(payload)+",alignment", "-resource:"+str(sites)+",row-sites",
                    "-out:"+str(exe), FOLDER/"Install.cs", FOLDER/"ShortcutBinding.cs"], check=True)
    return exe


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("output", type=Path)
    cli.add_argument("--zig", default="zig")
    args = cli.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory() as tmp:
        installer = build_installer(args.zig, Path(tmp))
        shutil.copy2(installer, args.output/installer.name)
        with zipfile.ZipFile(args.output/"Stylos-Inbetween-x86.zip","w",zipfile.ZIP_DEFLATED) as package:
            package.write(installer,installer.name)
            package.write(FOLDER.parents[1]/"LICENSE","LICENSE")
            for guide in ("README.md","README.zh-CN.md","README.ja.md"):
                package.write(FOLDER/guide,guide)
