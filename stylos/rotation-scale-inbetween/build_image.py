from pathlib import Path
import runpy
import subprocess


def prepare(images):
    builder = runpy.run_path(str(Path(__file__).with_name("build.py")))
    installer = builder["build_installer"](images.zig, images.work)
    for variant in ("chs", "chs-laa", "precanvas"):
        for stage in ("base", "main"):
            source = images.linked(variant, stage)
            target = images.work/(variant+"-"+stage+"-aligned.exe")
            subprocess.run([installer,source,target],check=True,stdout=subprocess.DEVNULL)
            images.variant(source.read_bytes(),target.read_bytes())
