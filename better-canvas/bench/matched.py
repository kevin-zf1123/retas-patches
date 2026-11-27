"""Match CSP's measured 1239 x 1146 document viewport and 59.5% scale."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import gzip
import json
from pathlib import Path
import time

from csp import sample
from desktop import launch, user, wait_for, windows


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("lab", type=Path)
    cli.add_argument("output", type=Path)
    cli.add_argument("--fast", action="store_true")
    args = cli.parse_args()
    lab = args.lab.resolve()
    ini = lab/"profile/CELSYS/RETAS STUDIO/Stylos/2.0/Preference/CanvasZoom.ini"
    ini.write_text(f"[Canvas]\nInterpolation=1\nFastNavigation={int(args.fast)}\n",encoding="ascii")
    env = dict(APPDATA=lab/"profile", LOCALAPPDATA=lab/"local", TEMP=lab/"tmp",
               TMP=lab/"tmp", __COMPAT_LAYER="HIGHDPIAWARE")
    with launch(lab/"Studio/Stylos/Stylos.exe",lab/"lines.bmp",env) as proc:
        frame = wait_for(lambda: next((w for w,c in windows(proc.pid) if c=="DrawFrameWnd"),0),"frame")
        canvas = wait_for(lambda: next((w for w,c in windows(proc.pid,frame) if c=="CCelWindow"
                        and user.GetDlgItem(w,0x6e40)),0),"canvas")
        if user.GetDpiForWindow(canvas) != user.GetDpiForSystem():
            raise RuntimeError("DPI virtualization changes the displayed scale")
        for _ in range(3):
            horizontal, vertical, size = W.RECT(), W.RECT(), W.RECT()
            user.GetWindowRect(user.GetDlgItem(canvas,100),C.byref(horizontal))
            user.GetWindowRect(user.GetDlgItem(canvas,101),C.byref(vertical))
            user.MapWindowPoints(0,canvas,C.byref(horizontal),2)
            user.MapWindowPoints(0,canvas,C.byref(vertical),2)
            view = (vertical.left,horizontal.top)
            if view == (1239,1146):
                break
            user.GetWindowRect(canvas,C.byref(size))
            user.SetWindowPos(canvas,0,0,0,size.right-size.left+1239-view[0],
                              size.bottom-size.top+1146-view[1],0x16)
            time.sleep(.25)
        else:
            raise RuntimeError(f"Viewport mismatch {view}")
        edit = user.GetDlgItem(canvas,121)
        user.SendMessageW(canvas,0x8000+0x394,round(.595*4096),0)
        time.sleep(.5)
        label = C.create_unicode_buffer(64)
        user.SendMessageW(edit,13,len(label),label)
        if not label.value.startswith("59.5"):
            raise RuntimeError("Scale mismatch: " + label.value)
        data = dict(viewport=view,scale=label.value,fast=args.fast)
        for kind in ("rotate","wheel"):
            data[kind] = [sample(canvas,(730,410),(610,540),kind,stylos=True) for _ in range(4)]
        with gzip.open(args.output,"wt",encoding="utf-8") as fp:
            json.dump(data,fp,separators=(",",":"))


if __name__ == "__main__":
    main()
