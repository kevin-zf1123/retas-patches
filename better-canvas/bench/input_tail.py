"""Measure how long the native angle/zoom field continues changing after a fixed-rate input burst."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import json
from pathlib import Path
import time

from desktop import launch, user, wait_for, windows
from csp import Patch


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("lab", type=Path)
    cli.add_argument("document", type=Path)
    cli.add_argument("output", type=Path)
    cli.add_argument("--rates", type=int, nargs="+", default=[30, 120])
    cli.add_argument("--kinds", nargs="+", choices=("key","wheel","rotated-wheel"),
                     default=["key","wheel","rotated-wheel"])
    cli.add_argument("--check-boundaries", action="store_true",
                     help="Check key repeat counts, direction changes, fractional wheel and scale limits")
    args = cli.parse_args()
    lab = args.lab.resolve()
    env = dict(APPDATA=lab/"profile", LOCALAPPDATA=lab/"local", TEMP=lab/"tmp", TMP=lab/"tmp")
    rows = []
    with launch(lab/"Studio/Stylos/Stylos.exe", args.document.resolve(), env) as process:
        frame = wait_for(lambda: next((w for w,c in windows(process.pid) if c=="DrawFrameWnd"),0),"frame")
        canvas = wait_for(lambda: next((w for w,c in windows(process.pid, frame) if c=="CCelWindow"
                          and user.GetDlgItem(w,0x6e41)),0),"canvas")
        def field(control):
            buf = C.create_unicode_buffer(32)
            user.SendMessageW(user.GetDlgItem(canvas,control),13,len(buf),buf)
            return float(buf.value.split()[0])
        def post(msg, wp, lp):
            if not user.PostMessageW(canvas,msg,wp,lp):
                raise C.WinError()
        point=W.POINT(310,230)
        user.ClientToScreen(canvas,C.byref(point))
        assert user.WindowFromPoint(point)==canvas
        anchor=(point.x & 65535) | ((point.y & 65535)<<16)
        user.SendMessageW(frame,0x111,40138,0)
        post(0x20a,480<<16,anchor)
        wait_for(lambda: field(121)==120,"initial zoom")
        def keys(direction, repeat=1):
            vk, scan = (0xbb,0x0d) if direction>0 else (0xbd,0x0c)
            post(0x100,vk,scan<<16 | repeat)
            post(0x101,vk,0xc0000001 | scan<<16)
        patch=Patch(canvas,310,230,192)
        for rate in args.rates:
            for kind in args.kinds:
                if kind=="rotated-wheel":
                    for _ in range(5):keys(1)
                    wait_for(lambda:field(122)==25,"rotated wheel setup")
                    time.sleep(.4)
                for direction in (1,-1,1,-1,1,-1):
                    ctrl=122 if kind=="key" else 121
                    start=field(ctrl)
                    goal=start+direction*120
                    initial=patch.read()
                    first=time.perf_counter()
                    for i in range(24):
                        time.sleep(max(0,first+i/rate-time.perf_counter()))
                        if kind=="key":keys(direction)
                        else:post(0x20a,(direction*120)<<16,anchor)
                    release=time.perf_counter()
                    geometry=None
                    samples=[]
                    while geometry is None or time.perf_counter()-geometry < .55:
                        now=time.perf_counter()
                        if now-release>10:raise RuntimeError(f"{kind}: {start} -> {goal}, got {field(ctrl)}")
                        if geometry is None and field(ctrl)==goal:
                            geometry=time.perf_counter()
                        digest=patch.read()
                        samples.append((time.perf_counter(),digest))
                        time.sleep(.005)
                    final=samples[-1][1]
                    last_change=max((i+1 for i,(_,d) in enumerate(samples) if d!=final),default=0)
                    rows.append(dict(kind=kind,rate=rate,goal=goal,
                                     tail_ms=round((geometry-release)*1000,2),
                                     settled_pixels_ms=round((samples[last_change][0]-release)*1000,2)
                                     if initial!=final else None))
                if kind=="rotated-wheel":
                    for _ in range(5):keys(-1)
                    wait_for(lambda:field(122)==0,"restored rotation")
                    time.sleep(.4)
        patch.close()
        keys(1,3)
        time.sleep(.6)
        repeat_angle=field(122)
        if args.check_boundaries:
            assert repeat_angle==15
            for _ in range(20):
                keys(-1)
                keys(1)
            # Switch from rotation to different wheel anchors and back before draining.
            keys(-1,4)
            for _ in range(8):
                post(0x20a,15<<16,anchor)
            post(0x20a,120<<16,anchor+1)
            keys(1,2)
            wait_for(lambda:field(122)==5 and field(121)==130,"mixed navigation lost a step")
            for _ in range(180):
                post(0x20a,120<<16,anchor)
            post(0x20a,-120<<16,anchor)
            wait_for(lambda:field(121)==995,"direction change at maximum zoom")
            for _ in range(205):
                post(0x20a,-120<<16,anchor)
            post(0x20a,120<<16,anchor)
            wait_for(lambda:field(121)==15,"direction change at minimum zoom")
    args.output.write_text(json.dumps(dict(bursts=rows,repeat_angle=repeat_angle),indent=2),"utf8")
    for kind in args.kinds:
        for rate in args.rates:
            times=sorted(row["tail_ms"] for row in rows if row["kind"]==kind and row["rate"]==rate)
            print(kind,rate,"ms",times)
    print("one key message with repeat count 3:",repeat_angle)


if __name__=="__main__":
    main()
