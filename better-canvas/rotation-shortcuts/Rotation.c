#include "../CanvasView.h"
#include "../NavigationTrace.h"
static void apply_angle(HWND canvas, unsigned char *view, POINT unused, int angle)
{
    if (*(int *)(view + RotationAngle) == angle) return;
    ApplyStylosRotation(canvas_image + SetRotation, view, angle);
    DisplayStylosRotation(canvas_image + SetRotationDisplay, view + RotationControl, angle);
}

BOOL RotateStylosCanvas(HWND canvas, int degrees, const MSG *input)
{
    unsigned char *view = canvas_view(canvas);
    if (!view || !canvas_is_active(canvas))
        return FALSE;
    POINT unused = {0,0};
#ifdef STYLOS_MANAGER_INPUT
    int previous = CanvasInputValue(input, canvas, view, RotationAngle);
    NoteCanvasNavigation(view);
#else
    int previous = *(int *)(view + RotationAngle);
#endif
    if (input && (input->message == WM_KEYDOWN || input->message == WM_SYSKEYDOWN) && LOWORD(input->lParam))
        degrees *= LOWORD(input->lParam);
    int angle = (previous % 360 + degrees) % 360;
    if (angle < -180)
        angle += 360;
    if (angle > 180)
        angle -= 360;
    NAV_TRACE(NavInput, WM_COMMAND, angle);
#ifdef STYLOS_MANAGER_INPUT
    ApplyOrQueueCanvasInput(input, canvas, view, unused, angle, apply_angle);
#else
    apply_angle(canvas, view, unused, angle);
#endif
    NAV_TRACE(NavInputDone, WM_COMMAND, angle);
    return TRUE;
}

BOOL rotate_from_key(const MSG *message)
{
    WPARAM key = message->wParam;
    if ((key != VK_OEM_MINUS && key != VK_OEM_PLUS)
        || (GetKeyState(VK_CONTROL) & 0x8000) || (GetKeyState(VK_SHIFT) & 0x8000)
        || (GetKeyState(VK_MENU) & 0x8000) || GetCapture())
        return FALSE;
    HWND canvas = GetFocus();
    if (canvas != message->hwnd)
        return FALSE;
    return RotateStylosCanvas(canvas, key == VK_OEM_MINUS ? -5 : 5, message);
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
#ifdef STYLOS_MANAGER_INPUT
    if (message == WM_LBUTTONDOWN || message == WM_POINTERDOWN || message == WM_KILLFOCUS)
        FlushQueuedCanvasInput();
#endif
#ifdef STYLOS_NAV_TRACE
    if (message == WM_APP + 0x394) {
        unsigned char *view = canvas_view(canvas);
        if (!view || wparam < MinimumScale || wparam > MaximumScale) return 0;
        ApplyStylosZoom(canvas_image + SetZoom, view, (double)wparam / ScaleUnits);
        DisplayStylosZoom(canvas_image + SetZoomDisplay, view + ZoomControl,
                          *(int *)(view + ZoomScale) * 100.0 / ScaleUnits);
        return *(int *)(view + ZoomScale);
    }
    if (message == WM_LBUTTONDOWN) {
        unsigned char *view = canvas_view(canvas);
        if (view) {
            double client[2] = {(short)LOWORD(lparam), (short)HIWORD(lparam)}, art[2];
            MapStylosPoint(canvas_image + ClientToArtwork, view, art, client);
            NAV_TRACE(NavFirstPoint, (int)(art[0]*256), (int)(art[1]*256));
        }
    }
#endif
    if (message == WM_COMMAND && lparam && HIWORD(wparam) == BN_CLICKED) {
        int button = LOWORD(wparam);
        if ((button == RotateLeftButton || button == RotateRightButton)
            && (HWND)lparam == GetDlgItem(canvas, button)) {
            MSG input = {canvas, message, wparam, lparam};
            if (RotateStylosCanvas(canvas, button == RotateLeftButton ? -5 : 5, &input))
                SetFocus(canvas);
            return 0;
        }
    }
    if (message == WM_NCDESTROY) {
#ifdef STYLOS_MANAGER_INPUT
        ForgetQueuedCanvasInput(canvas);
#endif
#ifndef RETAS_ROTATION
        forget_zoom(canvas);
#endif
        remove_subclass(canvas, canvas_message, id);
    }
    if (message == WM_PAINT)
        NAV_TRACE(NavPaint, 0, 0);
    LRESULT result = call_subclass(canvas, message, wparam, lparam);
    if (message == WM_PAINT)
        NAV_TRACE(NavPaintDone, 0, 0);
    if (message == WM_SIZE || message == WM_SHOWWINDOW || message == WM_PAINT)
        layout_rotation_buttons(canvas);
    return result;
}

void prepare_rotation_buttons(HWND canvas)
{
    if (!set_subclass || !canvas_view(canvas))
        return;
    if (GetDlgItem(canvas, RotateLeftButton))
        return;
    HWND angle = GetDlgItem(canvas, NativeAngleEdit);
    if (!angle || GetDlgItem(canvas, RotateRightButton)
        || !set_subclass(canvas, canvas_message, RotateLeftButton, 0))
        return;
    HINSTANCE instance = (HINSTANCE)canvas_image;
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

