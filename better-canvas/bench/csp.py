"""External CSP drag probe. Run inside the disposable desktop used by desktop.launch."""
import ctypes as C
from ctypes import wintypes as W
import hashlib
import struct
import time

from desktop import user


class Patch:
    """Read a small portion of the window DC. Polling cost is measured separately."""
    def __init__(self, hwnd, x, y, size=128):
        self.hwnd, self.size, self.x, self.y = hwnd, size, x, y
        self.gdi = gdi = C.windll.gdi32
        self.dc = dc = user.GetDC(hwnd)
        self.mem = gdi.CreateCompatibleDC(dc)
        info = C.create_string_buffer(struct.pack("<IiiHHIIIIII", 40, size, -size, 1, 32, 0, 0, 0, 0, 0, 0))
        self.bits = C.c_void_p()
        self.bmp = gdi.CreateDIBSection(dc, info, 0, C.byref(self.bits), None, 0)
        if not self.bmp:
            raise C.WinError()
        self.previous = gdi.SelectObject(self.mem, self.bmp)

    def read(self):
        if not self.gdi.BitBlt(self.mem, 0, 0, self.size, self.size, self.dc, self.x, self.y, 0xcc0020):
            raise RuntimeError("BitBlt failed")
        self.gdi.GdiFlush()
        return hashlib.blake2b(C.string_at(self.bits, self.size**2 * 4), digest_size=12).hexdigest()

    def close(self):
        self.gdi.SelectObject(self.mem, self.previous)
        self.gdi.DeleteObject(self.bmp)
        self.gdi.DeleteDC(self.mem)
        user.ReleaseDC(self.hwnd, self.dc)


def sample(hwnd, point, patch, kind, stylos=False):
    """One observed update per input, with a 16 ms minimum inter-input interval."""
    x, y = point
    wheel = kind == "wheel"
    observation = Patch(hwnd, *patch)
    def send(msg, flags, dx, dy):
        px, py = x + dx, y + dy
        if msg == 0x20a:
            pos = W.POINT(px, py)
            user.ClientToScreen(hwnd, C.byref(pos))
            px, py = pos.x, pos.y
        if not user.PostMessageW(hwnd, msg, flags, (px & 65535) | ((py & 65535) << 16)):
            raise C.WinError()
    try:
        baseline = observation.read()
        samples, reads = [], []
        previous = baseline
        if not wheel and not stylos:
            send(0x201, 1, 0, 0)
            time.sleep(.03)
        for step in list(range(1, 13)) + list(range(11, -1, -1)):
            start = time.perf_counter_ns()
            if stylos and kind == "rotate":
                button = 0x6e41 if len(samples) < 12 else 0x6e40
                user.PostMessageW(hwnd, 0x111, button, user.GetDlgItem(hwnd, button))
            elif wheel:
                send(0x20a, (120 if len(samples) % 4 < 2 else -120) << 16, 0, 0)
            else:
                send(0x200, 1, step * 8, step * 11 if kind == "rotate" else 0)
            while True:
                begin = time.perf_counter_ns()
                current = observation.read()
                end = time.perf_counter_ns()
                reads.append(end - begin)
                if current != previous or end - start > 150_000_000:
                    break
                time.sleep(.001)
            samples.append((start, end, current != previous))
            previous = current
            time.sleep(max(0, .016 - (end - start) * 1e-9))
        if not wheel and not stylos:
            send(0x202, 0, 0, 0)
        release = time.perf_counter_ns()
        tail = []
        for _ in range(24):
            digest = observation.read()
            tail.append((time.perf_counter_ns(), digest))
            time.sleep(.006)
        return dict(samples=samples, reads=reads, release=release, tail=tail, held=previous, initial=baseline)
    finally:
        observation.close()


def main():
    import argparse
    import gzip
    import json
    from pathlib import Path
    from desktop import capture, launch, wait_for, windows
    cli = argparse.ArgumentParser(description="CSP 3.0 Japanese UI, 2560 px display at 150% DPI")
    cli.add_argument("lab", type=Path)
    cli.add_argument("app", type=Path)
    cli.add_argument("output", type=Path)
    args = cli.parse_args()
    lab = args.lab.resolve()
    data = {}
    with launch(args.app.resolve(), lab/"lines.bmp", dict(APPDATA=lab/"profile", LOCALAPPDATA=lab/"local",
                TMP=lab/"tmp", TEMP=lab/"tmp")) as proc:
        def matching(predicate):
            for hwnd, cls in windows(proc.pid):
                if cls in ("Window", "SysShadow"):
                    continue
                r = W.RECT()
                user.GetClientRect(hwnd, C.byref(r))
                if predicate(r.right, r.bottom):
                    return hwnd
        frame = wait_for(lambda: matching(lambda w,h: w == 2560 and h > 1300), "CSP workspace", 35)
        time.sleep(6)

        def click(w, x, y):
            user.PostMessageW(w, 0x201, 1, x | y << 16)
            time.sleep(.05)
            user.PostMessageW(w, 0x202, 0, x | y << 16)
            time.sleep(.25)

        def set_fast(desired):
            click(frame, 60, 52)
            menu = wait_for(lambda: matching(lambda w,h: 500 <= w <= 525 and 1090 <= h <= 1130), "CSP File menu")
            click(menu, 104, 675)
            dialog = wait_for(lambda: matching(lambda w,h: 1080 <= w <= 1100 and 745 <= h <= 775), "Preferences")
            click(dialog, 80, 66)  # Tool
            def checked():
                pixels = capture(dialog).crop((231, 378, 249, 394)).convert("L")
                return sum(pixels.histogram()[185:]) > 8
            old = checked()
            if old != desired:
                click(dialog, 240, 386)
                if checked() != desired:
                    raise RuntimeError("Fast view checkbox did not change")
            click(dialog, 1000, 68)
            return old

        initial = set_fast(False)
        try:
            capture(frame).save(lab/"csp-workspace.png")
            for fast in (False, True):
                if fast:
                    set_fast(True)
                for kind in ("rotate", "zoom", "wheel"):
                    key = ord("R" if kind == "rotate" else "Z")
                    user.PostMessageW(frame, 0x100, key, 1)
                    user.PostMessageW(frame, 0x101, key, 0xc0000001)
                    time.sleep(.2)
                    from counters import GpuEngines, hard_faults
                    gpu = GpuEngines(proc.pid)
                    runs = []
                    try:
                        for _ in range(2 if kind == "wheel" else 4):
                            before = [gpu.read(), hard_faults(proc.pid)]
                            run = sample(frame, (1650,550), (1530,680), kind)
                            run["resources"] = [before, [gpu.read(), hard_faults(proc.pid)]]
                            runs.append(run)
                    finally:
                        gpu.close()
                    data[f"{int(fast)}-{kind}"] = runs
        finally:
            set_fast(initial)
        with gzip.open(args.output, "wt", encoding="utf-8") as fp:
            json.dump(data, fp, separators=(",", ":"))


if __name__ == "__main__":
    main()
