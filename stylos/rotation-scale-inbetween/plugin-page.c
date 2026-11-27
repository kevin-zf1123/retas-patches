#define UNICODE
#include <windows.h>
#include "../../retas-devkit/plugin.h"
#define USER(name) name
#define CHILD(w,id) GetDlgItem(w,id)
#define ITEM(w,id,msg,wp,lp) SendDlgItemMessageW(w,id,msg,wp,lp)
static void explain(HWND w,const WCHAR *s) {MessageBoxW(w,s,L"中割对位",MB_OK|MB_ICONINFORMATION);}
#include "ratio_editor.h"

// This property and draft layout belong to the released midpoint v0.1 editor.
typedef struct { LPARAM original; HWND button; Settings settings; BOOL dirty; } MidpointDraft;
static INT_PTR CALLBACK mid_page(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_INITDIALOG){
        SetWindowLongPtrW(w,DWLP_USER,lp);
        editor_initialize(w,(Settings *)lp);
        return FALSE;
    }
    if(msg!=WM_COMMAND)return FALSE;
    if(LOWORD(wp)==IDOK || LOWORD(wp)==IDCANCEL)return FALSE;
    Settings *s=(Settings *)GetWindowLongPtrW(w,DWLP_USER);
    return s && editor_command(w,s,LOWORD(wp),HIWORD(wp));
}
static HMODULE module(void) {
    HMODULE m=NULL;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS|GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       (LPCWSTR)&mid_page,&m);
    return m;
}

static const WCHAR property[]=L"Stylos.InbetweenAlignment.Preferences";
static Settings draft;
static HWND WINAPI create(HWND w,RECT r){
    MidpointDraft *m=(MidpointDraft *)GetPropW(w,property);
    if(!m || !IsWindow(m->button) || GetParent(m->button)!=w || m->settings.count>CAPACITY)return NULL;
    draft=m->settings;
    HWND page=CreateDialogParamW(module(),MAKEINTRESOURCEW(301),w,mid_page,(LPARAM)&draft);
    if(!page)return NULL;
    RECT design;
    GetClientRect(page,&design);
    int width=r.right-r.left,height=r.bottom-r.top;
    if(design.right>0 && design.bottom>0){
        WPARAM font=SendDlgItemMessageW(w,IDOK,WM_GETFONT,0,0);
        for(HWND child=GetWindow(page,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
            RECT pos;GetWindowRect(child,&pos);MapWindowPoints(NULL,page,(POINT *)&pos,2);
            SendMessageW(child,WM_SETFONT,font,FALSE);
            MoveWindow(child,MulDiv(pos.left,width,design.right),MulDiv(pos.top,height,design.bottom),
                MulDiv(pos.right-pos.left,width,design.right),MulDiv(pos.bottom-pos.top,height,design.bottom),FALSE);
        }
    }
    SetWindowPos(page,HWND_TOP,r.left,r.top,width,height,SWP_NOACTIVATE);

    ShowWindow(m->button,SW_HIDE);return page;
}
static void WINAPI apply_page(HWND page){
    MidpointDraft *m=(MidpointDraft *)GetPropW(GetParent(page),property);
    editor_flags(page,&draft);
    if(m && memcmp(&m->settings,&draft,sizeof(draft))){m->settings=draft;m->dirty=TRUE;}
}
BOOL WINAPI RetasPluginInit(const RetasHost *host,RetasPlugin *p){
    if(host->abi!=RETAS_PLUGIN_ABI || p->size<sizeof(*p))return FALSE;
    p->create_page=create;p->stage_page=apply_page;return TRUE;
}
