#define UNICODE
#include "CanvasView.h"
#include "plugin-services.h"
const RetasHost *canvasHost;
int GetCanvasFilter(void);
int SetCanvasFilter(int);
BOOL GetFastNavigation(void);
BOOL SetFastNavigation(BOOL);
BOOL FastCanvasFrame(BYTE *);
BOOL FastCanvasNavigationEnabled(void);
void WINAPI FilterCompositedTile(BYTE *,void *,void *,RECT *,RECT *);
BOOL FastNavigationComponent(void){return fast_api()!=NULL;}
BOOL CanvasDisplayComponent(void){return canvasHost->service("retas.canvas.filter/1")!=NULL;}
#ifndef RETAS_FAST
BOOL FastCanvasNavigationEnabled(void){FastCanvasAPI *f=fast_api();return f && f->enabled();}
BOOL FastCanvasFrame(BYTE *v){FastCanvasAPI *f=fast_api();return f && f->preview(v);}
#endif
static void WINAPI tile(BYTE *view,void *dst,void *src,RECT *buffer,RECT *out){
#ifdef RETAS_FAST
    if(CanvasDisplayComponent())return;
#endif
    FilterCompositedTile(view,dst,src,buffer,out);
}
static HWND WINAPI page(HWND dialog,RECT r){
    HWND panel=CreateWindowExW(WS_EX_CONTROLPARENT,L"Static",L"",WS_CHILD,
        r.left,r.top,r.right-r.left,r.bottom-r.top,dialog,NULL,NULL,NULL);
    if(!panel)return NULL;
    WPARAM font=SendDlgItemMessageW(dialog,IDOK,WM_GETFONT,0,0);
#ifdef RETAS_FAST
    HWND control=CreateWindowW(L"Button",L"旋转或缩放画布时使用快速预览(&F)",WS_CHILD|WS_VISIBLE|WS_TABSTOP|BS_AUTOCHECKBOX,
        8,8,r.right-r.left-16,36,panel,(HMENU)0x6e66,NULL,NULL);
    SendMessageW(control,BM_SETCHECK,GetFastNavigation()?BST_CHECKED:BST_UNCHECKED,0);
#else
    HWND label=CreateWindowW(L"Static",L"缩放插值(&I)",WS_CHILD|WS_VISIBLE,8,10,100,24,panel,NULL,NULL,NULL);
    SendMessageW(label,WM_SETFONT,font,FALSE);
    HWND control=CreateWindowW(L"ComboBox",L"",WS_CHILD|WS_VISIBLE|WS_TABSTOP|CBS_DROPDOWNLIST,
        112,8,r.right-r.left-120,140,panel,(HMENU)0x6e60,NULL,NULL);
    const WCHAR *names[]={L"原始缩放",L"双线性",L"双立方"};
    for(int i=0;i<3;i++)SendMessageW(control,CB_ADDSTRING,0,(LPARAM)names[i]);
    SendMessageW(control,CB_SETCURSEL,GetCanvasFilter(),0);
#endif
    SendMessageW(control,WM_SETFONT,font,FALSE);return panel;
}
static BOOL CALLBACK repaint(HWND w,LPARAM data){
    WCHAR name[32];BYTE *host=(BYTE *)GetModuleHandleW(NULL);
    if(GetClassNameW(w,name,32) && !lstrcmpW(name,L"CCelWindow")){
        BYTE *object=(BYTE *)GetWindowLongPtrW(w,GWLP_USERDATA);
        if(object && *(void **)object==host+CanvasVtable && *(HWND *)(object+4)==w){
            BYTE *v=*(BYTE **)(object+CanvasView);
            if(v)ApplyStylosRotation(host+SetRotation,v,*(int *)(v+RotationAngle));
        }
    }
    return TRUE;
}
static void WINAPI apply_page(HWND panel){
#ifdef RETAS_FAST
    BOOL enabled=IsDlgButtonChecked(panel,0x6e66)==BST_CHECKED;
    if(enabled==GetFastNavigation() || !SetFastNavigation(enabled))return;
#else
    int mode=SendDlgItemMessageW(panel,0x6e60,CB_GETCURSEL,0,0);
    if(mode==GetCanvasFilter() || !SetCanvasFilter(mode))return;
#endif
    EnumChildWindows(GetAncestor(panel,GA_ROOTOWNER),repaint,0);
}
BOOL WINAPI RetasPluginInit(const RetasHost *host,RetasPlugin *p){
    if(host->abi!=RETAS_PLUGIN_ABI || p->size<sizeof(*p))return FALSE;
    canvasHost=host;p->canvas_tile=tile;p->create_page=page;p->accept_page=apply_page;
#ifdef RETAS_FAST
    static FastCanvasAPI api={GetFastNavigation,FastCanvasFrame,NoteCanvasNavigation};
    return host->provide("retas.canvas.fast/1",&api);
#else
    static int version=1;
    return host->provide("retas.canvas.filter/1",&version);
#endif
}
