"""Install a newly compiled external package using the released WinForms GUI and exercise it in Stylos."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import hashlib
import json
import os
from pathlib import Path
import subprocess
import sys
import time
import uuid

ROOT=Path(__file__).resolve().parents[2]
sys.path.insert(0,str(ROOT/"better-canvas/bench"))
from desktop import launch, windows, wait_for, user


def caption(w):
    value=C.create_unicode_buffer(256)
    user.SendMessageW(w,13,len(value),value)
    return value.value


def verify(args):
    root=args.folder.resolve()
    work=args.work.resolve()
    mgr=root/"StylosPluginManager.exe"
    env=dict(APPDATA=str(args.profile.resolve()), LOCALAPPDATA=str(work/"local"), TEMP=str(work), TMP=str(work))
    work.mkdir(parents=True,exist_ok=True)
    digest=lambda:hashlib.sha256(mgr.read_bytes()).hexdigest()
    pinned=digest()
    sdk=args.sdk.resolve()
    source=sdk/"examples/view-info"
    manifest=json.loads((source/"plugin.json").read_bytes())
    manifest["id"]="external-"+uuid.uuid4().hex
    stage=work/manifest["id"];stage.mkdir()
    asset="资源/示例.txt"
    manifest["assets"]=[asset]
    marker=uuid.uuid4().hex[:12]
    implementation=work/(manifest["id"]+".c")
    example=(source/"view-info.c").read_text("utf8")
    implementation.write_text(example.replace('L"画布倍率 ', 'L"'+marker+' 画布倍率 '),"utf8")
    data=stage/"payload"/asset
    data.parent.mkdir(parents=True)
    data.write_bytes(b"standalone plugin asset")
    (stage/"plugin.json").write_text(json.dumps(manifest,ensure_ascii=False),"utf8")
    subprocess.run([args.zig,"cc","-target","x86-windows-gnu","-O2","-shared","-s","-I"+str(sdk),
                    implementation,sdk/"module.def","-o",stage/"ViewInfo.dll","-luser32"],check=True)
    archive=work/(manifest["id"]+".zip")
    sys.path.insert(0,str(sdk))
    from pack import pack
    pack(stage,archive)
    def cli(*params):
        return subprocess.run([mgr,params[0],root,*params[1:]],env={**os.environ,**env},check=True,capture_output=True).stdout
    assert manifest["id"].encode() not in cli("--list")
    with launch(mgr,None,env) as proc:
        frame=wait_for(lambda:next((w for w,c in windows(proc.pid) if "插件管理器" in caption(w)),0),"manager window")
        def control(text):
            return wait_for(lambda:next((w for w,c in windows(proc.pid,frame) if caption(w)==text),0),text)
        user.PostMessageW(control("从安装包安装…"),0xf5,0,0)
        dialog=wait_for(lambda:next((w for w,c in windows(proc.pid) if c=="#32770"),0),"file chooser")
        edit=wait_for(lambda:next((w for w,c in windows(proc.pid,dialog) if c=="Edit"),0),"file name edit")
        user.SendMessageW(edit,12,0,str(archive))
        user.PostMessageW(dialog,0x111,1,user.GetDlgItem(dialog,1))
        wait_for(lambda:not user.IsWindow(dialog),"file dialog did not close")
        apply=control("应用更改");wait_for(lambda:user.IsWindowEnabled(apply),"imported package not staged")
        user.PostMessageW(apply,0xf5,0,0)
        def installed():
            return (root/".stylos-plugins/active"/manifest["id"]/"module.dll").exists() and not user.IsWindowEnabled(apply)
        wait_for(installed,"Apply did not install the package",20)
        assert (root/".stylos-plugins/active"/manifest["id"]/asset).read_bytes()==data.read_bytes()
    def check_visible(expected):
        with launch(root/"Stylos.exe",args.document.resolve(),env) as proc:
            frame=wait_for(lambda:next((w for w,c in windows(proc.pid) if c=="DrawFrameWnd" and
                any(k=="CCelWindow" for _,k in windows(proc.pid,w))),0),"Stylos document canvas")
            api=user.RegisterWindowMessageW("RETAS.Stylos.DevKit.v1")
            page=user.RegisterWindowMessageW("RETAS.Stylos.Settings."+manifest["id"])
            wait_for(lambda:user.SendMessageW(frame,api,6,page),"preferences request")
            dialog=wait_for(lambda:next((w for w,c in windows(proc.pid) if c=="#32770" and user.GetDlgItem(w,1762)),0),"preferences")
            def visible():
                return any(c=="Static" and caption(w).startswith(marker+" 画布倍率 ") for w,c in windows(proc.pid,dialog))
            if expected:wait_for(visible,"external DLL did not render its settings page")
            else:time.sleep(.35);assert not visible()
    check_visible(True)
    cli("--disable",manifest["id"]);check_visible(False)
    cli("--enable",manifest["id"]);check_visible(True)
    cli("--uninstall",manifest["id"])
    assert manifest["id"].encode() not in cli("--list")
    assert not (root/".stylos-plugins/active"/manifest["id"]/"module.dll").exists()
    assert not (root/".stylos-plugins/active"/manifest["id"]/asset).exists()
    assert digest()==pinned
    print("PASS external GUI import, Apply, live page, disable, restart, re-enable and uninstall",
          manifest["id"],"manager SHA-256",pinned)


if __name__=="__main__":
    cli=argparse.ArgumentParser(description=__doc__)
    for name in ("folder","profile","document","work"):
        cli.add_argument("--"+name,type=Path,required=True)
    cli.add_argument("--zig",default="zig")
    cli.add_argument("--sdk",type=Path,default=ROOT/"retas-devkit")
    verify(cli.parse_args())
