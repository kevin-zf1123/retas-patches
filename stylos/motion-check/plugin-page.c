#define UNICODE
#include <windows.h>
#include <commctrl.h>
#include "../../retas-devkit/plugin.h"
#include "src/settings_page.h"

typedef struct {
    RenderSettings original, draft;
    HWND page;
} MotionPage;

// Validate before the native dialog handles OK. Cancel and a failed validation
// leave the live scheduler configuration intact.
static LRESULT CALLBACK validate(HWND dialog, UINT msg, WPARAM wp, LPARAM lp,
                                 UINT_PTR id, DWORD_PTR data) {
    MotionPage *state = (MotionPage *)data;
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK &&
        !read_motion_page(state->page, &state->original, &state->draft))
        return 0;
    if (msg == WM_NCDESTROY) {
        RemoveWindowSubclass(dialog, validate, id);
        HeapFree(GetProcessHeap(), 0, state);
    }
    return DefSubclassProc(dialog, msg, wp, lp);
}

static HWND WINAPI create(HWND dialog, RECT area) {
    HMODULE module = NULL;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS |
                           GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCWSTR)&create, &module)) return NULL;
    MotionPage *state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
    if (!state) return NULL;
    state->original = state->draft = render_settings();
    HWND page = CreateDialogParamW(module, MAKEINTRESOURCEW(MOTION_SETTINGS_PAGE),
                                   dialog, motion_page, (LPARAM)&state->original);
    if (!page || !SetWindowSubclass(dialog, validate, (UINT_PTR)page, (DWORD_PTR)state)) {
        if (page) DestroyWindow(page);
        HeapFree(GetProcessHeap(), 0, state);
        return NULL;
    }
    state->page = page;
    SetWindowLongPtrW(page, DWLP_USER, (LONG_PTR)state);
    RECT design;
    GetClientRect(page, &design);
    int width = area.right - area.left, height = area.bottom - area.top;
    WPARAM font = SendDlgItemMessageW(dialog, IDOK, WM_GETFONT, 0, 0);
    for (HWND child = GetWindow(page, GW_CHILD); child; child = GetWindow(child, GW_HWNDNEXT)) {
        RECT pos;
        GetWindowRect(child, &pos);
        MapWindowPoints(NULL, page, (POINT *)&pos, 2);
        SendMessageW(child, WM_SETFONT, font, FALSE);
        MoveWindow(child, MulDiv(pos.left, width, design.right), MulDiv(pos.top, height, design.bottom),
                   MulDiv(pos.right-pos.left, width, design.right), MulDiv(pos.bottom-pos.top, height, design.bottom), FALSE);
    }
    SetWindowPos(page, HWND_TOP, area.left, area.top, width, height, SWP_NOACTIVATE);
    return page;
}

static void WINAPI accept_motion_page(HWND page) {
    MotionPage *state = (MotionPage *)GetWindowLongPtrW(page, DWLP_USER);
    if (state && memcmp(&state->original, &state->draft, sizeof(state->draft)) &&
        !save_render_settings(&state->draft))
        MessageBoxW(GetParent(page), L"无法保存动检配置。", L"动检预渲染", MB_ICONERROR);
}

BOOL WINAPI RetasPluginInit(const RetasHost *host, RetasPlugin *plugin) {
    if (host->abi != RETAS_PLUGIN_ABI || plugin->size < sizeof(*plugin)) return FALSE;
    plugin->create_page = create;
    plugin->accept_page = accept_motion_page;
    return TRUE;
}
