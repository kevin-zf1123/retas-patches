"""Pack a schema-3 plugin directory. No manager checkout, registry or host image required."""
import argparse
import io
import json
from pathlib import Path
import re
import zipfile


def archive(members):
    out = io.BytesIO()
    with zipfile.ZipFile(out, "w") as z:
        for name, contents in sorted(members.items()):
            z.writestr(zipfile.ZipInfo(name), contents, compress_type=zipfile.ZIP_DEFLATED)
    return out.getvalue()


def pack(source, dest):
    source, dest = source.resolve(), dest.resolve()
    manifest = json.loads((source / "plugin.json").read_bytes())
    if manifest.get("schema") != 3 or not re.fullmatch("[a-z][a-z0-9-]{0,63}", manifest["id"]):
        raise ValueError("Expected schema 3 and a valid plugin id")
    if source in dest.parents:
        raise ValueError("Write the archive outside the input directory")
    members = {p.relative_to(source).as_posix(): p.read_bytes() for p in source.rglob("*") if p.is_file()}
    for n in [manifest.get("module"), *["files/" + n for n in manifest.get("files", [])],
              *["payload/" + n for n in manifest.get("assets", [])]]:
        if n and n not in members:
            raise ValueError("Missing payload: " + n)
    dest.parent.mkdir(parents=True,exist_ok=True)
    dest.write_bytes(archive(members))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    pack(args.source, args.output)
