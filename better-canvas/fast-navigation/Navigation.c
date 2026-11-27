#include <windows.h>
#include <commctrl.h>
#include <string.h>
#include "../CanvasView.h"
#include "../NavigationTrace.h"

/* UI-thread state, one entry per native drawing window. */
typedef struct Navigation {
    struct Navigation *next;
    HWND window;
    BYTE *view;
    int scale, angle;
    DWORD changed;
    BOOL pending;
} Navigation;

static _Thread_local Navigation *views;
enum { SettleMs = 60 };

void NoteCanvasNavigation(void *view)
{
    for (Navigation *s = views; s; s = s->next)
        if (s->view == view && s->pending) {
            s->changed = GetTickCount();
            SetTimer(s->window, (UINT_PTR)s, SettleMs, NULL);
            return;
        }
}

static int value(BYTE *view, int offset)
{
    int result;
    memcpy(&result, view + offset, sizeof(result));
    return result;
}

static BOOL owns_view(HWND window, BYTE *view)
{
    BYTE *object = (BYTE *)GetWindowLongPtrW(window, GWLP_USERDATA);
    return object && *(void **)object == (BYTE *)GetModuleHandleW(NULL) + CanvasVtable
        && *(HWND *)(object + 4) == window && *(BYTE **)(object + CanvasView) == view;
}

static void refine(Navigation *state)
{
    HWND w = state->window;
    BYTE *view = state->view;
    KillTimer(state->window, (UINT_PTR)state);
    state->pending = FALSE;
    if (!owns_view(state->window, state->view)) return;
    state->angle = value(state->view, RotationAngle);
    state->scale = value(state->view, ZoomScale);
    NAV_TRACE(NavRefine, state->scale, state->angle);
    BYTE *base = (BYTE *)GetModuleHandleW(NULL);
    // A full redraw recomputes scroll offsets from the host's saved centre.
    // Restore the same artwork point, including for rotated and mirrored views.
    double client[2] = {0,0}, art[2], before[2], after[2];
    MapStylosPoint(base + ClientToArtwork, view, art, client);
    MapStylosPoint(base + ArtworkToClient, view, before, art);
    ApplyStylosRotation(base + SetRotation, view, state->angle);
    if (!owns_view(w, view)) return;
    MapStylosPoint(base + ArtworkToClient, view, after, art);
    double x = after[0] - before[0], y = after[1] - before[1];
    int dx = (int)(x < 0 ? x - .5 : x + .5);
    int dy = (int)(y < 0 ? y - .5 : y + .5);
    if (dx || dy) ScrollStylosView(base + ScrollView, view, dx, dy);
    RedrawWindow(w, NULL, NULL, RDW_INVALIDATE | RDW_UPDATENOW | RDW_NOERASE);
    NAV_TRACE(NavRefined, 0, 0);
}

static LRESULT CALLBACK messages(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    Navigation *state = (Navigation *)data;
    if (msg == WM_TIMER && wp == id) {
        if (state->pending && GetTickCount() - state->changed >= SettleMs)
            refine(state);
        return 0;
    }
    if (msg == WM_LBUTTONDOWN || msg == WM_POINTERDOWN) {
        FlushQueuedCanvasInput();
        if (IsWindow(w) && state->pending) refine(state);
    }
    if (msg == WM_NCDESTROY) {
        ForgetQueuedCanvasInput(w);
        Navigation **link = &views;
        while (*link && *link != state) link = &(*link)->next;
        if (*link) *link = state->next;
        KillTimer(w, id);
        RemoveWindowSubclass(w, messages, id);
        HeapFree(GetProcessHeap(), 0, state);
    }
    return DefSubclassProc(w, msg, wp, lp);
}

typedef struct { BYTE *view; HWND window; } Search;

static BOOL CALLBACK find_child(HWND w, LPARAM param)
{
    Search *search = (Search *)param;
    WCHAR name[24];
    if (GetClassNameW(w, name, ARRAYSIZE(name)) && !lstrcmpW(name, L"CCelWindow")
        && owns_view(w, search->view)) {
        search->window = w;
        return FALSE;
    }
    return TRUE;
}

static BOOL CALLBACK find_frame(HWND w, LPARAM param)
{
    EnumChildWindows(w, find_child, param);
    return !((Search *)param)->window;
}

BOOL FastCanvasFrame(BYTE *view)
{
    int scale = value(view, ZoomScale), angle = value(view, RotationAngle);
    Navigation *state = views;
    while (state && state->view != view) state = state->next;
    if (!state) {
        if (!CanvasViewSupported()) return FALSE;
        Search search = {view, NULL};
        EnumThreadWindows(GetCurrentThreadId(), find_frame, (LPARAM)&search);
        if (!search.window) return FALSE;
        state = HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, sizeof(*state));
        if (!state) return FALSE;
        if (!SetWindowSubclass(search.window, messages, (UINT_PTR)state, (DWORD_PTR)state)) {
            HeapFree(GetProcessHeap(), 0, state);
            return FALSE;
        }
        state->next = views;
        views = state;
        state->window = search.window;
        state->view = view;
        state->scale = scale;
        state->angle = angle;
        return FALSE;
    }
    if (scale != state->scale || angle != state->angle) {
        state->scale = scale;
        state->angle = angle;
        state->changed = GetTickCount();
        if (SetTimer(state->window, (UINT_PTR)state, SettleMs, NULL))
            state->pending = TRUE;
    }
    return state->pending;
}
