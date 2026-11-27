#define UNICODE
#include <windows.h>
#include <commctrl.h>
#include <wchar.h>
#include "modules.h"
enum { Manager=0x6e63, Preferences=40048 };
static HWND frame;
static HMODULE host;
BOOL RetasDispatch(HWND,UINT,WPARAM,LPARAM,BOOL,LRESULT *);
static WCHAR path[MAX_PATH];
static BOOL registered;
__declspec(dllexport) BOOL CanvasInputOwned(void){return TRUE;}
static HMENU currentMenu;
static void label_native_plugin_path(HWND dialog){
    if(!GetDlgItem(dialog,1762) || !GetDlgItem(dialog,1763))return;
    for(HWND page=NULL;(page=FindWindowExW(dialog,page,L"#32770",NULL))!=NULL;){
        // Native preferences template 411: directory edit and browse button.
        if(!GetDlgItem(page,1045) || !GetDlgItem(page,1049))continue;
        HWND group=FindWindowExW(page,NULL,L"Button",NULL);
        if(group && (GetWindowLongW(group,GWL_STYLE)&BS_TYPEMASK)==BS_GROUPBOX){
            WCHAR caption[64];
            GetWindowTextW(group,caption,ARRAYSIZE(caption));
            if(!lstrcmpW(caption,L"插件文件夹位置(&P)") || !lstrcmpW(caption,L"插件路径(&P)"))
                SetWindowTextW(group,L"原版插件路径(&P)");
            else if(!lstrcmpW(caption,L"插件路径"))
                SetWindowTextW(group,L"原版插件路径");
        }
    }
}
static int find_item(HMENU menu,UINT command,HMENU *owner){
    for(int i=0;i<GetMenuItemCount(menu);i++){
        if(GetMenuItemID(menu,i)==command){*owner=menu;return i;}
        HMENU child=GetSubMenu(menu,i);
        if(child){int j=find_item(child,command,owner);if(j>=0)return j;}
    }
    return -1;
}
static void menu(HWND window){
    HMENU main=GetMenu(window),parent=NULL;
    if(!main || currentMenu==main)return;
    int i=find_item(main,Preferences,&parent);if(i<0)return;
    if(GetMenuState(parent,Manager,MF_BYCOMMAND)==(UINT)-1 &&
       !InsertMenuW(parent,i+1,MF_BYPOSITION|MF_STRING,Manager,L"插件管理器…"))return;
    currentMenu=main;
}
static void launch(HWND w){
    STARTUPINFOW si={sizeof(si)};PROCESS_INFORMATION pi={0};
    if(CreateProcessW(path,NULL,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi)){CloseHandle(pi.hThread);CloseHandle(pi.hProcess);}
    else MessageBoxW(w,L"请重新安装 Stylos 插件管理器。",L"插件管理器",MB_ICONERROR);
}
static LRESULT CALLBACK frame_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp,UINT_PTR id,DWORD_PTR data){
    if(msg==WM_ENTERIDLE && wp==MSGF_DIALOGBOX)
        label_native_plugin_path((HWND)lp);
    LRESULT api;
    if(RetasDispatch(w,msg,wp,lp,TRUE,&api))return api;
    if(msg==WM_COMMAND && !lp && LOWORD(wp)==Manager){launch(w);return 0;}
    if((msg==WM_INITMENU || (msg==WM_COMMAND && LOWORD(wp)==40049)))menu(w);
    if(msg==WM_NCDESTROY){RemoveWindowSubclass(w,frame_proc,id);frame=NULL;currentMenu=NULL;}
    LRESULT result=DefSubclassProc(w,msg,wp,lp);
    if(msg==WM_INITMENUPOPUP)EnableMenuItem((HMENU)wp,Manager,MF_BYCOMMAND|MF_ENABLED);
    return result;
}
typedef struct {int cmd; WORD key,mod;} Binding;
static BOOL manager_key(const MSG *m){
    if(!frame || (m->message!=WM_KEYDOWN && m->message!=WM_SYSKEYDOWN) ||
        !IsWindowEnabled(frame) || GetCapture() || GetAncestor(m->hwnd,GA_ROOTOWNER)!=frame)return FALSE;
    WCHAR cl[32];GetClassNameW(GetFocus(),cl,32);
    if(!lstrcmpiW(cl,L"Edit") || !lstrcmpiW(cl,L"ComboBox"))return FALSE;
    BYTE *app=*(BYTE **)((BYTE *)host+0x7095c8),*s=app?*(BYTE **)(app+0x80):NULL;
    if(!s||*(BYTE **)s!=(BYTE *)host+0x5d350c)return FALSE;
    int count=*(int *)(s+0x7c);Binding **items=*(Binding ***)(s+0x64);
    int mod=(GetKeyState(VK_CONTROL)<0)|((GetKeyState(VK_SHIFT)<0)<<1)|((GetKeyState(VK_MENU)<0)<<2);
    if(!items||count<0||count>4096)return FALSE;
    for(int i=0;i<count;i++)if(items[i] && items[i]->cmd==Manager && items[i]->mod==mod){
        WORD vk=((WORD (__cdecl *)(unsigned))((BYTE *)host+0x18370))(items[i]->key);
        if(vk && m->wParam==vk)return TRUE;
    }
    return FALSE;
}
static BOOL loaded;
static WORD word_at(const BYTE *p){WORD v;memcpy(&v,p,2);return v;}
static DWORD dword_at(const BYTE *p){DWORD v;memcpy(&v,p,4);return v;}
static void load_binding(void){
    if(loaded || !registered)return;
    BYTE *base=(BYTE *)host,*app=*(BYTE **)(base+0x7095c8);
    if(!app || !*(void **)(app+0x98))return;
    BYTE *s=*(BYTE **)(app+0x80);
    if(!s || *(BYTE **)s!=base+0x5d350c)return;
    int count=*(int *)(s+0x7c);Binding **rows=*(Binding ***)(s+0x64);
    if(count<0 || count>4096 || (!rows && count))return;
    loaded=TRUE;
    for(int i=0;i<count;i++)if(rows[i]->cmd==Manager)return;
    WCHAR file[MAX_PATH];DWORD n=GetEnvironmentVariableW(L"APPDATA",file,MAX_PATH);
    const WCHAR suffix[]=L"\\CELSYS\\RETAS STUDIO\\Stylos\\2.0\\Preference\\shortcut.rac";
    if(!n || n+ARRAYSIZE(suffix)>MAX_PATH)return;
    wcscat(file,suffix);
    HANDLE f=CreateFileW(file,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
    if(f==INVALID_HANDLE_VALUE)return;
    DWORD size=GetFileSize(f,NULL),got=0;BYTE *b=size>=22&&size<1048576?HeapAlloc(GetProcessHeap(),0,size):NULL;
    Binding key={Manager,0,0};
    if(b && ReadFile(f,b,size,&got,NULL) && got==size && !memcmp(b,"CARC",4) && !memcmp(b+10,"SLTS",4) && dword_at(b+14)<=size-18){
        DWORD end=18+dword_at(b+14);
        for(DWORD p=22;p+10<=end;){
            unsigned l=word_at(b+p+4),step=dword_at(b+p);
            if(step!=l+6 || step>end-p-4)break;
            if(l==19 && !memcmp(b+p+6,"EDIT_PLUGIN_MANAGER",19)){key.key=word_at(b+p+6+l);key.mod=word_at(b+p+8+l);break;}
            p+=4+step;
        }
    }
    if(b)HeapFree(GetProcessHeap(),0,b);CloseHandle(f);if(!key.key)return;
    for(int i=0;i<count;i++)if(rows[i]->key==key.key && rows[i]->mod==key.mod)return;
    Binding *record=((void *(__cdecl *)(size_t))(base+0x35ab0a))(sizeof(Binding));if(!record)return;
    *record=key;
    typedef void (__attribute__((thiscall)) *Resize)(void *,int);
    typedef void (__attribute__((thiscall)) *Refresh)(void *);
    ((Resize)(base+0x25450))(s+0x5c,count+1);(*(Binding ***)(s+0x64))[count]=record;
    ((Refresh)(base+0x19d50))(s);
}static void register_command(void){
    BYTE *base=(BYTE *)host;
    if(registered || *(BYTE **)(base+0x70836c)!=base+0x5d0c44)return;
    typedef void (__attribute__((thiscall)) *Link)(void*,int,const char*);
    ((Link)(base+0xde60))(base+0x70836c,Manager,"EDIT_PLUGIN_MANAGER");
    registered=TRUE;
}
BOOL WINAPI PluginManagerMessage(MSG *message,HWND w,UINT lo,UINT hi){
    if(!host){
        host=GetModuleHandleW(NULL);
        DWORD n=GetModuleFileNameW(NULL,path,MAX_PATH);
        WCHAR *leaf=n && n<MAX_PATH?wcsrchr(path,L'\\'):NULL;
        if(leaf && leaf-path+24<MAX_PATH)wcscpy(leaf+1,L"StylosPluginManager.exe");
        else path[0]=0;
        LoadModules();
    }
    register_command();load_binding();
    for(LoadedPlugin *p=modules;p;p=p->next)if(p->callbacks.before_message)p->callbacks.before_message(w,lo,hi);
    BOOL result=GetMessageA(message,w,lo,hi);
    if(result<=0)return result;
    if(!frame && message->hwnd){
        HWND candidate=GetAncestor(message->hwnd,GA_ROOTOWNER);WCHAR klass[32];
        if(GetClassNameW(candidate,klass,32) && !lstrcmpW(klass,L"DrawFrameWnd") && GetMenu(candidate) &&
           SetWindowSubclass(candidate,frame_proc,Manager,0)){frame=candidate;menu(frame);}
    }
    for(LoadedPlugin *p=modules;p;p=p->next)if(p->callbacks.message)p->callbacks.message(message);
    if(manager_key(message)){PostMessageW(frame,WM_COMMAND,Manager,0);message->message=WM_NULL;}
    return result;
}
