import base64
import hashlib
import json
from pathlib import Path


def prepare(images):
    variants=json.loads(Path(__file__).with_name("versions.json").read_bytes())["variants"]
    for v in variants[:3]:
        variant=v["id"]
        basis=images.reference("chs" if variant=="chs" else "chs-laa","base").read_bytes()
        source=images.reference(variant,"main").read_bytes()
        workers={}
        for name,spec in v["workers"].items():
            worker=bytearray(source[:spec["size"]]);worker.extend(bytes(spec["size"]-len(worker)))
            for offset,encoded in spec["delta"]:
                value=base64.b64decode(encoded);worker[offset:offset+len(value)]=value
            if hashlib.sha256(worker).hexdigest()!=spec["hash"].lower():raise ValueError("Worker mismatch: "+name)
            workers[name]=(basis,worker)
        images.variant(images.linked(variant,"base").read_bytes(),images.linked(variant,"main").read_bytes(),workers)
