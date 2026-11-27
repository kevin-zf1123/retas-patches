#pragma once
#include <windows.h>

/* Stylos 2.6 x86 in-process ABI. Init is called on the UI thread after the
   normal DLL load, never in DllMain. All callbacks and services last until exit. */
#define RETAS_PLUGIN_ABI 1
typedef struct RetasHost {
    DWORD size, abi;
    void *(WINAPI *service)(const char *name);
    BOOL (WINAPI *provide)(const char *name, void *value);
} RetasHost;
typedef struct RetasPlugin {
    DWORD size, abi;
    void (WINAPI *before_message)(HWND filter, UINT low, UINT high);
    void (WINAPI *message)(MSG *message);
    void (WINAPI *canvas_tile)(BYTE *view, void *bitmap, void *source, RECT *buffer, RECT *updated);
    /* Create an initially hidden child of dialog in the given client rectangle.
       Return NULL if this page is unavailable. Host shows/hides the returned HWND. */
    HWND (WINAPI *create_page)(HWND dialog, RECT area);
    /* Persist only when the outer dialog actually closes with OK. */
    void (WINAPI *accept_page)(HWND page);
    /* Optional: stage a value in a native host draft before its OK handler. */
    void (WINAPI *stage_page)(HWND page);
} RetasPlugin;
typedef BOOL (WINAPI *RetasInit)(const RetasHost *, RetasPlugin *);
/* Export RetasPluginInit by this exact, undecorated name (see example .def). */
