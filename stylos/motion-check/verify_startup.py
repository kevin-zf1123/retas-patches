"""Exercise startup, save cancellation and bounded retry on Windows."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent


def wait_for(fn, seconds=35):
    deadline = time.monotonic() + seconds
    while time.monotonic() < deadline:
        if fn():
            return
        time.sleep(0.05)
    raise AssertionError("Timed out waiting for the pre-render scheduler")


def check(root, case):
    sheet = root / "scene/case.sxf"
    log = root / (case + ".log")
    env = dict(os.environ, STYLOS_RENDER_TRACE=str(log), LOCALAPPDATA=str(root))
    if case == "fail":
        env["STYLOS_QA_FAIL"] = "1"
    proc = subprocess.Popen([root / "SchedulerQA.exe", sheet], creationflags=subprocess.CREATE_NO_WINDOW, env=env)

    def events(kind):
        lines = log.read_text().splitlines() if log.exists() else []
        return [int(parts[4]) for line in lines if len(parts := line.split()) == 5 and parts[2] == kind]

    try:
        wait_for(lambda: events("test-start"))
        if case == "fail":
            wait_for(lambda: len(events("worker-exit")) == 3)
            time.sleep(3)
            assert events("worker-exit") == [21, 21, 21] and len(events("start-worker")) == 3
        else:
            if case == "save":
                (sheet.parent / "changed.dga").write_bytes(b"saved")
            wait_for(lambda: 0 in events("worker-exit"))
            assert events("worker-exit") == ([23, 0] if case == "save" else [0])
            assert len(events("test-complete")) == 1
        print("PASS:", case, flush=True)
    finally:
        proc.terminate()
        proc.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--zig", default="zig")
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        (root / "scene").mkdir()
        (root / "scene/case.sxf").touch()
        exe = root / "SchedulerQA.exe"
        subprocess.run([args.zig, "cc", "-target", "x86-windows-gnu", "-municode", "-O2",
                        str(HERE / "verify_startup.c"), "-o", str(exe), "-lgdi32", "-luser32",
                        "-ladvapi32", "-lcabinet"], check=True)
        shutil.copyfile(exe, root / "StylosRender64.exe")
        for case in ("complete", "save", "fail"):
            check(root, case)


if __name__ == "__main__":
    main()
