"""Read process counters outside the timed navigation sequence."""
import ctypes as C
from ctypes import wintypes as W


class UnicodeString(C.Structure):
    _fields_ = [("length", W.WORD), ("capacity", W.WORD), ("text", W.LPVOID)]


class ProcessInfo(C.Structure):
    # Prefix of Windows SYSTEM_PROCESS_INFORMATION only.
    _fields_ = [("next",W.DWORD), ("threads",W.DWORD), ("private_ws",C.c_longlong),
                ("hard_faults",W.DWORD), ("peak_threads",W.DWORD), ("cycles",C.c_ulonglong),
                ("created",C.c_longlong), ("user",C.c_longlong), ("kernel",C.c_longlong),
                ("image",UnicodeString), ("priority",W.LONG), ("pid",W.HANDLE)]


def hard_faults(pid):
    size = 1 << 20
    needed = W.DWORD()
    while size <= 64 << 20:
        buf = C.create_string_buffer(size)
        status = C.windll.ntdll.NtQuerySystemInformation(5, buf, size, C.byref(needed)) & 0xffffffff
        if status == 0xc0000004:
            size = max(size * 2, needed.value + 4096)
            continue
        if status:
            return None
        offset = 0
        while offset + C.sizeof(ProcessInfo) <= needed.value:
            item = ProcessInfo.from_buffer(buf, offset)
            if item.pid == pid:
                return item.hard_faults
            if not item.next:
                break
            offset += item.next
        return None


class Raw(C.Structure):
    _fields_ = [("status",W.DWORD), ("time",W.FILETIME), ("first",C.c_longlong),
                ("second",C.c_longlong), ("count",W.DWORD)]


class Item(C.Structure):
    _fields_ = [("name",W.LPWSTR), ("value",Raw)]


class GpuEngines:
    def __init__(self, pid):
        self.prefix = f"pid_{pid}_"
        self.pdh = C.windll.pdh
        self.query = W.HANDLE()
        self.counter = W.HANDLE()
        self.enabled = self.pdh.PdhOpenQueryW(None,0,C.byref(self.query)) == 0 and (
            self.pdh.PdhAddEnglishCounterW(self.query,r"\GPU Engine(*)\Running Time",0,C.byref(self.counter)) == 0)

    def read(self):
        if not self.enabled or self.pdh.PdhCollectQueryData(self.query):
            return None
        size, count = W.DWORD(), W.DWORD()
        get = self.pdh.PdhGetRawCounterArrayW
        get(self.counter,C.byref(size),C.byref(count),None)
        if not size.value:
            return None
        buf = C.create_string_buffer(size.value)
        if get(self.counter,C.byref(size),C.byref(count),buf):
            return None
        items = C.cast(buf,C.POINTER(Item))
        return {i.name:i.value.first for i in items[:count.value]
                if i.name.startswith(self.prefix) and i.value.status in (0,1)}

    def close(self):
        if self.query:
            self.pdh.PdhCloseQuery(self.query)
