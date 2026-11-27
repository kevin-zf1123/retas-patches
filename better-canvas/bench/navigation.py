"""Measure a disposable, instrumented Stylos installation. See RESULTS.md."""
import argparse
import ctypes as C
from ctypes import wintypes as W
import gzip
import hashlib
import json
import mmap
import os
from pathlib import Path
import struct
import time

from desktop import capture, kernel, launch, user, wait_for, windows

record = struct.Struct("<qiiii")
frequency = C.c_longlong()
kernel.QueryPerformanceFrequency(C.byref(frequency))


def clock():
    value = C.c_longlong()
    kernel.QueryPerformanceCounter(C.byref(value))
    return value.value


def memory(process):
    # GetProcessMemoryInfo returns total faults, including soft faults.
    class Counters(C.Structure):
        _fields_ = [("cb", W.DWORD), ("faults", W.DWORD)] + [
            (n, C.c_size_t) for n in ("peakWS", "ws", "peakPaged", "paged",
                                      "peakNonpaged", "nonpaged", "commit", "peakCommit", "private")]
    class Region(C.Structure):
        _fields_ = [("base", W.LPVOID), ("alloc", W.LPVOID), ("prot", W.DWORD),
                    ("size", C.c_size_t), ("state", W.DWORD), ("protect", W.DWORD), ("type", W.DWORD)]
    counters = Counters()
    counters.cb = C.sizeof(counters)
    handle = W.HANDLE(process.handle)
    if not C.windll.psapi.GetProcessMemoryInfo(handle, C.byref(counters), counters.cb):
        raise C.WinError()
    region = Region()
    addr = largest = 0
    while addr < 2**32 and kernel.VirtualQueryEx(handle, C.c_void_p(addr), C.byref(region), C.sizeof(region)):
        end = (region.base or 0) + region.size
        if end <= addr:
            raise RuntimeError("Invalid memory region")
        if region.state == 0x10000:
            largest = max(largest, min(end, 2**32) - addr)
        addr = end
    times = [C.c_ulonglong() for _ in range(4)]
    if not kernel.GetThreadTimes(W.HANDLE(process.thread), *(C.byref(t) for t in times)):
        raise C.WinError()
    return dict(private=counters.private, faults=counters.faults, largest_free=largest,
                ui_cpu_100ns=times[2].value + times[3].value)


