#define UNICODE
#include <windows.h>
#include <commctrl.h>
#include "modules.h"
#include "../../retas-devkit/retas.h"
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

typedef struct { HWND child; HTREEITEM item; LoadedPlugin *module; } Page;
typedef struct {
    HWND tree, original, native;
    UINT count;
    BOOL accepted;
    Page pages[1];
} Dialog;
static UINT_PTR subclassId=0x6e65;
static const UINT finish=WM_APP+0x492;
static LRESULT CALLBACK dialog_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data){
    Dialog *d=(Dialog *)data;
    if(msg==finish){
        UINT requested=(UINT)(UINT_PTR)RemovePropW(GetAncestor(w,GA_ROOTOWNER),RETAS_PAGE_PROPERTY);
        HTREEITEM parent=find_plugins(d->tree);
        HWND native=GetDlgItem(w,1763);
        RECT r;GetWindowRect(native,&r);MapWindowPoints(NULL,w,(POINT *)&r,2);
        for(LoadedPlugin *p=modules;p;p=p->next){
            if(!p->callbacks.create_page)continue;
            HWND child=p->callbacks.create_page(w,r);
            if(!child)continue;
            Page *page=&d->pages[d->count++];
            page->child=child;page->module=p;
            ShowWindow(child,SW_HIDE);
            page->item=item(d->tree,parent,p->title);
        }
        TreeView_Expand(d->tree,parent,TVE_EXPAND);
        for(UINT i=0;requested && i<d->count;i++){
            LoadedPlugin *p=d->pages[i].module;
            if(requested==p->page || requested==p->legacyPage)TreeView_SelectItem(d->tree,d->pages[i].item);
        }
        return 0;
    }
    if(msg==WM_NOTIFY && lp && ((NMHDR *)lp)->hwndFrom==d->tree){
        NMTREEVIEWW *n=(NMTREEVIEWW *)lp;
        if(n->hdr.code==TVN_SELCHANGEDW || n->hdr.code==TVN_SELCHANGEDA){
            BOOL custom=FALSE;
            for(UINT i=0;i<d->count;i++){
                BOOL show=n->itemNew.hItem==d->pages[i].item;
                custom|=show;ShowWindow(d->pages[i].child,show?SW_SHOWNA:SW_HIDE);
            }
            if(custom && !d->native){
                for(HWND h=GetWindow(w,GW_CHILD);h;h=GetWindow(h,GW_HWNDNEXT)){
                    WCHAR c[24];
                    if(!(GetWindowLongW(h,GWL_STYLE)&WS_VISIBLE) || !GetClassNameW(h,c,24) || lstrcmpW(c,L"#32770"))continue;
                    BOOL plugin=FALSE;
                    for(UINT i=0;i<d->count;i++)if(h==d->pages[i].child)plugin=TRUE;
                    if(!plugin){d->native=h;ShowWindow(h,SW_HIDE);break;}
                }
            }else if(!custom){
                if(d->native){ShowWindow(d->native,SW_SHOWNA);d->native=NULL;}
                if(n->itemNew.lParam)TreeView_SelectItem(d->original,(HTREEITEM)n->itemNew.lParam);
            }
        }
        return FALSE;
    }
    if(msg==WM_COMMAND && LOWORD(wp)==IDOK){
        d->accepted=TRUE;
        for(UINT i=0;i<d->count;i++)if(d->pages[i].module->callbacks.stage_page)
            d->pages[i].module->callbacks.stage_page(d->pages[i].child);
    }
    if(msg==WM_CLOSE || (msg==WM_COMMAND && LOWORD(wp)==IDCANCEL))d->accepted=FALSE;
    if(msg==WM_DESTROY && d->accepted)
        for(UINT i=0;i<d->count;i++)if(d->pages[i].module->callbacks.accept_page)
            d->pages[i].module->callbacks.accept_page(d->pages[i].child);
    if(msg==WM_NCDESTROY){RemoveWindowSubclass(w,dialog_proc,id);HeapFree(GetProcessHeap(),0,d);}
    return DefSubclassProc(w,msg,wp,lp);
}
INT_PTR WINAPI CanvasPreferences(HWND w,WPARAM wp,LPARAM lp){
    typedef INT_PTR (__cdecl *NativeInit)(HWND,WPARAM,LPARAM);
    INT_PTR result=((NativeInit)((BYTE *)GetModuleHandleW(NULL)+0xdcc0))(w,wp,lp);
    UINT count=0;
    for(LoadedPlugin *p=modules;p;p=p->next)if(p->callbacks.create_page)count++;
    HWND tree=GetDlgItem(w,1762);
    if(!count || !tree || !GetDlgItem(w,1763))return result;
    Dialog *d=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*d)+(count-1)*sizeof(Page));
    if(!d)return result;
    RECT r;GetWindowRect(tree,&r);MapWindowPoints(NULL,w,(POINT *)&r,2);
    d->original=tree;
    d->tree=CreateWindowExW(GetWindowLongW(tree,GWL_EXSTYLE),WC_TREEVIEWW,L"",
        (GetWindowLongW(tree,GWL_STYLE)&~WS_VISIBLE)|TVS_HASBUTTONS|TVS_HASLINES|TVS_LINESATROOT,
        r.left,r.top,r.right-r.left,r.bottom-r.top,w,(HMENU)subclassId,NULL,NULL);
    if(!d->tree || !SetWindowSubclass(w,dialog_proc,subclassId,(DWORD_PTR)d)){
        if(d->tree)DestroyWindow(d->tree);HeapFree(GetProcessHeap(),0,d);return result;
    }
    SendMessageW(d->tree,WM_SETFONT,SendMessageW(tree,WM_GETFONT,0,0),FALSE);
    HTREEITEM current=mirror_items(tree,d->tree,TVI_ROOT,TVI_ROOT,TreeView_GetSelection(tree));
    if(current)TreeView_SelectItem(d->tree,current);
    ShowWindow(tree,SW_HIDE);ShowWindow(d->tree,SW_SHOWNA);
    PostMessageW(w,finish,0,0);
    return result;
}
INT_PTR WINAPI MidpointPreferences(HWND w,WPARAM wp,LPARAM lp){return CanvasPreferences(w,wp,lp);}
