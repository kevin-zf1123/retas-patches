#ifndef STYLOS_INBETWEEN_UI_H
#define STYLOS_INBETWEEN_UI_H
#include <windows.h>

/* Slot order is part of the previous installer format. */
enum { SetX, SetY, SetScale, SetAngle, Refresh, LengthGet, LengthSet,
       Module, Procedure, Message, SetText, GetText, End, GetApp, GetSet,
       NativePreferencesModal, NativePreferencesProc, ViewToPixels, RebuildRow };
extern __attribute__((visibility("hidden"))) const volatile int stylos_links[];
static inline void *address(int slot) { return (char *)stylos_links + stylos_links[slot]; }
#define IMPORT(slot,type) (*(type *)address(slot))
static inline void *system_api(const char *module,const char *name) {
    return (void *)IMPORT(Procedure,__typeof__(&GetProcAddress))(
        IMPORT(Module,__typeof__(&GetModuleHandleA))(module),name);
}
#define USER(name) ((__typeof__(&name))system_api("user32.dll",#name))
#define REG(name) ((__typeof__(&name))system_api("advapi32.dll",#name))
void explain(HWND owner,const WCHAR *message);
enum { FOUR_POINTS=0x8000 };
int choose_step(HWND owner,BOOL *skip_preview);
#endif
