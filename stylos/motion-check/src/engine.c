
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <wincrypt.h>
#include <compressapi.h>
#include <stdint.h>
#include <stdio.h>
#include <wchar.h>
#include <stdlib.h>
#include <string.h>
#include <process.h>
#include "engine.h"

#define EXPORT __declspec(dllexport)
#define THIS __attribute__((thiscall))
#define V(p,k) (*(int*)((char*)(p)+(k)))
#define PTR(p,k) (*(void**)((char*)(p)+(k)))
#define CALLPTR(a,t) (*(t*)(uintptr_t)(a))
typedef void(THIS *PathText)(void*,char*);
typedef void(THIS *BmpCreate)(void*,HWND,int,int,int,int,int);
typedef void(THIS *DestroyBitmap)(void*);
typedef void(THIS *Dispose)(void*,int);
typedef void*(THIS *Probe)(void*,int,int,int,int,int,int,int,void*);
#include "frame_cache.h"
#include "cache_space.h"
static BOOL native_path(void* obj,wchar_t path[MAX_PATH]){
    char raw[8192]={0};CALLPTR(0x9cfe94,PathText)(obj,raw);
    return raw[0] && MultiByteToWideChar(CP_ACP,0,raw,-1,path,MAX_PATH)>0;
}
static BOOL isext(const wchar_t* p,const wchar_t* ext){const wchar_t* e=wcsrchr(p,L'.');return e&&!_wcsicmp(e,ext);}
EXPORT int __cdecl MotionSettingsVersion(void){return 1;}
static BOOL cache_dir(wchar_t dir[MAX_PATH]){
    RenderSettings config=render_settings();
    return ensure_render_cache(&config,dir);
}
static void trace(const char* type,int frame,int value){
    wchar_t path[MAX_PATH];if(!GetEnvironmentVariableW(L"STYLOS_RENDER_TRACE",path,MAX_PATH))return;
    HANDLE f=CreateFileW(path,FILE_APPEND_DATA,FILE_SHARE_READ|FILE_SHARE_WRITE,0,OPEN_ALWAYS,0,0);
    if(f==INVALID_HANDLE_VALUE)return;char line[112];int n=snprintf(line,sizeof(line),"%lu %d %s %d %d\r\n",(unsigned long)GetCurrentProcessId(),-1,type,frame,value);
    DWORD wrote;WriteFile(f,line,n,&wrote,0);CloseHandle(f);
}
EXPORT int __cdecl Background(void){return 0;}
static BOOL wine_runtime(void){
    return GetProcAddress(GetModuleHandleW(L"ntdll.dll"),"wine_get_version")!=NULL;
}
#include "scheduler.inc"
typedef struct {wchar_t path[MAX_PATH];BYTE id[32];} Dependency;
typedef struct {
    Digest key;BOOL probing,valid,missing;unsigned count;Dependency deps[MaxFrameInputs];
    wchar_t file[MAX_PATH];int w,h,frame;
} Frame;
static __thread Frame current;
static void append_file(const wchar_t* path){
    Frame* f=&current;if(!f->valid)return;
    if(f->count>=MaxFrameInputs){f->valid=FALSE;return;}
    Dependency* d=f->deps+f->count++;
    wcscpy(d->path,path);f->valid=file_digest(path,d->id)&&add(&f->key,d->id,32);
}
typedef struct OpenCel {void* doc;struct OpenCel* next;} OpenCel;
static __thread OpenCel* cel_list;
static __thread void* rendered_doc;
EXPORT void __cdecl TrackCel(void* doc){
    OpenCel* n=malloc(sizeof(*n));if(n){n->doc=doc;n->next=cel_list;cel_list=n;}
}
EXPORT void __cdecl UntrackCel(void* doc){
    OpenCel** pp=&cel_list;while(*pp){if((*pp)->doc==doc){OpenCel* e=*pp;*pp=e->next;free(e);return;}pp=&(*pp)->next;}
}
static void* dirty_cel(const wchar_t* path,void* except){
    wchar_t name[MAX_PATH];typedef int(THIS *Modified)(void*);
    for(OpenCel* n=cel_list;n;n=n->next){
        void* doc=n->doc;
        if(doc==except||PTR(doc,0)!=(void*)0x9d85f4)continue;
        if(native_path((char*)doc+16,name)&&!_wcsicmp(name,path)&&((Modified)(*(void***)doc)[14])(doc))return doc;
    }
    return NULL;
}
EXPORT void* __cdecl RenderCel(void* temporary,void* args){
    typedef void*(THIS *Render)(void*,void*);
    wchar_t path[MAX_PATH];void* live=NULL;
    if(native_path((char*)temporary+16,path))live=dirty_cel(path,temporary);
    void* source=live?live:temporary;
    if(live){current.valid=FALSE;trace("unsaved",current.frame,0);}
    void* pixels=((Render)0x440a10)(source,args);
    rendered_doc=source;return pixels;
}
EXPORT void* __cdecl CelGeometry(void){return (char*)rendered_doc+0x80;}
EXPORT int __cdecl DependencyPath(void* frame){
    if(!current.probing)return 0;
    MEMORY_BASIC_INFORMATION site,ours;if(!VirtualQuery(PTR(frame,4),&site,sizeof(site))||!VirtualQuery((void*)DependencyPath,&ours,sizeof(ours))||site.AllocationBase!=ours.AllocationBase)return 0;
    wchar_t path[MAX_PATH];if(!native_path((char*)frame-0x74,path))current.valid=FALSE;
    else if(dirty_cel(path,NULL))current.valid=FALSE;else append_file(path);
    return 1;
}
EXPORT void __cdecl MissingSource(void){if(current.probing)current.missing=TRUE;}
static BOOL unchanged(void){
    BYTE h[32];for(unsigned n=0;n<current.count;n++)
        if(!file_digest(current.deps[n].path,h)||memcmp(h,current.deps[n].id,32))return FALSE;
    return TRUE;
}

