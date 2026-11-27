"""Export the standalone plugin-author SDK."""
import argparse
from pathlib import Path
import sys
sys.path.insert(0,str(Path(__file__).resolve().parent))
from pack import archive

if __name__=="__main__":
    cli=argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--out",type=Path,required=True)
    args=cli.parse_args()
    sdk=Path(__file__).resolve().parent
    files={name:(sdk/name).read_bytes() for name in
           ("plugin.h","retas.h","module.def","pack.py","delta.py","PatchMap.cs","host260.json","entries.json","retas_client.py","README.md")}
    for p in (sdk/"examples").rglob("*"):
        if p.is_file() and p.suffix in (".c",".json"):files[p.relative_to(sdk).as_posix()]=p.read_bytes()
    files["LICENSE"]=(sdk.parent/"LICENSE").read_bytes()
    args.out.mkdir(parents=True,exist_ok=True)
    (args.out/"Retas-DevKit-0.2.zip").write_bytes(archive(files))
