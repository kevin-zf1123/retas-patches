"""Shared build-time operations for plugin-owned Stylos reference image recipes."""
import hashlib
import json
from pathlib import Path
import sys

SDK=Path(__file__).resolve().parents[1]/"retas-devkit"
sys.path.insert(0,str(SDK))
from delta import transition


class LinkedImages:
    def __init__(self,folder,refs,zig,work,members):
        if not refs:raise ValueError("Provide --refs for this linked-image plugin")
        self.refs,self.zig,self.work,self.members=refs,zig,work,members
        self.expected=json.loads((SDK/"reference-hashes.json").read_bytes())
        declarations=json.loads((folder/"host-layout.json").read_bytes())
        def claims(key):
            return [dict(rva=int(s["rva"],0),size=s["size"]) for s in declarations.get(key,[])]
        self.layout=dict(claims=claims("sites"),workerClaims=claims("workerSites"),
                         ownedSections=declarations["segments"],variants={})

    def reference(self,variant,stage):
        p=self.refs/variant/(stage+".exe")
        if hashlib.sha256(p.read_bytes()).hexdigest()!=self.expected[variant][stage]:
            raise ValueError("Incorrect reference: "+str(p))
        return p

    def linked(self,variant,stage):
        from stylos260 import link_canvas
        p=self.work/(variant+"-"+stage+".exe")
        if not p.exists():link_canvas(self.reference(variant,stage),p)
        return p

    def variant(self,before,after,workers=None):
        def edit(name,source,target):
            data=transition(source,target)
            key="patches/"+hashlib.sha256(data).hexdigest()+".bin"
            self.members[key]=data
            return dict(image=name,delta=key)
        images=[edit("Stylos.exe",before,after)]
        for name,(source,target) in (workers or {}).items():images.append(edit(name,source,target))
        self.layout["variants"][hashlib.sha256(before).hexdigest()]=dict(images=images)
