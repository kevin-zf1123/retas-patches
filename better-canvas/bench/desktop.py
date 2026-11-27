"""Win32 helpers for running a benchmark process on a private desktop."""
import ctypes as C
from ctypes import wintypes as W
import os
import subprocess
import time
from contextlib import contextmanager

user = C.WinDLL("user32", use_last_error=True)
kernel = C.WinDLL("kernel32", use_last_error=True)
callback = C.WINFUNCTYPE(W.BOOL, W.HWND, W.LPARAM)


class Startup(C.Structure):
    _fields_ = ([("cb", W.DWORD), ("reserved", W.LPWSTR), ("desktop", W.LPWSTR),
                 ("title", W.LPWSTR)] + [(name, W.DWORD) for name in
                 ("x", "y", "width", "height", "columns", "rows", "fill", "flags")] +
                [("show", W.WORD), ("unused", W.WORD), ("data", W.LPVOID),
                 ("stdin", W.HANDLE), ("stdout", W.HANDLE), ("stderr", W.HANDLE)])


class Process(C.Structure):
    _fields_ = [("handle", W.HANDLE), ("thread", W.HANDLE),
                ("pid", W.DWORD), ("tid", W.DWORD)]


@contextmanager
def launch(app, document, environment):
    name = "CanvasNavigationQA_" + str(os.getpid())
    user.CreateDesktopW.restype = W.HANDLE
    handle = user.CreateDesktopW(name, None, None, 0, 0x1ff, None)
    if not handle or not user.SetThreadDesktop(W.HANDLE(handle)):
        raise C.WinError(C.get_last_error())
    user.SetProcessDPIAware()
    env = {**os.environ, **environment}
    block = "\0".join(f"{k}={v}" for k, v in sorted(env.items(), key=lambda p: p[0].upper())) + "\0\0"
    info = Startup()
    info.cb, info.desktop = C.sizeof(info), name
    process = Process()
    argv = [str(app)] if document is None else [str(app), str(document)]
    args = C.create_unicode_buffer(subprocess.list2cmdline(argv))
    if not kernel.CreateProcessW(str(app), args, None, None, False, 0x400, C.create_unicode_buffer(block),
                                 str(app.parent), C.byref(info), C.byref(process)):
        raise C.WinError(C.get_last_error())
    try:
        yield process
    finally:
        # Only the process created above, with a disposable document and profile.
        kernel.TerminateProcess(W.HANDLE(process.handle), 0)
        kernel.WaitForSingleObject(W.HANDLE(process.handle), 5000)
        kernel.CloseHandle(W.HANDLE(process.thread))
        kernel.CloseHandle(W.HANDLE(process.handle))


def windows(pid, parent=None):
    result = []

    @callback
    def visit(hwnd, unused):
        owner = W.DWORD()
        user.GetWindowThreadProcessId(hwnd, C.byref(owner))
        if owner.value == pid and user.IsWindowVisible(hwnd):
            buf = C.create_unicode_buffer(128)
            user.GetClassNameW(hwnd, buf, len(buf))
            result.append((hwnd, buf.value))
        return True

    if parent:
        user.EnumChildWindows(parent, visit, 0)
    else:
        user.EnumWindows(visit, 0)
    return result


def wait_for(fn, message, timeout=12):
    end = time.monotonic() + timeout
    while time.monotonic() < end:
        value = fn()
        if value:
            return value
        time.sleep(0.01)
    raise RuntimeError(message)


def capture(hwnd, presented=False):
    """RGB client image from the window surface or PrintWindow, outside timed samples."""
    from PIL import Image
    import struct
    rect = W.RECT()
    user.GetClientRect(hwnd, C.byref(rect))
    w, h = rect.right, rect.bottom
    gdi = C.windll.gdi32
    dc = user.GetDC(hwnd)
    mem = gdi.CreateCompatibleDC(dc)
    bitmap = gdi.CreateCompatibleBitmap(dc, w, h)
    previous = gdi.SelectObject(mem, bitmap)
    try:
        copied = gdi.BitBlt(mem, 0, 0, w, h, dc, 0, 0, 0xcc0020) if presented else user.PrintWindow(hwnd, mem, 1)
        if not copied:
            raise RuntimeError("Canvas capture failed")
        gdi.SelectObject(mem, previous)
        info = C.create_string_buffer(struct.pack("<IiiHHIIIIII", 40, w, -h, 1, 32, 0, 0, 0, 0, 0, 0))
        pixels = C.create_string_buffer(w * h * 4)
        if gdi.GetDIBits(mem, bitmap, 0, h, pixels, info, 0) != h:
            raise RuntimeError("GetDIBits failed")
        return Image.frombytes("RGB", (w, h), pixels.raw, "raw", "BGRX")
    finally:
        gdi.SelectObject(mem, previous)
        gdi.DeleteObject(bitmap)
        gdi.DeleteDC(mem)
        user.ReleaseDC(hwnd, dc)
