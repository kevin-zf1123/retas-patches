#define UNICODE
#include <windows.h>
#include <wchar.h>
#include <string.h>
#include "modules.h"

LoadedPlugin *modules;
typedef struct Service {
    struct Service *next;
    char name[128];
    void *value;
} Service;
static Service *services;

static void *WINAPI lookup(const char *name) {
    for(Service *s=services;s;s=s->next) if(!strcmp(name,s->name))return s->value;
    return NULL;
}
static BOOL WINAPI provide(const char *name,void *value) {
    if(!name || !value || strlen(name)>=128 || lookup(name))return FALSE;
    Service *s=HeapAlloc(GetProcessHeap(),0,sizeof(*s));
    if(!s)return FALSE;
    strcpy(s->name,name);s->value=value;s->next=services;services=s;
    return TRUE;
}
static const RetasHost api={sizeof(RetasHost),RETAS_PLUGIN_ABI,lookup,provide};

void LoadModules(void) {
    static BOOL started;
    if(started)return;
    started=TRUE;
    WCHAR root[MAX_PATH],ini[MAX_PATH];
    DWORD n=GetModuleFileNameW(NULL,root,MAX_PATH);
    if(!n || n>=MAX_PATH)return;
    WCHAR *leaf=wcsrchr(root,L'\\');
    if(!leaf)return;
    leaf[1]=0;
    if(wcslen(root)+40>=MAX_PATH)return;
    wcscpy(ini,root);wcscat(ini,L".stylos-plugins\\modules.ini");
    UINT count=GetPrivateProfileIntW(L"Modules",L"Count",0,ini);
    LoadedPlugin **tail=&modules;
    for(UINT i=0;i<count;i++){
        WCHAR section[32],path[MAX_PATH],message[160],id[96];
        wsprintfW(section,L"Module%u",i);
        if(!GetPrivateProfileStringW(section,L"Id",L"",id,ARRAYSIZE(id),ini))break;
        if(wcsspn(id,L"abcdefghijklmnopqrstuvwxyz0123456789-")!=wcslen(id) ||
           wcslen(root)+wcslen(id)+wcslen(L".stylos-plugins\\active\\\\module.dll")>=MAX_PATH)continue;
        wsprintfW(path,L"%s.stylos-plugins\\active\\%s\\module.dll",root,id);
        LoadedPlugin *p=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,sizeof(*p));
        if(!p)break;
        wcscpy(p->id,id);
        GetPrivateProfileStringW(section,L"Title",L"",p->title,ARRAYSIZE(p->title),ini);
        p->legacyPage=GetPrivateProfileIntW(section,L"Page",0,ini);
        wsprintfW(message,L"RETAS.Stylos.Settings.%s",p->id);
        p->page=RegisterWindowMessageW(message);
        p->library=LoadLibraryExW(path,NULL,LOAD_WITH_ALTERED_SEARCH_PATH);
        RetasInit init=p->library?(RetasInit)GetProcAddress(p->library,"RetasPluginInit"):NULL;
        p->callbacks.size=sizeof(p->callbacks);p->callbacks.abi=RETAS_PLUGIN_ABI;
        Service *prior=services;
        if(init && init(&api,&p->callbacks) && p->callbacks.abi==RETAS_PLUGIN_ABI &&
           p->callbacks.size==sizeof(p->callbacks)){
            *tail=p;tail=&p->next;
        }else{
            OutputDebugStringW(L"RETAS: plugin initialization failed: ");OutputDebugStringW(path);
            while(services!=prior){Service *s=services;services=s->next;HeapFree(GetProcessHeap(),0,s);}
            if(p->library)FreeLibrary(p->library);
            HeapFree(GetProcessHeap(),0,p);
        }
    }
}
int NativeTile(void *,void *,void *,void *,void *,void *,void *,void *);
static int tile(DWORD rva,BYTE *view,void *dst,void *src,void *origin,RECT *buffer,void *a,void *b,RECT *out){
#ifdef RETAS_TRACE_TILE
    extern void CanvasTrace(unsigned,int,int);
    CanvasTrace(1,rva==0x629d0,0);
#endif
    int ok=NativeTile((BYTE *)GetModuleHandleW(NULL)+rva,view,dst,src,origin,buffer,a,b);
#ifdef RETAS_TRACE_TILE
    CanvasTrace(2,0,0);
#endif
    if(ok)for(LoadedPlugin *p=modules;p;p=p->next)
        if(p->callbacks.canvas_tile)p->callbacks.canvas_tile(view,dst,src,buffer,out);
#ifdef RETAS_TRACE_TILE
    CanvasTrace(7,0,0);
#endif
    return ok;
}
int WINAPI CanvasScale(BYTE *v,void *d,void *s,void *o,RECT *b,RECT *src,RECT *out){
    return tile(0x61060,v,d,s,o,b,src,out,out);
}
int WINAPI CanvasRotate(BYTE *v,void *d,void *s,void *o,RECT *b,RECT *out,int block){
    return tile(0x629d0,v,d,s,o,b,out,(void *)block,out);
}
