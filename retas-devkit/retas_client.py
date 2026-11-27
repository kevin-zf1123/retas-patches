"""Python client for the local Stylos DevKit v1. Standard library only."""
import argparse
import ctypes
from ctypes import wintypes

from pathlib import Path
import json
REGISTRY = json.loads(Path(__file__).with_name("entries.json").read_text(encoding="utf8"))
ENTRIES = {entry["name"]: entry["id"] for entry in REGISTRY["entries"]}
PAGES = {"general": 0, "canvas": 1, "midpoint": 2, "fast-navigation": 3}

def request(entry, arg=0, hwnd=None):
    u = ctypes.WinDLL("user32", use_last_error=True)
    u.FindWindowW.restype = wintypes.HWND
    hwnd = hwnd or u.FindWindowW("DrawFrameWnd", None)
    if not hwnd:
        raise RuntimeError("Stylos window unavailable")
    msg = u.RegisterWindowMessageW("RETAS.Stylos.DevKit.v1")
    u.SendMessageTimeoutW.argtypes = [wintypes.HWND, wintypes.UINT, wintypes.WPARAM, wintypes.LPARAM,
                                      wintypes.UINT, wintypes.UINT, ctypes.POINTER(ctypes.c_size_t)]
    result = ctypes.c_size_t()
    if not u.SendMessageTimeoutW(hwnd,msg,ENTRIES[entry],arg,2,1000,ctypes.byref(result)):
        raise OSError("Stylos did not respond")
    return ctypes.c_int32(result.value).value if entry=="rotation" else result.value

if __name__ == "__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("entry",choices=ENTRIES)
    parser.add_argument("--page",default="general",help="Plugin ID or legacy page alias")
    args=parser.parse_args()
    if request("version")!=1:raise SystemExit("Stylos DevKit v1 not registered")
    page=PAGES.get(args.page)
    if page is None:
        plugin_id = "motion-check" if args.page == "motion" else args.page
        page = ctypes.windll.user32.RegisterWindowMessageW("RETAS.Stylos.Settings."+plugin_id)
    print(request(args.entry,page if args.entry=="preferences" else 0))
