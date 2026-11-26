#include "CanvasView.h"
#include <string.h>
unsigned char *canvas_image;
static HHOOK wheel_hook;
static BOOL handling_input, native_shortcuts;
BOOL (WINAPI *set_subclass)(HWND, CanvasSubclass, UINT_PTR, DWORD_PTR);
BOOL (WINAPI *remove_subclass)(HWND, CanvasSubclass, UINT_PTR);
CallSubclass call_subclass;

static BOOL matches_stylos(unsigned char *base)
{
    const unsigned char setter[] = {0x6a, 0xff, 0x68, 0xe0, 0xf5, 0x76, 0x00, 0x64};
    const unsigned char display[] = {0x64, 0xa1, 0x00, 0x00, 0x00, 0x00, 0x6a, 0xff};
    const unsigned char to_artwork[] = {0x83, 0xec, 0x10, 0xdd, 0x44, 0x24, 0x18, 0x56};
    const unsigned char to_client[] = {0x83, 0xec, 0x20, 0x8b, 0x54, 0x24, 0x2c, 0x56};
    const unsigned char scroll[] = {0x6a, 0xff, 0x68, 0xb1, 0xf7, 0x76, 0x00, 0x64};
    const unsigned char rotate[] = {0x8b, 0x44, 0x24, 0x04, 0x55, 0x33, 0xed, 0x83};
    const unsigned char angle[] = {0x6a, 0xff, 0x68, 0xab, 0x5d, 0x7a, 0x00, 0x64};
    const IMAGE_DOS_HEADER *dos = (const IMAGE_DOS_HEADER *)base;
    if (!dos || dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || dos->e_lfanew > 4096)
        return FALSE;
    const IMAGE_NT_HEADERS32 *nt = (const IMAGE_NT_HEADERS32 *)(base + dos->e_lfanew);
    return nt->Signature == IMAGE_NT_SIGNATURE
        && nt->FileHeader.Machine == IMAGE_FILE_MACHINE_I386
        && nt->OptionalHeader.SizeOfImage > CanvasVtable + sizeof(void *)
        && memcmp(base + SetZoom, setter, sizeof(setter)) == 0
        && memcmp(base + SetZoomDisplay, display, sizeof(display)) == 0
        && memcmp(base + ClientToArtwork, to_artwork, sizeof(to_artwork)) == 0
        && memcmp(base + ArtworkToClient, to_client, sizeof(to_client)) == 0
        && memcmp(base + ScrollView, scroll, sizeof(scroll)) == 0
        && memcmp(base + SetRotation, rotate, sizeof(rotate)) == 0
        && memcmp(base + SetRotationDisplay, angle, sizeof(angle)) == 0;
}

unsigned char *canvas_view(HWND canvas)
{
    char name[32];
    if (!canvas || GetWindowThreadProcessId(canvas, NULL) != GetCurrentThreadId()
        || !GetClassNameA(canvas, name, sizeof(name)) || strcmp(name, "CCelWindow") != 0)
        return NULL;
    unsigned char *object = (unsigned char *)GetWindowLongA(canvas, GWL_USERDATA);
    if (!object || *(void **)object != canvas_image + CanvasVtable || *(HWND *)(object + 4) != canvas)
        return NULL;
    unsigned char *view = *(unsigned char **)(object + CanvasView);
    if (!view || !*(unsigned char **)view
        || *(void **)(*(unsigned char **)view + ViewWheelSlot) != canvas_image + OriginalWheel
        || *(void **)(*(unsigned char **)view + ViewportSlot) != canvas_image + OriginalViewport)
        return NULL;
    return view;
}

BOOL canvas_is_active(HWND canvas)
{
    return IsWindowEnabled(GetAncestor(canvas, GA_ROOT))
        && (HWND)SendMessageA(GetParent(canvas), WM_MDIGETACTIVE, 0, 0) == canvas;
}

static void initialize_view(void)
{
    static BOOL checked;
    if (checked)
        return;
    checked = TRUE;
    unsigned char *candidate = (unsigned char *)GetModuleHandleA(NULL);
    if (!matches_stylos(candidate))
        return;
    canvas_image = candidate;
    HMODULE common = GetModuleHandleA("comctl32.dll");
    if (common) {
        remove_subclass = (void *)GetProcAddress(common, "RemoveWindowSubclass");
        call_subclass = (void *)GetProcAddress(common, "DefSubclassProc");
        if (remove_subclass && call_subclass)
            set_subclass = (void *)GetProcAddress(common, "SetWindowSubclass");
    }
}

void DispatchCanvasInput(MSG *message, BOOL wheel, BOOL rotate)
{
    if(handling_input || (!wheel && !rotate))return;
    initialize_view();
    if(!canvas_image)return;
    handling_input=TRUE;
        if (rotate && !native_shortcuts && message->hwnd)
            native_shortcuts = InstallStylosShortcuts(canvas_image, message->hwnd);
        if ((wheel && message->message == WM_MOUSEWHEEL && zoom_canvas(message))
            || (rotate && native_shortcuts && (message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN)
                && DispatchStylosShortcut(message))
            || (rotate && !native_shortcuts && message->message == WM_KEYDOWN && rotate_from_key(message)))
            message->message = WM_NULL;
        if (rotate && message->message == WM_PAINT) {
            prepare_rotation_buttons(message->hwnd);
        }
    handling_input=FALSE;
}

#ifndef STYLOS_MANAGER_INPUT
static LRESULT CALLBACK wheel_message(int code,WPARAM operation,LPARAM data)
{
    if(code>=0 && (operation & PM_REMOVE) && data)DispatchCanvasInput((MSG *)data,TRUE,TRUE);
    return CallNextHookEx(wheel_hook,code,operation,data);
}
void InstallStylosLinearZoom(void)
{
    static BOOL done;
    if(done)return;
    done=TRUE;
    HMODULE filter=GetModuleHandleW(L"StylosCanvasFilter.dll");
    BOOL (__cdecl *managed)(void)=filter?(void *)GetProcAddress(filter,"CanvasInputOwned"):NULL;
    if(managed && managed())return;
    initialize_view();
    if(!canvas_image)return;
    HMODULE module;
    if(GetModuleHandleExA(5,(LPCSTR)wheel_message,&module))
        wheel_hook=SetWindowsHookExA(WH_GETMESSAGE,wheel_message,NULL,GetCurrentThreadId());
}
#endif
