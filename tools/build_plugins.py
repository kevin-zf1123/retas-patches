"""Release builder for the maintained plugins. This program is not part of the manager."""
import argparse
import runpy
import struct
import json
import os
import shutil
from pathlib import Path
import subprocess
import sys
from tempfile import TemporaryDirectory

ROOT=Path(__file__).resolve().parents[1]
SDK=ROOT/"retas-devkit"
sys.path.insert(0,str(SDK))
from delta import transition
from pack import archive


def build(manifest,args):
    p=json.loads(manifest.read_bytes());folder=manifest.parent
    recipe=json.loads((folder/"build.json").read_bytes())
    members={"plugin.json":manifest.read_bytes(),"LICENSE":(ROOT/"LICENSE").read_bytes(),"README.md":(folder/recipe.get("guide","README.md")).read_bytes()}
    with TemporaryDirectory() as temp:
        d=Path(temp)
        if recipe.get("sources"):
            res=[]
            if recipe.get("resource"):
                res=[d/"page.res"]
                subprocess.run([args.zig,"rc","/c65001","/fo",res[0],ROOT/recipe["resource"]],check=True)
            module=d/p["module"]
            trace=["-DSTYLOS_NAV_TRACE",ROOT/"better-canvas/bench/trace.c",
                   *[ROOT/x for x in recipe.get("traceSources",[])]] if args.trace else []
            subprocess.run([args.zig,"cc","-target","x86-windows-gnu","-march=pentium4","-O3","-ffp-contract=off",*trace,
                            "-shared","-s",*["-D"+x for x in recipe.get("defines",[])],
                            *[ROOT/x for x in recipe["sources"]],SDK/"module.def",*res,"-o",module,
                            "-luser32","-lcomctl32","-lgdi32"],check=True)
            data=bytearray(module.read_bytes())
            struct.pack_into("<I",data,struct.unpack_from("<I",data,60)[0]+8,0)
            members[p["module"]]=bytes(data)
        if recipe.get("runtimeBuild"):
            extra=["-Tcc",args.tcc] if recipe.get("usesTcc") else []
            env=dict(os.environ,PATH=str(Path(args.zig).resolve().parent)+os.pathsep+os.environ["PATH"])
            subprocess.run([shutil.which("pwsh") or "powershell","-NoProfile","-ExecutionPolicy","Bypass","-File",folder/recipe["runtimeBuild"],
                            "-OutputDirectory",d,*extra],env=env,check=True)
            for name in p["files"]:
                path=d/name if (d/name).exists() else folder/name
                members["files/"+name]=path.read_bytes()
        if recipe.get("imageBuilder"):
            sys.path.insert(0,str(Path(__file__).resolve().parent))
            from linked_images import LinkedImages
            images=LinkedImages(folder,args.refs,args.zig,d,members)
            runpy.run_path(str(folder/recipe["imageBuilder"]))["prepare"](images)
            members["layout.json"]=json.dumps(images.layout,separators=(",",":")).encode()
        args.out.mkdir(parents=True,exist_ok=True)
        (args.out/recipe["package"]).write_bytes(archive(members))
        print(recipe["package"],flush=True)


if __name__=="__main__":
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--out",type=Path,required=True)
    cli.add_argument("--refs",type=Path)
    cli.add_argument("--inbetween",type=Path,help=argparse.SUPPRESS)  # compatibility with older builds
    cli.add_argument("--zig",default="zig")
    cli.add_argument("--tcc",default="tcc.exe")
    cli.add_argument("--only")
    cli.add_argument("--trace",action="store_true",help="Instrument a canvas module in a disposable lab")
    args=cli.parse_args()
    found=False
    for base in (ROOT/"better-canvas",ROOT/"stylos"):
        for m in sorted(base.rglob("plugin.json")):
            if (m.parent/"build.json").is_file() and (not args.only or json.loads(m.read_bytes())["id"]==args.only):
                found=True;build(m,args)
    if not found:cli.error("Unknown component")
