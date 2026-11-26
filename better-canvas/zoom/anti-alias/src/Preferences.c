#define UNICODE
#define _UNICODE
#include <windows.h>
#include <commctrl.h>

int GetCanvasFilter(void);
int SetCanvasFilter(int mode);
static HWND frame;
static BYTE *host;
enum { Combo = 0x6e60, Label = 0x6e61 };

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


#define USER(name) name
#define CHILD(w,id) GetDlgItem(w,id)
#define ITEM(w,id,msg,wp,lp) SendDlgItemMessageW(w,id,msg,wp,lp)
static void explain(HWND w,const WCHAR *s) {MessageBoxW(w,s,L"中割对位",MB_OK|MB_ICONINFORMATION);}
#include "../../../../stylos/rotation-scale-inbetween/ratio_editor.h"

// This property and draft layout belong to the released midpoint v0.1 editor.
typedef struct { LPARAM original; HWND button; Settings settings; BOOL dirty; } MidpointDraft;
typedef struct {
    HWND tree, originalTree, canvasLabel, combo, group, midPage, nativePage;
    HTREEITEM plugins, canvasItem, midItem;
    RECT page;
    Settings draft;
    BOOL midpointReady, accepted;
} Pages;

static const UINT finishInit=WM_APP+0x492;
static const WCHAR midpointProperty[]=L"Stylos.InbetweenAlignment.Preferences";

