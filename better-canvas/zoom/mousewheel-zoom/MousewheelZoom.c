#include "../../CanvasView.h"
#include "../../NavigationTrace.h"
static HWND last_canvas;
static int last_scale;
static int wheel_remainder;

static int nearest_pixel(double value)
{
    return (int)(value + (value < 0.0 ? -0.5 : 0.5));
}

static void zoom_at(unsigned char *view, POINT pointer, int desired)
{
    double client[2] = {pointer.x, pointer.y};
    double artwork[2], before[2], after[2];
    // Project the same artwork point after scaling so rotation and mirroring are included.
    MapStylosPoint(canvas_image + ClientToArtwork, view, artwork, client);
    NAV_TRACE(NavAnchorBefore, nearest_pixel(artwork[0]*256), nearest_pixel(artwork[1]*256));
    MapStylosPoint(canvas_image + ArtworkToClient, view, before, artwork);
    ApplyStylosZoom(canvas_image + SetZoom, view, (double)desired / ScaleUnits);
    NAV_TRACE(NavZoomDone, desired, 0);
    MapStylosPoint(canvas_image + ArtworkToClient, view, after, artwork);
    int dx = nearest_pixel(after[0] - before[0]);
    int dy = nearest_pixel(after[1] - before[1]);
    if (dx || dy)
        ScrollStylosView(canvas_image + ScrollView, view, dx, dy);
    NAV_TRACE(NavScrollDone, dx, dy);
#ifdef STYLOS_NAV_TRACE
    MapStylosPoint(canvas_image + ClientToArtwork, view, artwork, client);
    NAV_TRACE(NavAnchorAfter, nearest_pixel(artwork[0]*256), nearest_pixel(artwork[1]*256));
#endif
}

static void apply_zoom(HWND canvas, unsigned char *view, POINT pointer, int desired)
{
    zoom_at(view, pointer, desired);
    int actual = *(int *)(view + ZoomScale);
    DisplayStylosZoom(canvas_image + SetZoomDisplay, view + ZoomControl, actual * (100.0 / ScaleUnits));
#ifdef STYLOS_MANAGER_INPUT
    if (FastCanvasNavigationEnabled())
        UpdateWindow(canvas);
#endif
}

BOOL zoom_canvas(const MSG *message)
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
    GetStylosViewport(canvas_image + OriginalViewport, view, &viewport);
    if (!PtInRect(&viewport, pointer))
        return FALSE;
#ifdef STYLOS_MANAGER_INPUT
    int current = CanvasInputValue(message, canvas, view, ZoomScale);
#else
    int current = *(int *)(view + ZoomScale);
#endif
    if (current < 1 || current > MaximumScale)
        return FALSE;
    if (canvas != last_canvas || current != last_scale)
        wheel_remainder = 0;
    last_canvas = canvas;

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
#ifdef STYLOS_MANAGER_INPUT
        NoteCanvasNavigation(view);
#endif
        NAV_TRACE(NavInput, WM_MOUSEWHEEL, desired);
#ifdef STYLOS_MANAGER_INPUT
        ApplyOrQueueCanvasInput(message, canvas, view, pointer, desired, apply_zoom);
        current = CanvasInputValue(message, canvas, view, ZoomScale);
#else
        apply_zoom(canvas, view, pointer, desired);
        current = *(int *)(view + ZoomScale);
#endif
        NAV_TRACE(NavInputDone, WM_MOUSEWHEEL, current);
    }
    last_scale = current;
    return TRUE;
}


void forget_zoom(HWND canvas) { if(last_canvas==canvas)last_canvas=NULL; }