def measure(args, shared, process, engines=None):
    def find_canvas():
        for frame, cls in windows(process.pid):
            if cls == "DrawFrameWnd":
                for child, kind in windows(process.pid, frame):
                    if kind == "CCelWindow" and user.GetDlgItem(child, 0x6e40):
                        return frame, child
    try:
        frame, canvas = wait_for(find_canvas, "missing editable canvas or rotation buttons")
    except RuntimeError:
        print("Visible windows:", windows(process.pid))
        raise

    def field(code):
        buf = C.create_unicode_buffer(64)
        user.SendMessageW(user.GetDlgItem(canvas, code), 13, len(buf), buf)
        return buf.value

    def emit(kind, direction):
        if kind == "rotate":
            button = 0x6e41 if direction > 0 else 0x6e40
            ok = user.PostMessageW(canvas, 0x111, button, user.GetDlgItem(canvas, button))
        else:
            ok = user.PostMessageW(canvas, 0x20a, (direction * 120) << 16, point)
        if not ok:
            raise C.WinError()

    user.SendMessageW(frame, 0x111, 40138, 0)  # 100%
    time.sleep(.4)
    rect = W.RECT()
    user.GetClientRect(canvas, C.byref(rect))
    for j in range(4, 18):
        for i in range(4, 18):
            pt = W.POINT(rect.right * i // 20, rect.bottom * j // 20)
            user.ClientToScreen(canvas, C.byref(pt))
            if user.WindowFromPoint(pt) == canvas:
                break
        else:
            continue
        break
    else:
        raise RuntimeError("no visible wheel anchor")
    point = (pt.x & 65535) | ((pt.y & 65535) << 16)
    for _ in range(args.duplicate_layers):
        user.SendMessageW(frame, 0x111, 40265, 0)  # Cel > Layer > Duplicate
    if args.reference:
        user.PostMessageW(frame, 0x111, 40223, 0)  # Light Table > Register
        dialog = wait_for(lambda: next((w for w,c in windows(process.pid) if c == "#32770"), 0),
                          "registration dialog")
        edit = wait_for(lambda: next((w for w,c in windows(process.pid, dialog) if c == "Edit"), 0),
                        "reference file name")
        user.SendMessageW(edit, 12, 0, str(args.reference.resolve()))
        user.PostMessageW(dialog, 0x111, 1, user.GetDlgItem(dialog, 1))
        wait_for(lambda: not user.IsWindow(dialog), "reference registration failed")
        time.sleep(.3)
        user.SendMessageW(frame, 0x111, 40194, 0)  # Show all
        time.sleep(.2)
        shown = capture(canvas)
        user.SendMessageW(frame, 0x111, 40195, 0)
        time.sleep(.2)
        hidden = capture(canvas)
        user.SendMessageW(frame, 0x111, 40194, 0)
        if hidden.tobytes() == shown.tobytes():
            raise RuntimeError("Reference had no visible effect")
    for _ in range(4):
        emit("zoom", 1)
        time.sleep(.12)
    wait_for(lambda: field(121).startswith("120"), "wheel setup failed")
    time.sleep(.3)
    runs = []
    shots = {}
    for kind in ("rotate", "zoom"):
        for burst in range(args.bursts):
            start_view = [field(121), field(122)]
            before = memory(process)
            if engines:
                from counters import hard_faults
                before.update(hard_faults=hard_faults(process.pid), gpu=engines.read())
            index = struct.unpack_from("<I", shared)[0]
            posted = []
            # Alternating four-input runs return to the same final transform.
            for n in range(16):
                posted.append(clock())
                emit(kind, 1 if n % 8 < 4 else -1)
                time.sleep(.016)
            wait_for(lambda: sum(r[1] == 13 for r in events(shared, index)) >= 16,
                     "input was lost or instrumentation was unavailable")
            time.sleep(.45)
            trace = events(shared, index)
            if len({r[4] for r in trace}) != 1:
                raise RuntimeError("multiple trace producers")
            final_view = [field(121), field(122)]
            if final_view != start_view:
                raise RuntimeError(f"view changed after return: {start_view} -> {final_view}")
            after = memory(process)
            if engines:
                after.update(hard_faults=hard_faults(process.pid), gpu=engines.read())
            runs.append(dict(kind=kind, burst=burst, posted=posted, events=trace,
                             before=before, after=after, view=final_view))
        # A nontrivial angle/scale avoids comparing two unfiltered identity views.
        emit(kind, 1)
        time.sleep(.5)
        image = capture(canvas)
        cropped = image.crop((18, 48, image.width - 32, image.height - 48))
        if cropped.getextrema() == ((0, 0),) * 3:
            raise RuntimeError("blank canvas capture")
        shots[kind] = hashlib.sha256(cropped.tobytes()).hexdigest()
        image.save(args.output.with_name(args.output.stem + "-" + kind + ".png"))
        emit(kind, -1)
        time.sleep(.5)
    validation = {}
    if args.validate:
        from PIL import ImageChops
        # Establish a known filtered image at +5 degrees. Return to it and press
        # the pen immediately, before the refinement timer can fire.
        emit("rotate", 1)
        time.sleep(.4)
        x, y = rect.right//2, rect.bottom//2
        roi = (x-20, y-20, x+64, y+48)
        before = capture(canvas).crop(roi)
        emit("rotate", -1)
        time.sleep(.2)
        first_point = struct.unpack_from("<I", shared)[0]
        emit("rotate", 1)
        for msg, buttons, dx, dy in [(0x201,1,0,0), (0x200,1,12,6), (0x200,1,24,12),
                                     (0x200,1,36,18), (0x202,0,36,18)]:
            user.PostMessageW(canvas, msg, buttons, (x+dx) | ((y+dy)<<16))
            time.sleep(.005)
        time.sleep(.4)
        ink = capture(canvas).crop(roi)
        validation["stroke_bbox"] = ImageChops.difference(before, ink).getbbox()
        validation["first_point_256"] = [r[2:4] for r in events(shared, first_point) if r[1] == 17]
        validation["stroke_hash"] = hashlib.sha256(ink.tobytes()).hexdigest()
        if validation["stroke_bbox"]:
            user.SendMessageW(frame, 0x111, 40007, 0)
            time.sleep(.35)
            validation["undo_restored"] = before.tobytes() == capture(canvas).crop(roi).tobytes()
        # Exercise an active native selection while navigating.
        user.SendMessageW(frame, 0x111, 40137, 0)  # Fit the selection boundary in the viewport
        time.sleep(.5)
        normal = capture(canvas, presented=True)
        user.SendMessageW(frame, 0x111, 40025, 0)  # Select all
        time.sleep(.3)
        validation["selection_bbox"] = ImageChops.difference(normal, capture(canvas, presented=True)).getbbox()
        emit("zoom", 1)
        time.sleep(.35)
        validation["selection_view"] = [field(121),field(122)]
        emit("zoom", -1)
        time.sleep(.3)
        user.SendMessageW(frame, 0x111, 40026, 0)
        time.sleep(.3)
        user.SendMessageW(frame, 0x111, 40137, 0)
        time.sleep(.4)
        validation["deselect_restored"] = normal.tobytes() == capture(canvas, presented=True).tobytes()
    return dict(frequency=frequency.value, viewport=[rect.right, rect.bottom], mode=args.mode,
                fast=args.fast, duplicate_layers=args.duplicate_layers, reference=bool(args.reference),
                captures=shots, validation=validation, runs=runs)


def events(shared, start):
    end, overflow = struct.unpack_from("<ii", shared)
    if overflow:
        raise RuntimeError("trace buffer full")
    return list(record.iter_unpack(shared[8 + start * record.size:8 + end * record.size]))


def main():
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("lab", type=Path)
    cli.add_argument("document", type=Path)
    cli.add_argument("mode", type=int, choices=(0, 1, 2))
    cli.add_argument("output", type=Path)
    cli.add_argument("--bursts", type=int, default=4)
    cli.add_argument("--fast", action="store_true")
    cli.add_argument("--duplicate-layers", type=int, default=0)
    cli.add_argument("--reference", type=Path)
    cli.add_argument("--validate", action="store_true")
    cli.add_argument("--resources", action="store_true")
    args = cli.parse_args()
    args.lab = args.lab.resolve()
    profile = args.lab / "profile"
    ini = profile / "CELSYS/RETAS STUDIO/Stylos/2.0/Preference/CanvasZoom.ini"
    ini.parent.mkdir(parents=True, exist_ok=True)
    ini.write_text(f"[Canvas]\nInterpolation={args.mode}\nFastNavigation={int(args.fast)}\n", encoding="ascii")
    app = (args.lab / "Studio/Stylos/Stylos.exe").resolve()
    name = "Local\\StylosNavQA_" + str(os.getpid())
    with mmap.mmap(-1, 8 + 262144 * record.size, tagname=name) as sh:
        env = dict(APPDATA=profile, LOCALAPPDATA=args.lab/"local", TEMP=args.lab/"tmp",
                   TMP=args.lab/"tmp", STYLOS_NAV_TRACE=name)
        with launch(app, args.document.resolve(), env) as process:
            from counters import GpuEngines
            engines = GpuEngines(process.pid) if args.resources else None
            try:
                data = measure(args, sh, process, engines)
            finally:
                if engines:
                    engines.close()
        data["document_sha256"] = hashlib.sha256(args.document.read_bytes()).hexdigest()
        with gzip.open(args.output, "wt", encoding="utf-8") as fp:
            json.dump(data, fp, separators=(",", ":"))
    print(args.output, data["viewport"], data["captures"], flush=True)


if __name__ == "__main__":
    main()
