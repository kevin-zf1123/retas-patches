"""Create a new disposable Stylos lab from a manager-enabled 2.6.0 installation."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import sys
import zipfile

from PIL import Image, ImageDraw

root = Path(__file__).resolve().parents[2]



def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("installed", type=Path, help="Folder containing the installed Stylos.exe")
    cli.add_argument("lab", type=Path, help="New directory")
    cli.add_argument("--zig", default="zig")
    cli.add_argument("--profile", type=Path, default=Path(os.environ["APPDATA"])/"CELSYS/RETAS STUDIO/Stylos")
    args = cli.parse_args()
    args.lab.mkdir(parents=True, exist_ok=False)
    target = args.lab/"Studio/Stylos"
    target.mkdir(parents=True)
    for src in args.installed.iterdir():
        if src.suffix.lower() in (".dll",".sys",".dat") or src.name in (
                "Stylos.exe","StylosPluginManager.exe","InitInfo.exe","InitExcl.exe"):
            shutil.copy2(src, target/src.name)
    shutil.copytree(args.installed.parent/"Common Files",target.parent/"Common Files")
    original=args.installed/".stylos-plugins"
    if not (original/"modules.ini").is_file():
        raise ValueError("Expected a DevKit v2 installation with enabled canvas modules")
    shutil.copytree(original/"active",target/".stylos-plugins/active")
    shutil.copy2(original/"modules.ini",target/".stylos-plugins/modules.ini")
    shutil.copytree(args.profile, args.lab/"profile/CELSYS/RETAS STUDIO/Stylos")
    for folder in ("local","tmp"):
        (args.lab/folder).mkdir()
    import importlib.util
    spec=importlib.util.spec_from_file_location("host_builder",root/"plugin-manager/build.py")
    builder=importlib.util.module_from_spec(spec);spec.loader.exec_module(builder)
    builder.build_bridge(args.zig,target/"StylosCanvasFilter.dll",root/"better-canvas/bench/trace.c")
    for id in ("anti-alias","fast-navigation","rotation-shortcuts","mousewheel-zoom"):
        slot=target/".stylos-plugins/active"/id/"module.dll"
        if not slot.exists():continue
        out=args.lab/"instrumented"/id
        subprocess.run([sys.executable,root/"tools/build_plugins.py","--only",id,"--trace","--out",out,"--zig",args.zig],check=True)
        with zipfile.ZipFile(next(out.glob("*.zip"))) as package:slot.write_bytes(package.read("module.dll"))
    for name, size, start, step, left, right, top, bottom, a, b, width in (
        ("lines", (2048,1536),64,49,160,180,100,1400,34,270,3),
        ("large", (6144,4096),0,87,600,700,200,3800,15,310,5)):
        image = Image.new("RGB",size,"white")
        draw = ImageDraw.Draw(image)
        for x in range(start, 2000 if name == "lines" else size[0], step):
            draw.arc((x-left, top, x+right, bottom), a, b, fill="black", width=width)
        image.save(args.lab/(name+".bmp"))


if __name__ == "__main__":
    main()
