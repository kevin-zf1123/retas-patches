#include <windows.h>
#include <string.h>

void ApplyStylosZoom(void *entry, void *view, double scale);
void DisplayStylosZoom(void *entry, void *control, double percent);
void GetStylosViewport(void *entry, void *view, RECT *result);
void MapStylosPoint(void *entry, void *view, double result[2], const double point[2]);
void ScrollStylosView(void *entry, void *view, int dx, int dy);
void ApplyStylosRotation(void *entry, void *view, int angle);
void DisplayStylosRotation(void *entry, void *control, int angle);
BOOL InstallStylosShortcuts(unsigned char *base, HWND message_window);
BOOL DispatchStylosShortcut(const MSG *message);

typedef LRESULT (CALLBACK *CanvasSubclass)(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
typedef LRESULT (WINAPI *CallSubclass)(HWND, UINT, WPARAM, LPARAM);

// Stylos 2.6.0 stores the view scale in units of 1/4096.
enum {
    CanvasVtable = 0x5d279c,
    CanvasView = 0x24,
    ViewWheelSlot = 0x64,
    ViewportSlot = 0x8c,
    OriginalViewport = 0x59c60,
    OriginalWheel = 0x69000,
    ClientToArtwork = 0x56380,
    ArtworkToClient = 0x563e0,
    SetZoom = 0x5e6f0,
    ScrollView = 0x60d10,
    SetZoomDisplay = 0x343520,
    SetRotation = 0x671e0,
    SetRotationDisplay = 0x3434a0,
    ZoomControl = 0x150,
    RotationControl = 0x1e8,
    ZoomScale = 0x290,
    RotationAngle = 0x2d0,
    ScaleUnits = 4096,
    MinimumScale = 410,
    MaximumScale = 40960,
    NativeAngleEdit = 122,
    NativeToolbarRight = 466,
    RotateLeftButton = 0x6e40,
    RotateRightButton = 0x6e41
};

static unsigned char *image;
static HHOOK wheel_hook;
static HWND last_canvas;
static int last_scale;
static int wheel_remainder;
static BOOL handling_input;
static BOOL native_shortcuts;
enum { CanvasFilterCombo = 0x6e60 };
static int (__cdecl *read_filter)(void);
static int (__cdecl *write_filter)(int);
static BOOL (WINAPI *set_subclass)(HWND, CanvasSubclass, UINT_PTR, DWORD_PTR);
static BOOL (WINAPI *remove_subclass)(HWND, CanvasSubclass, UINT_PTR);
static CallSubclass call_subclass;

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

static unsigned char *canvas_view(HWND canvas)
{
    char name[32];
    if (!canvas || GetWindowThreadProcessId(canvas, NULL) != GetCurrentThreadId()
        || !GetClassNameA(canvas, name, sizeof(name)) || strcmp(name, "CCelWindow") != 0)
        return NULL;
    unsigned char *object = (unsigned char *)GetWindowLongA(canvas, GWL_USERDATA);
    if (!object || *(void **)object != image + CanvasVtable || *(HWND *)(object + 4) != canvas)
        return NULL;
    unsigned char *view = *(unsigned char **)(object + CanvasView);
    if (!view || !*(unsigned char **)view
        || *(void **)(*(unsigned char **)view + ViewWheelSlot) != image + OriginalWheel
        || *(void **)(*(unsigned char **)view + ViewportSlot) != image + OriginalViewport)
        return NULL;
    return view;
}

static BOOL canvas_is_active(HWND canvas)
{
    return IsWindowEnabled(GetAncestor(canvas, GA_ROOT))
        && (HWND)SendMessageA(GetParent(canvas), WM_MDIGETACTIVE, 0, 0) == canvas;
}

static int nearest_pixel(double value)
{
    return (int)(value + (value < 0.0 ? -0.5 : 0.5));
}

static void zoom_at(unsigned char *view, POINT pointer, int desired)
{
    double client[2] = {pointer.x, pointer.y};
    double artwork[2], before[2], after[2];
    // Project the same artwork point after scaling so rotation and mirroring are included.
    MapStylosPoint(image + ClientToArtwork, view, artwork, client);
    MapStylosPoint(image + ArtworkToClient, view, before, artwork);
    ApplyStylosZoom(image + SetZoom, view, (double)desired / ScaleUnits);
    MapStylosPoint(image + ArtworkToClient, view, after, artwork);
    int dx = nearest_pixel(after[0] - before[0]);
    int dy = nearest_pixel(after[1] - before[1]);
    if (dx || dy)
        ScrollStylosView(image + ScrollView, view, dx, dy);
}

static BOOL zoom_canvas(const MSG *message)
{
    int delta = (short)HIWORD(message->wParam);
    if (!delta || (LOWORD(message->wParam) & (MK_CONTROL | MK_SHIFT | MK_LBUTTON | MK_RBUTTON | MK_MBUTTON)))
        return FALSE;
    POINT pointer = {(short)LOWORD(message->lParam), (short)HIWORD(message->lParam)};
    HWND canvas = WindowFromPoint(pointer);
    unsigned char *view = canvas_view(canvas);
    if (!view || !canvas_is_active(canvas) || !ScreenToClient(canvas, &pointer))
        return FALSE;
    RECT viewport;
    GetStylosViewport(image + OriginalViewport, view, &viewport);
    if (!PtInRect(&viewport, pointer))
        return FALSE;

    int current = *(int *)(view + ZoomScale);
    if (current < 1 || current > MaximumScale)
        return FALSE;
    if (canvas != last_canvas || current != last_scale)
        wheel_remainder = 0;

    // Five percentage points per 120 wheel units, retaining fractional wheel motion.
    const int divisor = WHEEL_DELTA * 100;
    int total = wheel_remainder + delta * 5 * ScaleUnits;
    int desired = current + total / divisor;
    wheel_remainder = total % divisor;
    if (desired < MinimumScale || desired > MaximumScale) {
        desired = desired < MinimumScale ? MinimumScale : MaximumScale;
        wheel_remainder = 0;
    }
    if (desired != current) {
        zoom_at(view, pointer, desired);
        current = *(int *)(view + ZoomScale);
        DisplayStylosZoom(image + SetZoomDisplay, view + ZoomControl, current * (100.0 / ScaleUnits));
    }
    last_canvas = canvas;
    last_scale = current;
    return TRUE;
}

BOOL RotateStylosCanvas(HWND canvas, int degrees)
{
    unsigned char *view = canvas_view(canvas);
    if (!view || !canvas_is_active(canvas))
        return FALSE;
    int angle = (*(int *)(view + RotationAngle) % 360 + degrees) % 360;
    if (angle < -180)
        angle += 360;
    if (angle > 180)
        angle -= 360;
    ApplyStylosRotation(image + SetRotation, view, angle);
    DisplayStylosRotation(image + SetRotationDisplay, view + RotationControl, angle);
    return TRUE;
}

static BOOL rotate_from_key(const MSG *message)
{
    WPARAM key = message->wParam;
    if ((key != VK_OEM_MINUS && key != VK_OEM_PLUS)
        || (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_SHIFT) & 0x8000)
        || (GetKeyState(VK_MENU) & 0x8000) || GetCapture())
        return FALSE;
    HWND canvas = GetFocus();
    if (canvas != message->hwnd)
        return FALSE;
    return RotateStylosCanvas(canvas, key == VK_OEM_MINUS ? -5 : 5);
}