// Stylos ships this cdecl zlib ABI. Wine's cabinet.dll exports compression stubs.
typedef int (__cdecl *ZlibCompress)(BYTE*,DWORD*,const BYTE*,DWORD,int);
typedef int (__cdecl *ZlibExpand)(BYTE*,DWORD*,const BYTE*,DWORD);
static FARPROC host_zlib(const char* name){
    HMODULE zlib=GetModuleHandleW(L"zlib.dll");
    return zlib?GetProcAddress(zlib,name):NULL;
}
static void bitmap_info(BITMAPINFO* bi,int w,int h){
    memset(bi,0,sizeof(*bi));bi->bmiHeader.biSize=sizeof(BITMAPINFOHEADER);bi->bmiHeader.biWidth=w;
    bi->bmiHeader.biHeight=-h;bi->bmiHeader.biPlanes=1;bi->bmiHeader.biBitCount=32;
}
static BOOL getall(HANDLE f,void* p,DWORD n){DWORD r=0;return ReadFile(f,p,n,&r,0)&&r==n;}
static BOOL load_frame(void* bp,void* bitmap){
    Frame* f=&current;DWORD size=(DWORD)f->w*f->h*4;
    HANDLE fd=CreateFileW(f->file,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,0,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL,0);
    if(fd==INVALID_HANDLE_VALUE)return FALSE;
    Header h;LARGE_INTEGER fileSize;BYTE *blob=0,*pixels=0;BOOL ok=FALSE;DECOMPRESSOR_HANDLE z=0;
    if(!GetFileSizeEx(fd,&fileSize)||!getall(fd,&h,sizeof(h))||(h.magic!=FrameXpress&&h.magic!=FrameZlib)||h.w!=(DWORD)f->w||h.h!=(DWORD)f->h||!h.size||h.size>size+65536||fileSize.QuadPart!=sizeof(h)+h.size)goto done;
    blob=malloc(h.size);pixels=malloc(size);if(!blob||!pixels||!getall(fd,blob,h.size))goto done;
    if(h.magic==FrameZlib){
        ZlibExpand expand=(ZlibExpand)host_zlib("uncompress");
        DWORD length=size;
        if(!expand||expand(pixels,&length,blob,h.size)!=0||length!=size)goto done;
    }else{
        SIZE_T length;
        if(wine_runtime()||!CreateDecompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,0,&z)
           ||!Decompress(z,blob,h.size,pixels,size,&length)||length!=size)goto done;
    }
    BYTE sum[32];if(!sha(pixels,size,sum)||memcmp(sum,h.digest,32))goto done;
    HWND window=PTR(PTR(bp,-0x1c),8);
    CALLPTR(0x9d011c,BmpCreate)(bitmap,window,f->w,f->h,32,1,0);
    HBITMAP bm=PTR(bitmap,0x10);
    BITMAPINFO info;bitmap_info(&info,f->w,f->h);HDC dc=GetDC(window);
    if(bm&&dc)ok=SetDIBits(dc,bm,0,f->h,pixels,&info,DIB_RGB_COLORS)==f->h;
    if(dc)ReleaseDC(window,dc);
    if(!ok&&bm)CALLPTR(0x9cfa00,DestroyBitmap)(bitmap);