static HTREEITEM item(HWND tree,HTREEITEM parent,const WCHAR *text) {
    TVINSERTSTRUCTW insert={0};
    insert.hParent=parent;insert.hInsertAfter=TVI_LAST;
    insert.item.mask=TVIF_TEXT;insert.item.pszText=(WCHAR *)text;
    return (HTREEITEM)SendMessageW(tree,TVM_INSERTITEMW,0,(LPARAM)&insert);
}
// Stylos treats every item of its category tree as an internal page. Keep those
// objects in its own tree and mirror navigation in the visible Win32 tree.
static HTREEITEM mirror_items(HWND original, HWND mirror, HTREEITEM originalParent,
                               HTREEITEM mirrorParent, HTREEITEM selected) {
    HTREEITEM match=NULL;
    for(HTREEITEM h=originalParent==TVI_ROOT?TreeView_GetRoot(original):TreeView_GetChild(original,originalParent);h;h=TreeView_GetNextSibling(original,h)){
        WCHAR label[256]={0};TVITEMW v={0};
        v.mask=TVIF_TEXT|TVIF_STATE;v.stateMask=TVIS_EXPANDED;v.hItem=h;v.pszText=label;v.cchTextMax=256;
        if(!SendMessageW(original,TVM_GETITEMW,0,(LPARAM)&v))continue;
        HTREEITEM copy=item(mirror,mirrorParent,label);
        if(!copy)continue;
        TVITEMW route={0};route.mask=TVIF_PARAM;route.hItem=copy;route.lParam=(LPARAM)h;
        SendMessageW(mirror,TVM_SETITEMW,0,(LPARAM)&route);
        HTREEITEM child=mirror_items(original,mirror,h,copy,selected);
        if(v.state&TVIS_EXPANDED)TreeView_Expand(mirror,copy,TVE_EXPAND);
        if(h==selected)match=copy;else if(child)match=child;
    }
    return match;
}
static HTREEITEM find_plugins(HWND tree) {
    for(HTREEITEM i=TreeView_GetRoot(tree);i;i=TreeView_GetNextSibling(tree,i)){
        WCHAR buf[96]={0};TVITEMW v={0};
        v.mask=TVIF_TEXT;v.hItem=i;v.pszText=buf;v.cchTextMax=96;
        if(SendMessageW(tree,TVM_GETITEMW,0,(LPARAM)&v) &&
           (!lstrcmpW(buf,L"插件") || !lstrcmpW(buf,L"プラグイン") ||
            !lstrcmpiW(buf,L"Plugins") || !lstrcmpiW(buf,L"Plug-in")))return i;
    }
    return item(tree,TVI_ROOT,L"插件");
}
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
static HWND find_native_page(HWND w,const Pages *p) {
    for(HWND h=GetWindow(w,GW_CHILD);h;h=GetWindow(h,GW_HWNDNEXT)){
        WCHAR c[24];
        if(h!=p->midPage && (GetWindowLongW(h,GWL_STYLE)&WS_VISIBLE) &&
           GetClassNameW(h,c,24) && !lstrcmpW(c,L"#32770"))return h;
    }
    return NULL;
}
static void show_page(HWND w,Pages *p,HTREEITEM sel) {
    BOOL canvas=p->canvasItem && sel==p->canvasItem,mid=p->midItem && sel==p->midItem;
    if(canvas || mid){
        if(!p->nativePage)p->nativePage=find_native_page(w,p);
        if(p->nativePage)ShowWindow(p->nativePage,SW_HIDE);
    }
    if(p->group) ShowWindow(p->group,canvas?SW_SHOWNA:SW_HIDE);
    if(p->canvasLabel)ShowWindow(p->canvasLabel,canvas?SW_SHOWNA:SW_HIDE);
    ShowWindow(p->combo,canvas?SW_SHOWNA:SW_HIDE);
    if(p->midPage)ShowWindow(p->midPage,mid?SW_SHOWNA:SW_HIDE);
    if(!canvas && !mid && p->nativePage){ShowWindow(p->nativePage,SW_SHOWNA);p->nativePage=NULL;}
}
static void attach_midpoint(HWND w,Pages *p) {
    if(p->midpointReady)return;
    MidpointDraft *m=(MidpointDraft *)GetPropW(w,midpointProperty);
    if(!m || !IsWindow(m->button) || GetParent(m->button)!=w || m->settings.count>CAPACITY)return;
    p->draft=m->settings;
    p->midPage=CreateDialogParamW(module(),MAKEINTRESOURCEW(301),w,mid_page,(LPARAM)&p->draft);
    if(!p->midPage)return;
    RECT r=p->page, design;
    GetClientRect(p->midPage,&design);
    int width=r.right-r.left,height=r.bottom-r.top;
    if(design.right>0 && design.bottom>0){
        WPARAM font=SendDlgItemMessageW(w,IDOK,WM_GETFONT,0,0);
        for(HWND child=GetWindow(p->midPage,GW_CHILD);child;child=GetWindow(child,GW_HWNDNEXT)){
            RECT pos;GetWindowRect(child,&pos);MapWindowPoints(NULL,p->midPage,(POINT *)&pos,2);
            SendMessageW(child,WM_SETFONT,font,FALSE);
            MoveWindow(child,MulDiv(pos.left,width,design.right),MulDiv(pos.top,height,design.bottom),
                MulDiv(pos.right-pos.left,width,design.right),MulDiv(pos.bottom-pos.top,height,design.bottom),FALSE);
        }
    }
    SetWindowPos(p->midPage,HWND_TOP,r.left,r.top,width,height,SWP_NOACTIVATE);
    p->midItem=item(p->tree,p->plugins,L"中割对位");
    if(!p->midItem){DestroyWindow(p->midPage);p->midPage=NULL;return;}
    p->midpointReady=TRUE;
    ShowWindow(m->button,SW_HIDE);
    TreeView_Expand(p->tree,p->plugins,TVE_EXPAND);
}
static LRESULT CALLBACK preferences(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    Pages *p=(Pages *)data;
    if(msg==finishInit){
        attach_midpoint(w,p);
        int page=(int)(INT_PTR)RemovePropW(GetAncestor(w,GA_ROOTOWNER),L"RETAS.Stylos.DevKit.RequestedPage");
        HTREEITEM select=page==1?p->canvasItem:page==2?p->midItem:NULL;
        if(select)TreeView_SelectItem(p->tree,select);
        return 0;
    }
    if(msg==WM_NOTIFY && lp){
        NMHDR *header=(NMHDR *)lp;
        if(header->hwndFrom==p->tree){
            if(header->code==TVN_SELCHANGEDA || header->code==TVN_SELCHANGEDW){
                NMTREEVIEWW *n=(NMTREEVIEWW *)lp;
                HTREEITEM sel=n->itemNew.hItem;
                show_page(w,p,sel);
                if(sel!=p->canvasItem && sel!=p->midItem && n->itemNew.lParam)
                    TreeView_SelectItem(p->originalTree,(HTREEITEM)n->itemNew.lParam);
            }
            return FALSE;
        }
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK) {
        p->accepted=TRUE;
        MidpointDraft *m=(MidpointDraft *)GetPropW(w,midpointProperty);
        if(m && p->midpointReady){
            editor_flags(p->midPage,&p->draft);
            if(memcmp(&m->settings,&p->draft,sizeof(Settings))!=0){m->settings=p->draft;m->dirty=TRUE;}
        }
        if(p->canvasItem)SetPropW(w,choice,(HANDLE)(SendMessageW(p->combo,CB_GETCURSEL,0,0)+1));
    }
    if(msg==WM_CLOSE || (msg==WM_COMMAND && LOWORD(wp)==IDCANCEL))p->accepted=FALSE;
    if(msg==WM_DESTROY){
        int mode=(int)(INT_PTR)RemovePropW(w,choice)-1;
        if(p->accepted && mode>=0 && mode!=GetCanvasFilter() && SetCanvasFilter(mode))
            PostMessageW(frame,refresh_message,0,0);
    }
    if(msg==WM_NCDESTROY){RemoveWindowSubclass(w,preferences,id);HeapFree(GetProcessHeap(),0,p);}
    return DefSubclassProc(w,msg,wp,lp);
}
static void setup(HWND w,BOOL canvas) {
    HWND tree=GetDlgItem(w,1762),ok=GetDlgItem(w,IDOK);
    if(!tree || !ok || !GetDlgItem(w,1763) || !GetDlgItem(w,IDCANCEL))return;
    Pages *p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*p));
    if(!p)return;
    p->originalTree=tree;
    HWND page=find_native_page(w,p);
    if(!page)page=GetDlgItem(w,1763);
    GetWindowRect(page,&p->page);MapWindowPoints(NULL,w,(POINT *)&p->page,2);
    if(!SetWindowSubclass(w,preferences,Combo,(DWORD_PTR)p)){HeapFree(GetProcessHeap(),0,p);return;}
    RECT nav;GetWindowRect(tree,&nav);MapWindowPoints(NULL,w,(POINT *)&nav,2);
    DWORD style=(GetWindowLongW(tree,GWL_STYLE) & ~WS_VISIBLE)|TVS_HASBUTTONS|TVS_HASLINES|TVS_LINESATROOT;
    p->tree=CreateWindowExW(GetWindowLongW(tree,GWL_EXSTYLE),WC_TREEVIEWW,L"",style,
        nav.left,nav.top,nav.right-nav.left,nav.bottom-nav.top,w,(HMENU)0x6e65,module(),NULL);
    if(!p->tree)return;
    SendMessageW(p->tree,WM_SETFONT,SendMessageW(tree,WM_GETFONT,0,0),FALSE);
    HTREEITEM current=mirror_items(tree,p->tree,TVI_ROOT,TVI_ROOT,TreeView_GetSelection(tree));
    if(!TreeView_GetRoot(p->tree))return;
    p->plugins=find_plugins(p->tree);
    if(current)TreeView_SelectItem(p->tree,current);
    BOOL focused=GetFocus()==tree;
    ShowWindow(tree,SW_HIDE);ShowWindow(p->tree,SW_SHOWNA);
    if(focused)SetFocus(p->tree);
    if(!p->plugins)return;
    RECT r=p->page,du={8,8,72,16};MapDialogRect(w,&du);
    int left=r.left+du.left,top=r.top+du.top,width=r.right-r.left-2*du.left;
    // Keep the combo as a direct child for compatibility with earlier tablet modules.
    p->combo=CreateWindowW(L"ComboBox",L"",WS_CHILD|WS_TABSTOP|CBS_DROPDOWNLIST,
        left+du.left+du.right,top+du.bottom,width-du.right-3*du.left,du.bottom*7,
        w,(HMENU)Combo,module(),NULL);
    if(canvas && p->combo){
        p->group=CreateWindowW(L"Button",L"画布显示",WS_CHILD|BS_GROUPBOX,
            left,top,width,du.bottom*3,w,NULL,module(),NULL);
        p->canvasLabel=CreateWindowW(L"Static",L"缩放插值(&I)",WS_CHILD|SS_CENTERIMAGE,
            left+du.left,top+du.bottom,du.right,du.bottom,w,(HMENU)Label,module(),NULL);
        WPARAM font=SendMessageW(ok,WM_GETFONT,0,0);
        SendMessageW(p->group,WM_SETFONT,font,TRUE);SendMessageW(p->canvasLabel,WM_SETFONT,font,TRUE);
        SendMessageW(p->combo,WM_SETFONT,font,TRUE);
        const WCHAR *options[]={L"原始缩放",L"双线性",L"双立方"};
        for(int i=0;i<3;i++)SendMessageW(p->combo,CB_ADDSTRING,0,(LPARAM)options[i]);
        SendMessageW(p->combo,CB_SETCURSEL,GetCanvasFilter(),0);
        p->canvasItem=item(p->tree,p->plugins,L"画布显示");
        TreeView_Expand(p->tree,p->plugins,TVE_EXPAND);
    }
    PostMessageW(w,finishInit,0,0); // The v0.1 modal creates its settings draft after native WM_INITDIALOG.
}
static LRESULT CALLBACK main_frame(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data) {
    if(msg==refresh_message){EnumChildWindows(w,refresh_canvas,0);return 0;}
    if(msg==WM_NCDESTROY){RemoveWindowSubclass(w,main_frame,id);frame=NULL;}
    return DefSubclassProc(w,msg,wp,lp);
}
static INT_PTR initialize_pages(HWND w,WPARAM wp,LPARAM lp,BOOL canvas) {
    host=(BYTE *)GetModuleHandleW(NULL);
    typedef INT_PTR (__cdecl *NativeInit)(HWND, WPARAM, LPARAM);
    INT_PTR result=((NativeInit)(host+0xdcc0))(w,wp,lp);
    if(GetDlgItem(w,1762) && GetDlgItem(w,1763) && GetDlgItem(w,IDOK)){
        HWND owner=GetAncestor(w,GA_ROOTOWNER);
        if(!frame && owner){
            refresh_message=RegisterWindowMessageW(L"RETAS.Stylos.BetterCanvas.Refresh");
            if(refresh_message && SetWindowSubclass(owner,main_frame,Combo,0))frame=owner;
        }
        setup(w,canvas);
    }
    return result;
}
INT_PTR WINAPI CanvasPreferences(HWND w,WPARAM wp,LPARAM lp){return initialize_pages(w,wp,lp,TRUE);}
INT_PTR WINAPI MidpointPreferences(HWND w,WPARAM wp,LPARAM lp){return initialize_pages(w,wp,lp,FALSE);}