static void place_rotation_button(HWND canvas, HWND button, int x, int y, int width, int height, BOOL show)
{
    BOOL visible = (GetWindowLongA(button, GWL_STYLE) & WS_VISIBLE) != 0;
    if (!show) {
        if (visible)
            ShowWindow(button, SW_HIDE);
        return;
    }
    RECT current;
    if (visible && GetWindowRect(button, &current)) {
        MapWindowPoints(NULL, canvas, (POINT *)&current, 2);
        if (current.left == x && current.top == y && current.right == x + width && current.bottom == y + height)
            return;
    }
    SetWindowPos(button, NULL, x, y, width, height, SWP_NOACTIVATE | SWP_NOZORDER | SWP_SHOWWINDOW);
}

static void layout_rotation_buttons(HWND canvas)
{
    HWND angle = GetDlgItem(canvas, NativeAngleEdit);
    HWND left = GetDlgItem(canvas, RotateLeftButton);
    HWND right = GetDlgItem(canvas, RotateRightButton);
    RECT client, edit;
    if (!angle || !left || !right || !GetClientRect(canvas, &client) || !GetWindowRect(angle, &edit))
        return;
    MapWindowPoints(NULL, canvas, (POINT *)&edit, 2);
    int height = edit.bottom - edit.top;
    int width = 3 * height;
    int gap = height / 4;
    int x = client.right - 2 * width - gap - 2 * height;
    BOOL show = (GetWindowLongA(angle, GWL_STYLE) & WS_VISIBLE) && height > 0 && x >= NativeToolbarRight + gap;
    place_rotation_button(canvas, left, x, edit.top, width, height, show);
    place_rotation_button(canvas, right, x + width + gap, edit.top, width, height, show);
}

