#include <windows.h>
#include <stdint.h>

/* Single producer (the canvas UI thread). Publish each completed record. */
typedef struct { int64_t time; int32_t kind, a, b, tid; } Event;
typedef struct { LONG count, overflow; Event events[262144]; } Trace;

void CanvasTrace(unsigned event, int a, int b)
{
    static Trace *trace;
    if (!trace) {
        WCHAR name[96];
        if (!GetEnvironmentVariableW(L"STYLOS_NAV_TRACE", name, ARRAYSIZE(name))) return;
        HANDLE map = OpenFileMappingW(FILE_MAP_WRITE, FALSE, name);
        if (!map) return;
        trace = MapViewOfFile(map, FILE_MAP_WRITE, 0, 0, sizeof(*trace));
        CloseHandle(map);
        if (!trace) return;
    }
    LONG index = trace->count;
    if ((unsigned)index >= ARRAYSIZE(trace->events)) {
        InterlockedExchange(&trace->overflow, 1);
        return;
    }
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    trace->events[index] = (Event){now.QuadPart, event, a, b, GetCurrentThreadId()};
    InterlockedExchange(&trace->count, index + 1);
}