done:
    if(z)CloseDecompressor(z);free(blob);free(pixels);CloseHandle(fd);return ok;
}
static __thread void* live_slots[128];
static __thread unsigned live_count;
static __thread void* slot_base;
EXPORT void __cdecl NewPlayback(void){live_count=0;}
static void track_slot(void* bp,void* bitmap,int w,int h){
    if(slot_base!=PTR(bp,-0x78)){slot_base=PTR(bp,-0x78);live_count=0;}
    if(!PTR(bitmap,0x10))return;
    unsigned budget=64u*1024u*1024u;
    unsigned max=budget/((unsigned)w*h*4);if(max<2)max=2;if(max>128)max=128;
    for(unsigned i=0;i<live_count;i++)if(live_slots[i]==bitmap)return;
    if(live_count>=max){
        trace("evict",current.frame,(int)live_count);CALLPTR(0x9cfa00,DestroyBitmap)(live_slots[0]);
        memmove(live_slots,live_slots+1,(live_count-1)*sizeof(void*));live_count--;
    }
    live_slots[live_count++]=bitmap;
}
static BOOL representative_frame(const wchar_t* dir,const BYTE scene_hash[32],const BYTE prefs[32],int frame,DWORD* representative){
    static __thread wchar_t previous[72];
    static __thread DWORD frames[6000],count;
    wchar_t name[72],path[MAX_PATH];
    if(frame<0||!worker_map_name(scene_hash,prefs,name))return FALSE;
    if(wcscmp(previous,name)){
        if(swprintf(path,MAX_PATH,L"%ls\\%ls",dir,name)<0)return FALSE;
        HANDLE file=CreateFileW(path,GENERIC_READ,FILE_SHARE_READ|FILE_SHARE_DELETE,NULL,OPEN_EXISTING,0,NULL);
        if(file==INVALID_HANDLE_VALUE)return FALSE;
        DWORD size=GetFileSize(file,NULL);BYTE stored[32],actual[32];
        BOOL ok=size>32&&size<=32+sizeof(frames)&&(size-32)%4==0&&getall(file,stored,32)&&getall(file,frames,size-32)
              &&sha(frames,size-32,actual)&&!memcmp(stored,actual,32);
        CloseHandle(file);count=0;previous[0]=0;
        if(!ok)return FALSE;
        count=(size-32)/4;wcscpy(previous,name);
    }
    if((unsigned)frame>=count||frames[frame]>(unsigned)frame)return FALSE;
    *representative=frames[frame];return TRUE;
}
EXPORT int __cdecl BeginFrame(void* bp,void* bitmap){
    Frame* f=&current;f->probing=FALSE;f->valid=FALSE;f->missing=FALSE;f->file[0]=0;f->count=0;int frame=V(bp,-0x14);
    int slot=(int)(((intptr_t)bitmap-(intptr_t)PTR(bp,-0x78))/40);
    void* doc=PTR(bp,8);
    wchar_t path[MAX_PATH],dir[MAX_PATH];if(V(doc,0x20)>0||!native_path((char*)doc+16,path)||!isext(path,L".sxf")||!cache_dir(dir))return 0;
    f->w=V(bp,-0x2c);f->h=V(bp,-0x28);f->frame=frame;
    if(f->w<=0||f->h<=0||(uint64_t)f->w*f->h>64*1024*1024 || !hash_init(&f->key))return 0;
    f->valid=TRUE;
    append_file(path);
    // Include the saved scene description and image-affecting drawing preferences.
    // Live sheet changes bypass cache. File content, not last-write time, identifies drawings.
    add(&f->key,path,(DWORD)(wcslen(path)*sizeof(wchar_t)));
    wchar_t sceneInfo[MAX_PATH];wcscpy(sceneInfo,path);wcscpy(wcsrchr(sceneInfo,L'.'),L".mn6");
    if(GetFileAttributesW(sceneInfo)!=INVALID_FILE_ATTRIBUTES)append_file(sceneInfo);
    wchar_t appdata[MAX_PATH],pref[MAX_PATH];
    DWORD appn=GetEnvironmentVariableW(L"APPDATA",appdata,MAX_PATH);
    if(appn && appn<MAX_PATH){
        const wchar_t* names[]={L"AppColor",L"AppVecSetting"};
        for(int i=0;i<2;i++){
            if(swprintf(pref,MAX_PATH,L"%ls\\CELSYS\\RETAS STUDIO\\Stylos\\2.0\\Preference\\Data\\%ls\\pref.pff",appdata,names[i])>0 && GetFileAttributesW(pref)!=INVALID_FILE_ATTRIBUTES)append_file(pref);
        }
    }
    // The settings object has one alignment gap after the 16-bit layer selector.
    add(&f->key,(char*)bp-0x100+8,0x2a);add(&f->key,(char*)bp-0x100+0x34,0x20);
    const int args[]={(unsigned short)V(bp,-0xa0),V(bp,-0xc4),V(bp,-0xd8),V(bp,-0xa8),V(bp,-0x50),V(bp,-0x4c),f->w,f->h,4};
    add(&f->key,args,sizeof(args));
    // Let the native exposure resolver enumerate layers, holds and filenames in paint order.
    // This dry pass skips cel creation and pixels, keeping the document on its owner thread.
    f->probing=TRUE;
    void* obj=((Probe)0x4f5bf0)(doc,args[0],frame,args[1],args[2],args[3],1,1,0);
    f->probing=FALSE;
    if(obj)((Dispose)(*(void***)obj)[0])(obj,1);else f->valid=FALSE;
    BYTE key[32];if(!finish(&f->key,key)||!f->valid||f->count<2)return 0;
    wchar_t hex[65];for(int i=0;i<32;i++)swprintf(hex+i*2,3,L"%02x",key[i]);
    if(swprintf(f->file,MAX_PATH,L"%ls\\%ls.frm",dir,hex)<0){f->valid=FALSE;return 0;}
    if(!f->missing && load_frame(bp,bitmap)){track_slot(bp,bitmap,f->w,f->h);trace("hit",frame,slot);return 1;}
    wchar_t name[72],original[MAX_PATH];
    {
        BYTE prefs[32],inputs[MaxFrameInputs][32];double scale;
        memcpy(&scale,(char*)bp-0xf8,sizeof(scale));
        for(unsigned i=0;i<f->count;i++)memcpy(inputs[i],f->deps[i].id,32);
        DWORD representative;
        if(worker_settings_key(scale,(DWORD)V(bp,-0xe0),*(WORD*)((char*)bp-0xa0),V(bp,-0xc4),V(bp,-0xd8),V(bp,-0xa8),
                               V(bp,-0x50),V(bp,-0x4c),prefs)&&representative_frame(dir,inputs[0],prefs,frame,&representative)
           &&worker_frame_key(representative,f->w,f->h,prefs,inputs,f->count,key)){
            worker_frame_name(key,name);wcscpy(original,f->file);
            BOOL hit=swprintf(f->file,MAX_PATH,L"%ls\\%ls",dir,name)>=0&&load_frame(bp,bitmap);
            wcscpy(f->file,original);
            if(hit){track_slot(bp,bitmap,f->w,f->h);trace("worker-hit",frame,slot);return 1;}
        }
    }
    trace("miss",frame,slot);return 0;
}
typedef struct {wchar_t path[MAX_PATH];int w,h,frame;DWORD size;BYTE* pixels;} WriteTask;
static volatile LONG queued;
static void publish(const WriteTask* task){
    COMPRESSOR_HANDLE compressor=NULL;BYTE* packed=NULL;SIZE_T cap=0,n=0;
    Header h={FrameXpress,task->w,task->h,0,{0}};
    wchar_t root[MAX_PATH];ULARGE_INTEGER freeBytes;
    if(!cache_dir(root)||_wcsnicmp(root,task->path,wcslen(root))||task->path[wcslen(root)]!=L'\\'||!GetDiskFreeSpaceExW(root,&freeBytes,0,0)||freeBytes.QuadPart<128ULL*1024*1024+task->size)return;
    if(!sha(task->pixels,task->size,h.digest))goto done;
    if(wine_runtime()){
        ZlibCompress compress=(ZlibCompress)host_zlib("compress2");
        DWORD length=task->size+65536;
        if(!compress||!(packed=malloc(length))||compress(packed,&length,task->pixels,task->size,1)!=0)goto done;
        h.magic=FrameZlib;n=length;
    }else{
        if(!CreateCompressor(COMPRESS_ALGORITHM_XPRESS_HUFF,0,&compressor))goto done;
        Compress(compressor,task->pixels,task->size,NULL,0,&cap);
        if(!cap||cap>task->size+65536||!(packed=malloc(cap)))goto done;
        if(!Compress(compressor,task->pixels,task->size,packed,cap,&n))goto done;
    }
    h.size=(DWORD)n;
    if(store_cache_file(root,task->path,&h,sizeof(h),packed,(DWORD)n))trace("stored",task->frame,(int)n);
done:
    if(compressor)CloseCompressor(compressor);free(packed);
}
static VOID CALLBACK write_later(PTP_CALLBACK_INSTANCE ctx,void* ptr){
    WriteTask* task=ptr;publish(task);free(task->pixels);free(task);InterlockedDecrement(&queued);
}
EXPORT void __cdecl StoreFrame(void* bp,void* bitmap){
    Frame* f=&current;if(!f->valid||f->missing||!f->file[0]||!unchanged())return;
    track_slot(bp,bitmap,f->w,f->h);
    if(InterlockedIncrement(&queued)>2){InterlockedDecrement(&queued);return;}
    WriteTask* task=NULL;BITMAP bm;HBITMAP handle=PTR(bitmap,0x10);
    if(!handle||!GetObjectW(handle,sizeof(bm),&bm)||bm.bmWidth!=f->w||bm.bmHeight!=f->h)goto done;
    task=calloc(1,sizeof(*task));if(!task)goto done;
    task->w=f->w;task->h=f->h;task->frame=f->frame;task->size=(DWORD)f->w*f->h*4;
    wcscpy(task->path,f->file);task->pixels=malloc(task->size);if(!task->pixels)goto done;
    BITMAPINFO bi;bitmap_info(&bi,f->w,f->h);
    HDC dc=GetDC(NULL);int rows=GetDIBits(dc,handle,0,f->h,task->pixels,&bi,DIB_RGB_COLORS);ReleaseDC(NULL,dc);
    if(rows==f->h){
        if(TrySubmitThreadpoolCallback(write_later,task,NULL)){f->valid=FALSE;return;}
    }
done:
    if(task){free(task->pixels);free(task);}InterlockedDecrement(&queued);f->valid=FALSE;
}
typedef void (__cdecl *Raster)(const RECT*,BYTE*,const BYTE*,int,int,DWORD);
#define FAST_RASTER ((Raster)0x9cee80)
typedef struct { RECT rect;BYTE* dst;const BYTE* src;int ds,ss;DWORD color;HANDLE done; } Strip;
static VOID CALLBACK render_strip(PTP_CALLBACK_INSTANCE context,void* arg){
    Strip* s=arg;FAST_RASTER(&s->rect,s->dst,s->src,s->ds,s->ss,s->color);
    SetEventWhenCallbackReturns(context,s->done);
}
EXPORT void __cdecl ParallelRaster(const RECT* r,BYTE* dst,const BYTE* src,int ds,int ss,DWORD color){
    int h=r->bottom-r->top,w=r->right-r->left;
    if(h<16||w<=0||(int64_t)w*h<65536||ds<w*4||ss<w||((uintptr_t)dst<(uintptr_t)src+(size_t)ss*(h-1)+w && (uintptr_t)src<(uintptr_t)dst+(size_t)ds*(h-1)+4u*w)){
        FAST_RASTER(r,dst,src,ds,ss,color);return;
    }
    int half=h/2;
    Strip child={{0,0,w,h-half},dst+(size_t)ds*half,src+(size_t)ss*half,ds,ss,color,NULL};
    child.done=CreateEventW(NULL,FALSE,FALSE,NULL);
    if(!child.done||!TrySubmitThreadpoolCallback(render_strip,&child,NULL)){
        if(child.done)CloseHandle(child.done);FAST_RASTER(r,dst,src,ds,ss,color);return;
    }
    RECT first={0,0,w,half};FAST_RASTER(&first,dst,src,ds,ss,color);
    WaitForSingleObject(child.done,INFINITE);CloseHandle(child.done);
    static LONG counted;if(InterlockedCompareExchange(&counted,1,0)==0)trace("parallel-mask",w,h);
}