static LRESULT CALLBACK canvas_message(HWND canvas, UINT message, WPARAM wparam, LPARAM lparam,
    UINT_PTR id, DWORD_PTR data)
{
    if (message == WM_COMMAND && lparam && HIWORD(wparam) == BN_CLICKED) {
        int button = LOWORD(wparam);
        if ((button == RotateLeftButton || button == RotateRightButton)
            && (HWND)lparam == GetDlgItem(canvas, button)) {
            if (RotateStylosCanvas(canvas, button == RotateLeftButton ? -5 : 5))
                SetFocus(canvas);
            return 0;
        }
    }
    if (message == WM_NCDESTROY) {
        if (last_canvas == canvas)
            last_canvas = NULL;
        remove_subclass(canvas, canvas_message, id);
    }
    LRESULT result = call_subclass(canvas, message, wparam, lparam);
    if (message == WM_SIZE || message == WM_SHOWWINDOW || message == WM_PAINT)
        layout_rotation_buttons(canvas);
    return result;
}

static void prepare_rotation_buttons(HWND canvas)
{
    if (!set_subclass || !canvas_view(canvas))
        return;
    if (GetDlgItem(canvas, RotateLeftButton))
        return;
    HWND angle = GetDlgItem(canvas, NativeAngleEdit);
    if (!angle || GetDlgItem(canvas, RotateRightButton)
        || !set_subclass(canvas, canvas_message, RotateLeftButton, 0))
        return;
    HINSTANCE instance = (HINSTANCE)image;
    DWORD style = WS_CHILD | BS_PUSHBUTTON;
    HWND left = CreateWindowExW(0, L"BUTTON", L"\x5de6\x8f6c 5\x00b0", style,
        0, 0, 0, 0, canvas, (HMENU)RotateLeftButton, instance, NULL);
    HWND right = CreateWindowExW(0, L"BUTTON", L"\x53f3\x8f6c 5\x00b0", style,
        0, 0, 0, 0, canvas, (HMENU)RotateRightButton, instance, NULL);
    if (!left || !right) {
        if (left)
            DestroyWindow(left);
        if (right)
            DestroyWindow(right);
        remove_subclass(canvas, canvas_message, RotateLeftButton);
        return;
    }
    WPARAM font = SendMessageW(angle, WM_GETFONT, 0, 0);
    if (!font)
        font = (WPARAM)GetStockObject(DEFAULT_GUI_FONT);
    SendMessageW(left, WM_SETFONT, font, FALSE);
    SendMessageW(right, WM_SETFONT, font, FALSE);
    layout_rotation_buttons(canvas);
}

static BOOL CALLBACK refresh_filter(HWND window, LPARAM unused)
{
    unsigned char *view = canvas_view(window);
    if (view)
        ApplyStylosRotation(image + SetRotation, view, *(int *)(view + RotationAngle));
    return TRUE;
}

static LRESULT CALLBACK filter_preferences(HWND dialog, UINT msg, WPARAM wp, LPARAM lp,
    UINT_PTR id, DWORD_PTR data)
{
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK) {
        int mode = SendDlgItemMessageW(dialog, CanvasFilterCombo, CB_GETCURSEL, 0, 0);
        SetPropW(dialog, L"StylosCanvasChoice", (HANDLE)(mode+1));
    }
    if ((msg == WM_COMMAND && LOWORD(wp) == IDCANCEL) || msg == WM_CLOSE)
        RemovePropW(dialog, L"StylosCanvasChoice");
    if (msg == WM_DESTROY) {
        int mode = (int)RemovePropW(dialog, L"StylosCanvasChoice")-1;
        if (mode >= 0 && mode != read_filter() && write_filter(mode))
            PostMessageW(GetAncestor(dialog, GA_ROOTOWNER), WM_APP+0x4f8, 0, 0);
    }
    if (msg == WM_NCDESTROY)
        remove_subclass(dialog, filter_preferences, id);
    return call_subclass(dialog, msg, wp, lp);
}

