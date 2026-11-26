#include "retas.h"
#include <wchar.h>
#define PLUGIN_MANAGER_COMMAND 0x6e63
static HWND active_canvas(HWND w){
    HWND mdi=FindWindowExW(w,NULL,L"MDIClient",NULL);
    HWND canvas=mdi?(HWND)SendMessageW(mdi,WM_MDIGETACTIVE,0,0):NULL;
    WCHAR name[32];
    return canvas && GetClassNameW(canvas,name,32) && !wcscmp(name,L"CCelWindow")?canvas:NULL;
}
BOOL RetasDispatch(HWND w,UINT msg,WPARAM op,LPARAM arg,BOOL manager,LRESULT *result){
    static UINT registered;
    if(!registered)registered=RegisterWindowMessageW(RETAS_MESSAGE_V1);
    if(!registered || msg!=registered)return FALSE;
    HWND c=active_canvas(w);
    *result=0;
    switch(op){
    case RETAS_VERSION:*result=1;break;
    case RETAS_CAPABILITIES:*result=RETAS_VIEW|RETAS_PREFERENCES|(manager?RETAS_MANAGER:0);break;
    case RETAS_ACTIVE_CANVAS:*result=(LRESULT)c;break;
    case RETAS_ROTATION_DEGREES:case RETAS_ZOOM_BASIS_POINTS:
        if(c){
            WCHAR text[48];
            if(SendDlgItemMessageW(c,op==RETAS_ROTATION_DEGREES?122:121,WM_GETTEXT,48,(LPARAM)text))
                *result=op==RETAS_ROTATION_DEGREES?wcstol(text,NULL,10):(LONG)(wcstod(text,NULL)*100+.5);
        }break;
    case RETAS_OPEN_PREFERENCES:
        if(arg<0 || arg>3 || !IsWindowEnabled(w))break;
        if(arg)SetPropW(w,RETAS_PAGE_PROPERTY,(HANDLE)(INT_PTR)arg);else RemovePropW(w,RETAS_PAGE_PROPERTY);
        *result=PostMessageW(w,WM_COMMAND,40048,0);break;
    case RETAS_OPEN_MANAGER:
        if(manager && IsWindowEnabled(w))*result=PostMessageW(w,WM_COMMAND,PLUGIN_MANAGER_COMMAND,0);break;
    }
    return TRUE;
}
