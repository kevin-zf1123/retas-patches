"""Run offline lifecycle, insertion conflict and compositor tests on disposable host copies."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
import zipfile


def digest(p):
    return hashlib.sha256(p.read_bytes()).hexdigest()


def main():
    cli=argparse.ArgumentParser(description=__doc__)
    for arg in ("manager","base","packages","profile"):
        cli.add_argument("--"+arg,type=Path,required=True)
    cli.add_argument("--high-dpi",action="store_true")
    a=cli.parse_args()
    with TemporaryDirectory() as temp:
        work=Path(temp)
        host=work/"host";host.mkdir()
        exe=host/"Stylos.exe";shutil.copy2(a.base,exe)
        appdata=work/"profile";shutil.copytree(a.profile/"CELSYS/RETAS STUDIO/Stylos",appdata/"CELSYS/RETAS STUDIO/Stylos")
        def run(action,*params,ok=True):
            result=subprocess.run([a.manager,action,host,*params],capture_output=True,env={**os.environ,"APPDATA":str(appdata)})
            if (result.returncode==0)!=ok:
                raise RuntimeError(f"{action} {params}: {result.returncode}, {result.stderr!r}")
            return result.stdout
        run("--setup");assert not run("--list").strip()
        boot=digest(exe)
        def package(id,layout,members):
            path=work/(id+".zip")
            manifest=dict(schema=3,host="stylos-2.6.0-x86",id=id,name=id,version="1.0.0")
            with zipfile.ZipFile(path,"w") as z:
                z.writestr("plugin.json",json.dumps(manifest));z.writestr("layout.json",json.dumps(layout))
                for k,v in members.items():z.writestr(k,v)
            return path
        def section(id,name,flag):
            return package(id,dict(sections=[dict(name=name,virtualSize=32,flags=0x40000040,data="data.bin")],
                               relocations=[dict(region=name,address=0,target=name,addend=8)]),
                               {"data.bin":bytes(4)+flag+bytes(20)})
        pa=section("external-first",".qaone",b"ONE!")
        pb=section("external-second",".qatwo",b"TWO!")
        def zip_with(name,members):
            path=work/name
            with zipfile.ZipFile(path,"w") as archive:
                for member in members:archive.writestr(member,b"")
            return path
        sdk=zip_with("tools.zip",["plugin.h","retas.h","pack.py"])
        manager=zip_with("manager.zip",["update.json","StylosPluginManager.exe"])
        malformed=zip_with("broken.zip",["README.md"])
        run("--install",sdk,manager,ok=False)
        run("--install",pa,sdk,malformed,ok=False)
        assert digest(exe)==boot and not run("--list").strip()
        run("--install",sdk,pa,manager,pb)
        layout=host/".stylos-plugins/layout.json"
        def slot(name):
            region=next(r for r in json.loads(layout.read_bytes())["images"]["Stylos.exe"] if r["region"]==name)
            actual=int.from_bytes(exe.read_bytes()[region["fileOffset"]:region["fileOffset"]+4],"little")
            assert actual==region["rva"]+8
            return region["rva"]
        prior=slot(".qatwo")
        before=digest(exe);state=host/".stylos-plugins/installed-v2.json";record=digest(state)
        run("--install",section("collides",".qaone",b"BAD!"),ok=False)
        assert digest(exe)==before and digest(state)==record
        run("--uninstall","external-first");assert slot(".qatwo")<prior
        run("--uninstall","external-second");assert digest(exe)==boot
        fixed=next(r for r in json.loads(layout.read_bytes())["images"]["Stylos.exe"] if r["region"]=="host")
        src=exe.read_bytes()[fixed["fileOffset"]:fixed["fileOffset"]+1]
        bad=package("occupied",dict(edits=[dict(rva=fixed["rva"],expected="a.bin",data="b.bin")]),
                    {"a.bin":src,"b.bin":bytes([src[0]^1])})
        run("--install",bad,ok=False);assert digest(exe)==boot
        for name in ("Rotation-Shortcuts","Mousewheel-Zoom","Fast-Navigation","Better-Canvas-Display",
                     "Native-Tablet","Windows-Ink","Motion-Check","Inbetween","High-DPI"):
            run("--install",a.packages/("Stylos-"+name+"-x86.zip"),ok=name!="High-DPI" or a.high_dpi)
        full=digest(exe);listing=run("--list")
        assert b"tablet-native disabled" in listing and b"windows-ink enabled" in listing
        backup=work/"backup.zip";run("--backup",backup)
        run("--disable","motion-check");assert not (host/"StylosRenderA.exe").exists()
        run("--uninstall","inbetween");run("--restore",backup)
        assert digest(exe)==full and run("--list").splitlines() and (host/"StylosRenderA.exe").is_file()
        run("--self-uninstall");assert digest(exe)==digest(a.base)
        print("PASS external sections, relocation after removal, conflict rollback, bundled plugin coexistence, backup and uninstall")


if __name__=="__main__":
    main()