void AddStylosFilterPreference(HWND child)
{
    HWND dialog = GetAncestor(child, GA_ROOT);
    if (!read_filter || !set_subclass || !dialog || GetDlgItem(dialog, CanvasFilterCombo))
        return;
    HWND tree = GetDlgItem(dialog, 1762), ok = GetDlgItem(dialog, IDOK);
    char name[32];
    if (!ok || !tree || !GetClassNameA(tree, name, sizeof(name)) || lstrcmpiA(name, "SysTreeView32")
        || !GetDlgItem(dialog, 1763) || !GetDlgItem(dialog, IDCANCEL))
        return;
    RECT row, list, units = {0, 0, 60, 85};
    GetWindowRect(ok, &row);
    GetWindowRect(tree, &list);
    MapWindowPoints(NULL, dialog, (POINT *)&row, 2);
    MapWindowPoints(NULL, dialog, (POINT *)&list, 2);
    MapDialogRect(dialog, &units);
    HWND label = CreateWindowExW(0, L"STATIC", L"\x753b\x5e03\x7f29\x653e", WS_CHILD | WS_VISIBLE | SS_CENTERIMAGE,
        list.left, row.top, units.right, row.bottom-row.top, dialog, NULL, (HINSTANCE)image, NULL);
    HWND combo = CreateWindowExW(0, L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_TABSTOP | CBS_DROPDOWNLIST,
        list.left+units.right, row.top, min(units.right*2, row.left-list.left-units.right-8), units.bottom,
        dialog, (HMENU)CanvasFilterCombo, (HINSTANCE)image, NULL);
    if (!label || !combo || !set_subclass(dialog, filter_preferences, CanvasFilterCombo, 0)) {
        if (label) DestroyWindow(label);
        if (combo) DestroyWindow(combo);
        return;
    }
    WPARAM font = SendMessageW(ok, WM_GETFONT, 0, 0);
    SendMessageW(label, WM_SETFONT, font, TRUE);
    SendMessageW(combo, WM_SETFONT, font, TRUE);
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"\x539f\x59cb\x7f29\x653e");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"\x53cc\x7ebf\x6027");
    SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"\x53cc\x7acb\x65b9");
    SendMessageW(combo, CB_SETCURSEL, read_filter(), 0);
}

void RefreshStylosFilter(HWND frame)
{
    EnumChildWindows(frame, refresh_filter, 0);
}

static LRESULT CALLBACK wheel_message(int code, WPARAM operation, LPARAM data)
{
    if (code >= 0 && (operation & PM_REMOVE) && data && !handling_input) {
        MSG *message = (MSG *)data;
        handling_input = TRUE;
        if (!native_shortcuts && message->hwnd)
            native_shortcuts = InstallStylosShortcuts(image, message->hwnd);
        if ((message->message == WM_MOUSEWHEEL && zoom_canvas(message))
            || (native_shortcuts && (message->message == WM_KEYDOWN || message->message == WM_SYSKEYDOWN)
                && DispatchStylosShortcut(message))
            || (!native_shortcuts && message->message == WM_KEYDOWN && rotate_from_key(message)))
            message->message = WM_NULL;
        if (message->message == WM_PAINT) {
            AddStylosFilterPreference(message->hwnd);
            prepare_rotation_buttons(message->hwnd);
        }
        handling_input = FALSE;
    }
    return CallNextHookEx(wheel_hook, code, operation, data);
}

void InstallStylosLinearZoom(void)
{
    static BOOL checked;
    if (checked)
        return;
    checked = TRUE;
    unsigned char *candidate = (unsigned char *)GetModuleHandleA(NULL);
    if (!matches_stylos(candidate))
        return;
    image = candidate;
    HMODULE filter = GetModuleHandleW(L"StylosCanvasFilter.dll");
    if (filter && !GetProcAddress(filter, "CanvasPreferences")) {
        write_filter = (void *)GetProcAddress(filter, "SetCanvasFilter");
        if (write_filter)
            read_filter = (void *)GetProcAddress(filter, "GetCanvasFilter");
    }
    HMODULE common = GetModuleHandleA("comctl32.dll");
    if (common) {
        remove_subclass = (void *)GetProcAddress(common, "RemoveWindowSubclass");
        call_subclass = (void *)GetProcAddress(common, "DefSubclassProc");
        if (remove_subclass && call_subclass)
            set_subclass = (void *)GetProcAddress(common, "SetWindowSubclass");
    }
    HMODULE module;
    // Keep the callback valid even if Stylos releases its WinTab library before exit.
    if (GetModuleHandleExA(5, (LPCSTR)wheel_message, &module))
        wheel_hook = SetWindowsHookExA(WH_GETMESSAGE, wheel_message, NULL, GetCurrentThreadId());
}
