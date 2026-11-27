#include <windows.h>
#include <string.h>

#define EXPORT __declspec(dllexport)
#define TABLET_SYSTEM_CONTEXT 4
#define STYLOS_PACKET_FIELDS 0x15e2

typedef struct {
    char name[40];
    int options, status, locks, message_base, device, rate;
    int packet_data, packet_mode, move_mask, buttons_down, buttons_up;
    int in_x, in_y, in_z, in_width, in_height, in_depth;
    int out_x, out_y, out_z, out_width, out_height, out_depth;
    int sensitivity_x, sensitivity_y, sensitivity_z;
    int system_mode, system_x, system_y, system_width, system_height;
    int system_sensitivity_x, system_sensitivity_y;
} LogContextA;

typedef UINT (WINAPI *GetInfo)(UINT, UINT, void *);
typedef HANDLE (WINAPI *OpenContext)(HWND, LogContextA *, BOOL);
typedef BOOL (WINAPI *CloseContext)(HANDLE);
typedef BOOL (WINAPI *UpdateContext)(HANDLE, BOOL);
typedef BOOL (WINAPI *ReadPacket)(HANDLE, UINT, void *);
typedef BOOL (WINAPI *QueueSize)(HANDLE, int);
typedef int (WINAPI *ReadPackets)(HANDLE, int, void *);
typedef BOOL (WINAPI *ContextSettings)(HANDLE, LogContextA *);


static HMODULE self;
static HMODULE driver;
static CRITICAL_SECTION initialization;
static BOOL wacom;
static GetInfo native_info;
static OpenContext native_open;
static CloseContext native_close;
static UpdateContext native_enable, native_overlap;
static ReadPacket native_packet;
static QueueSize native_queue;
static ReadPackets native_packets;
static ContextSettings native_get, native_set;

static BOOL connect_driver(void)
{
    EnterCriticalSection(&initialization);
    if (!driver) {
        char path[MAX_PATH];
        char choice[32];
        UINT length = GetSystemDirectoryA(path, sizeof(path));
        DWORD choice_length = GetEnvironmentVariableA("STYLOS_WINTAB_DEVICE", choice, sizeof(choice));
        wacom = choice_length > 0 && choice_length < sizeof(choice) && lstrcmpiA(choice, "Wacom") == 0;
        const char *name = wacom ? "\\Wacom_Tablet.dll" : "\\Wintab32.dll";
        if (length > 0 && length + strlen(name) < sizeof(path)) {
            strcat(path, name);
            HMODULE library = LoadLibraryA(path);
            if (library && library != self) {
                native_info = (GetInfo)GetProcAddress(library, "WTInfoA");
                native_open = (OpenContext)GetProcAddress(library, "WTOpenA");
                native_close = (CloseContext)GetProcAddress(library, "WTClose");
                native_enable = (UpdateContext)GetProcAddress(library, "WTEnable");
                native_overlap = (UpdateContext)GetProcAddress(library, "WTOverlap");
                native_packet = (ReadPacket)GetProcAddress(library, "WTPacket");
                native_queue = (QueueSize)GetProcAddress(library, "WTQueueSizeSet");
                native_packets = (ReadPackets)GetProcAddress(library, "WTPacketsGet");
                native_get = (ContextSettings)GetProcAddress(library, "WTGetA");
                native_set = (ContextSettings)GetProcAddress(library, "WTSetA");
                if (native_info && native_open && native_close && native_enable && native_overlap &&
                    native_packet && native_queue && native_packets && native_get && native_set)
                    driver = library;
                else
                    FreeLibrary(library);
            } else if (library) {
                FreeLibrary(library);
            }
        }
    }
    BOOL available = driver != NULL;
    LeaveCriticalSection(&initialization);
    return available;
}

static void normalize_screen_rectangle(LogContextA *settings)
{
    int screen_width = GetSystemMetrics(SM_CXVIRTUALSCREEN);
    int screen_height = GetSystemMetrics(SM_CYVIRTUALSCREEN);
    if (screen_width <= 0 || screen_height <= 0 || settings->system_height <= 0)
        return;
    double ratio = (double)settings->system_height / screen_height;
    if (ratio > 1.0) {
        // On this workstation Huion applies the desktop scaling once more in tablet-coordinate mode.
        if (!wacom)
            ratio *= ratio;
        settings->out_width = (int)(settings->out_width / ratio);
        settings->out_height = (int)(settings->out_height / ratio);
    }
    // Stylos has a second, Wacom-specific adjustment when these sizes differ.
    settings->system_width = screen_width;
    settings->system_height = screen_height;
}

EXPORT UINT WINAPI WTInfoA(UINT category, UINT index, void *output)
{
    if (!connect_driver())
        return 0;
    UINT size = native_info(category, index, output);
    if (output && category == TABLET_SYSTEM_CONTEXT && index == 0 && size == sizeof(LogContextA))
        normalize_screen_rectangle((LogContextA *)output);
    return size;
}

EXPORT HANDLE WINAPI WTOpenA(HWND window, LogContextA *settings, BOOL enable)
{
    if (!connect_driver())
        return NULL;
    if (settings && settings->packet_data == STYLOS_PACKET_FIELDS) {
        LogContextA original;
        if (native_info(TABLET_SYSTEM_CONTEXT, 0, &original) == sizeof(original)) {
            LogContextA request = *settings;
            // Keep the driver's active system mapping at the original size.
            request.system_width = original.system_width;
            request.system_height = original.system_height;
            return native_open(window, &request, enable);
        }
    }
    return native_open(window, settings, enable);
}

EXPORT BOOL WINAPI WTClose(HANDLE context)
{
    return connect_driver() && native_close(context);
}

EXPORT BOOL WINAPI WTEnable(HANDLE context, BOOL enable)
{
    return connect_driver() && native_enable(context, enable);
}

EXPORT BOOL WINAPI WTOverlap(HANDLE context, BOOL top)
{
    return connect_driver() && native_overlap(context, top);
}

EXPORT BOOL WINAPI WTPacket(HANDLE context, UINT serial, void *data)
{
    return connect_driver() && native_packet(context, serial, data);
}

EXPORT BOOL WINAPI WTQueueSizeSet(HANDLE context, int size)
{
    return connect_driver() && native_queue(context, size);
}

EXPORT int WINAPI WTPacketsGet(HANDLE context, int limit, void *data)
{
    return connect_driver() ? native_packets(context, limit, data) : 0;
}

EXPORT BOOL WINAPI WTGetA(HANDLE context, LogContextA *settings)
{
    return connect_driver() && native_get(context, settings);
}

EXPORT BOOL WINAPI WTSetA(HANDLE context, LogContextA *settings)
{
    return connect_driver() && native_set(context, settings);
}

BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, void *reserved)
{
    if (reason == DLL_PROCESS_ATTACH) {
        self = instance;
        InitializeCriticalSection(&initialization);
    } else if (reason == DLL_PROCESS_DETACH) {
        DeleteCriticalSection(&initialization);
    }
    return TRUE;
}
