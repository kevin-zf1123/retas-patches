#pragma once
#include <windows.h>
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

extern unsigned char *canvas_image;
extern BOOL (WINAPI *set_subclass)(HWND, CanvasSubclass, UINT_PTR, DWORD_PTR);
extern BOOL (WINAPI *remove_subclass)(HWND, CanvasSubclass, UINT_PTR);
extern CallSubclass call_subclass;
unsigned char *canvas_view(HWND canvas);
BOOL canvas_is_active(HWND canvas);
BOOL zoom_canvas(const MSG *message);
void forget_zoom(HWND canvas);
BOOL rotate_from_key(const MSG *message);
BOOL RotateStylosCanvas(HWND canvas, int degrees, const MSG *key);
#ifdef STYLOS_MANAGER_INPUT
typedef void (*CanvasUpdate)(HWND canvas, unsigned char *view, POINT anchor, int value);
void BeforeCanvasGetMessage(HWND filter, UINT low, UINT high);
void FlushQueuedCanvasInput(void);
void ForgetQueuedCanvasInput(HWND canvas);
int CanvasInputValue(const MSG *input, HWND canvas, unsigned char *view, int offset);
void ApplyOrQueueCanvasInput(const MSG *input, HWND canvas, unsigned char *view,
    POINT anchor, int value, CanvasUpdate apply);
BOOL FastCanvasNavigationEnabled(void);
BOOL CanvasViewSupported(void);
void NoteCanvasNavigation(void *view);
#endif
void prepare_rotation_buttons(HWND canvas);
