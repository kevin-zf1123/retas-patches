"""Build the open manager and the Stylos 2.6 host bridge without any plugin sources."""
import argparse
import json
import os
import struct
import subprocess
import sys
from pathlib import Path
from tempfile import TemporaryDirectory

HERE = Path(__file__).resolve().parent
SDK = HERE.parent / "retas-devkit"
sys.path.insert(0,str(SDK))
from delta import transition
from pack import archive
from stylos260 import link_canvas, digest

BASES = {"b39fd73bb4a186c1e57a5425e42004d0292c1a9699cb0fe1d87c99940170cc8d",
         "1671d29cd1ae996eadad6c77fe2efab9a2c691261b47354706dc3ee9be57ee97"}

def build_bridge(zig,dll,trace_source=None):
    src=HERE/"host"
    trace=["-DRETAS_TRACE_TILE",trace_source] if trace_source else []
    subprocess.run([zig,"cc","-target","x86-windows-gnu","-O2","-shared","-s",*sorted(src.glob("*.c")),
                    SDK/"StylosApi.c",SDK/"NativeTile.S",src/"bridge.def",*trace,"-o",dll,"-luser32","-lcomctl32"],check=True)
    data=bytearray(dll.read_bytes())
    struct.pack_into("<I",data,struct.unpack_from("<I",data,60)[0]+8,0)
    dll.write_bytes(data)


def build(bases, zig, out):
    if {digest(p.read_bytes()).hex() for p in bases} != BASES:
        raise ValueError("Expected the chs and chs-laa pristine Stylos 2.6 baselines")
    out.mkdir(exist_ok=True,parents=True)
    with TemporaryDirectory() as temp:
        work=Path(temp)
        resources={}
        for p in bases:
            linked=work/"linked.exe"
            link_canvas(p,linked)
            a=p.read_bytes()
            resources["bootstrap."+digest(a).hex()] = transition(a,linked.read_bytes())
        sites=json.loads((SDK/"host260.json").read_bytes())
        resources["adapter"]=json.dumps(dict(claims=[dict(rva=int(s["rva"],0),size=s["size"]) for s in sites["sites"]],
                                               ownedSections=sites["ownedSections"])).encode()
        dll=work/"StylosCanvasFilter.dll"
        build_bridge(zig,dll)
        resources["bridge"]=dll.read_bytes()
        exe=work/"StylosPluginManager.exe"
        version=work/"Version.cs";version.write_text('[assembly:System.Reflection.AssemblyFileVersion("0.2.1.0")]')
        args=[Path(os.environ["WINDIR"])/"Microsoft.NET/Framework/v4.0.30319/csc.exe","/nologo","/optimize+",
              "/platform:x86","/target:winexe","/out:"+str(exe)]
        for name in ["System.Windows.Forms","System.Drawing","System.Web.Extensions","System.IO.Compression"]:
            args.append("/r:"+name+".dll")
        for name,data in resources.items():
            path=work/name;path.write_bytes(data);args.append("/resource:"+str(path)+","+name)
        subprocess.run([*args,*sorted((HERE/"core").glob("*.cs")),SDK/"PatchMap.cs",SDK/"ShortcutBinding.cs",version],check=True)
        release={"product":"retas.stylos.manager.x86.v2","version":"0.2.1"}
        (out/"Stylos-Plugin-Manager-x86.zip").write_bytes(archive({exe.name:exe.read_bytes(),"update.json":json.dumps(release).encode(),
                    "README.md":(HERE/"README.md").read_bytes(),"LICENSE":(HERE.parent/"LICENSE").read_bytes()}))


if __name__=="__main__":
    p=argparse.ArgumentParser(description=__doc__)
    p.add_argument("--base",type=Path,action="append",required=True)
    p.add_argument("--zig",default="zig")
    p.add_argument("--out",type=Path,required=True)
    a=p.parse_args();build(a.base,a.zig,a.out)
