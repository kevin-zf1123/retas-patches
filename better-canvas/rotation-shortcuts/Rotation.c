#include "../CanvasView.h"
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
    ApplyStylosRotation(canvas_image + SetRotation, view, angle);
    DisplayStylosRotation(canvas_image + SetRotationDisplay, view + RotationControl, angle);
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
        forget_zoom(canvas);
        remove_subclass(canvas, canvas_message, id);
    }
    LRESULT result = call_subclass(canvas, message, wparam, lparam);
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

