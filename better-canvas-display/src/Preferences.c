#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>

int GetCanvasFilter(void);
int SetCanvasFilter(int mode);
static HWND frame;
static BYTE *host;
enum { Combo = 0x6e60, Label = 0x6e61 };
static const WCHAR placed[] = L"StylosCanvas.Display.Preference";
static const WCHAR choice[] = L"StylosCanvas.Display.Pending";
static UINT refresh_message;

static BOOL CALLBACK refresh_canvas(HWND w, LPARAM unused)
{
    WCHAR name[32];
    if (!GetClassNameW(w, name, 32) || lstrcmpW(name, L"CCelWindow")) return TRUE;
    BYTE *object = (BYTE *)GetWindowLongPtrW(w, GWLP_USERDATA);
    if (!object || *(void **)object != host + 0x5d279c || *(HWND *)(object+4) != w) return TRUE;
    BYTE *view = *(BYTE **)(object+0x24);
    if (view && *(void **)(*(BYTE **)view+0x8c) == host+0x59c60) {
        typedef void (__attribute__((thiscall)) *Rotate)(void *,int,int,int,int);
        ((Rotate)(host+0x671e0))(view,1,*(int *)(view+0x2d0),0,0);
    }
    return TRUE;
}

static LRESULT CALLBACK preferences(HWND w, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR id, DWORD_PTR data)
{
    if (msg == WM_COMMAND && LOWORD(wp) == IDOK)
        SetPropW(w, choice, (HANDLE)(SendDlgItemMessageW(w,Combo,CB_GETCURSEL,0,0)+1));
    if (msg == WM_CLOSE || (msg == WM_COMMAND && LOWORD(wp) == IDCANCEL)) RemovePropW(w,choice);
    if (msg == WM_DESTROY) {
        int mode=(int)(INT_PTR)RemovePropW(w,choice)-1;
        if (mode>=0 && mode!=GetCanvasFilter() && SetCanvasFilter(mode)) PostMessageW(frame,refresh_message,0,0);
    }
    if (msg == WM_NCDESTROY) RemoveWindowSubclass(w,preferences,id);
    return DefSubclassProc(w,msg,wp,lp);
}

static void place_controls(HWND w)
{
    HWND tree=GetDlgItem(w,1762), ok=GetDlgItem(w,IDOK);
    WCHAR klass[32];
    if (!tree || !ok || !GetDlgItem(w,1763) || !GetDlgItem(w,IDCANCEL) || GetPropW(w,placed)
        || !GetClassNameW(tree,klass,32) || lstrcmpW(klass,L"SysTreeView32")) return;
    RECT r, units={0,0,4,12};
    GetWindowRect(tree,&r);
    MapWindowPoints(NULL,w,(POINT *)&r,2);
    MapDialogRect(w,&units);
    HWND combo=GetDlgItem(w,Combo), label=NULL;
    BOOL legacy=combo!=NULL;
    if (legacy) label=FindWindowExW(w,NULL,L"Static",L"画布缩放");
    else {
        label=CreateWindowW(L"Static",L"画布缩放",WS_CHILD|WS_VISIBLE,0,0,0,0,w,(HMENU)Label,NULL,NULL);
        combo=CreateWindowW(L"ComboBox",L"",WS_TABSTOP|WS_CHILD|WS_VISIBLE|CBS_DROPDOWNLIST,
                            0,0,0,0,w,(HMENU)Combo,NULL,NULL);
        if (!label || !combo || !SetWindowSubclass(w,preferences,Combo,0)) {
            if(label) DestroyWindow(label);
            if(combo) DestroyWindow(combo);
            return;
        }
        const WCHAR *options[]={L"原始缩放",L"双线性",L"双立方"};
        for(int i=0;i<3;i++) SendMessageW(combo,CB_ADDSTRING,0,(LPARAM)options[i]);
        SendMessageW(combo,CB_SETCURSEL,GetCanvasFilter(),0);
    }
    if(!label) return;
    // Use the lower part of the category column. The midpoint patch owns the footer.
    int width=r.right-r.left, height=units.bottom;
    MoveWindow(tree,r.left,r.top,width,r.bottom-r.top-height*3,TRUE);
    MoveWindow(label,r.left,r.bottom-height*3+units.right,width,height,TRUE);
    MoveWindow(combo,r.left,r.bottom-2*height+units.right,width,6*height,TRUE);
    WPARAM font=SendMessageW(ok,WM_GETFONT,0,0);
    SendMessageW(label,WM_SETFONT,font,TRUE);
    SendMessageW(combo,WM_SETFONT,font,TRUE);
    SetPropW(w,placed,(HANDLE)1);
}

static LRESULT CALLBACK main_frame(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data)
{
    if (msg == refresh_message) { EnumChildWindows(w,refresh_canvas,0);return 0; }
    if (msg == WM_NCDESTROY) { RemoveWindowSubclass(w,main_frame,id);frame=NULL; }
    LRESULT result=DefSubclassProc(w,msg,wp,lp);

    return result;
}


// Preserve Stylos's WM_INITDIALOG return value and native control creation.
INT_PTR WINAPI CanvasPreferences(HWND w, WPARAM wp, LPARAM lp)
{
    host=(BYTE *)GetModuleHandleW(NULL);
    typedef INT_PTR (__cdecl *NativeInit)(HWND, WPARAM, LPARAM);
    INT_PTR result=((NativeInit)(host+0xdcc0))(w,wp,lp);
    if (GetDlgItem(w,1762) && GetDlgItem(w,1763) && GetDlgItem(w,IDOK)) {
        HWND owner=GetAncestor(w,GA_ROOTOWNER);
        if (!frame && owner) {
            refresh_message=RegisterWindowMessageW(L"RETAS.Stylos.BetterCanvas.Refresh");
            if (refresh_message && SetWindowSubclass(owner,main_frame,Combo,0)) frame=owner;
        }
        place_controls(w);
    }
    return result;
}