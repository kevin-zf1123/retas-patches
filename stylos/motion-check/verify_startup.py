"""Run on Windows: python verify_startup.py --zig <path to zig.exe>."""
import argparse
import ctypes
from ctypes import wintypes
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import time

HERE = Path(__file__).resolve().parent


def wait_for(predicate, seconds=20):
    until = time.monotonic() + seconds
    while time.monotonic() < until:
        if predicate():
            return
        time.sleep(0.05)
    raise AssertionError("Timed out waiting for scheduler event")


def run_case(folder, scenario):
    sheet = folder / 'scene' / 'case.sxf'
    log = folder / (scenario + '.log')
    env = dict(os.environ, STYLOS_QA_CASE=scenario, STYLOS_RENDER_TRACE=str(log),
               LOCALAPPDATA=str(folder / 'local'))
    proc = subprocess.Popen([folder / 'SchedulerQA.exe', sheet], cwd=folder, env=env,
                            creationflags=subprocess.CREATE_NO_WINDOW)

    def events():
        lines = log.read_bytes().splitlines() if log.exists() else []
        return [tuple(line.decode('ascii').split()) for line in lines if len(line.split()) == 5]

    def matches(kind, slot=None):
        return [e for e in events() if e[2] == kind and (slot is None or int(e[3]) == slot)]

    helper = None
    kernel = ctypes.WinDLL('kernel32', use_last_error=True)
    kernel.OpenProcess.argtypes = [wintypes.DWORD, wintypes.BOOL, wintypes.DWORD]
    kernel.OpenProcess.restype = wintypes.HANDLE
    kernel.WaitForSingleObject.argtypes = [wintypes.HANDLE, wintypes.DWORD]
    kernel.CloseHandle.argtypes = [wintypes.HANDLE]
    try:
        if scenario == 'fail':
            wait_for(lambda: len(matches('startup-exit', 0)) == 3)
            time.sleep(3.6)
            assert len(matches('start-lane', 0)) == 3 and not matches('start-lane', 1)
            return
        if scenario == 'timeout':
            wait_for(lambda: matches('startup-timeout', 0), 75)
            assert not matches('start-lane', 1)
            (folder / 'allow-timeout-retry').touch()
        if scenario == 'cancel':
            wait_for(lambda: matches('test-helper'))
            helper = kernel.OpenProcess(0x100000, False, int(matches('test-helper')[0][4]))
            assert helper and kernel.WaitForSingleObject(helper, 0) == 258
            (folder / 'allow-second').touch()
            (sheet.parent / 'autosaved.dga').write_bytes(b'saved')
        wait_for(lambda: len(matches('worker-exit')) == 2)
        rows = events()
        assert all(e[4] == '0' for e in matches('worker-exit'))
        assert len(matches('start-lane', 1)) == 1
        assert rows.index(matches('render-ready', 0)[0]) < rows.index(matches('start-lane', 1)[0])
        if helper:
            assert kernel.WaitForSingleObject(helper, 0) == 0
            assert len(matches('start-lane', 0)) == 2
            assert matches('render-ready', 0)[0][4] == matches('start-lane', 0)[1][4]
        if scenario == 'timeout':
            assert len(matches('start-lane', 0)) == 2
        if scenario == 'parallel':
            assert rows.index(matches('test-render', 1)[0]) < rows.index(matches('test-done', 0)[0])
    finally:
        if helper:
            kernel.CloseHandle(helper)
        proc.terminate()
        proc.wait(timeout=10)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--zig', default='zig')
    args = parser.parse_args()
    with tempfile.TemporaryDirectory() as temp:
        root = Path(temp)
        (root / 'local').mkdir()
        (root / 'scene').mkdir()
        (root / 'scene/case.sxf').touch()
        exe = root / 'SchedulerQA.exe'
        subprocess.run([args.zig, 'cc', '-target', 'x86-windows-gnu', '-municode', '-O2', HERE / 'verify_startup.c',
                        '-o', exe, '-lgdi32', '-luser32', '-ladvapi32', '-lcabinet'], check=True)
        for lane in 'AB':
            shutil.copyfile(exe, root / ('StylosRender' + lane + '.exe'))
        for scenario in ('parallel', 'fast', 'cancel', 'fail', 'timeout'):
            run_case(root, scenario)
            print('PASS:', scenario, flush=True)


if __name__ == '__main__':
    main()
