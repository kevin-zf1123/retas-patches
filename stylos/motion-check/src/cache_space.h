#pragma once
#include "render_settings.h"
#include <stdlib.h>
typedef struct {WIN32_FIND_DATAW data;BOOL map;} MotionCacheEntry;
static int cache_age(const void* a,const void* b){
    const MotionCacheEntry* left=(const MotionCacheEntry*)a;
    const MotionCacheEntry* right=(const MotionCacheEntry*)b;
    // Keep small exposure maps usable while any frame payloads remain.
    if(left->map!=right->map)return left->map?1:-1;
    return CompareFileTime(&left->data.ftLastWriteTime,&right->data.ftLastWriteTime);
}
static BOOL cache_filename(const WCHAR* name){
    BOOL worker=!wcsncmp(name,L"w-",2);if(worker)name+=2;
    if(wcslen(name)!=68)return FALSE;
    for(int i=0;i<64;i++)if(!(name[i]>=L'0'&&name[i]<=L'9')&&!(name[i]>=L'a'&&name[i]<=L'f'))return FALSE;
    return !_wcsicmp(name+64,L".frm")||(worker&&!_wcsicmp(name+64,L".map"));
}
static BOOL abandoned_cache_temp(const WCHAR* name){
    const WCHAR* ext=wcschr(name,L'.');
    if(!ext||wcslen(ext)<9||ext[4]!=L'.'||ext-name+4>=72)return FALSE;
    WCHAR base[72];size_t len=ext-name+4;memcpy(base,name,len*sizeof(WCHAR));base[len]=0;
    if(!cache_filename(base)||ext[5]<L'0'||ext[5]>L'9')return FALSE;
    WCHAR* end=NULL;DWORD pid=wcstoul(ext+5,&end,10);
    if(*end==L'.'&&end[1]>=L'0'&&end[1]<=L'9')wcstoul(end+1,&end,10);
    if(!pid||_wcsicmp(end,L".tmp"))return FALSE;
    HANDLE process=OpenProcess(SYNCHRONIZE,FALSE,pid);
    if(!process)return GetLastError()==ERROR_INVALID_PARAMETER;
    BOOL gone=WaitForSingleObject(process,0)==WAIT_OBJECT_0;CloseHandle(process);return gone;
}
static BOOL cache_trim(const WCHAR* dir,ULONGLONG limit,ULONGLONG incoming){
    if(incoming>limit)return FALSE;
    WCHAR pattern[MAX_PATH];if(swprintf(pattern,MAX_PATH,L"%ls\\*",dir)<0)return FALSE;
    WIN32_FIND_DATAW f;HANDLE it=FindFirstFileW(pattern,&f);
    if(it==INVALID_HANDLE_VALUE)return GetLastError()==ERROR_FILE_NOT_FOUND;
    MotionCacheEntry* entries=NULL;size_t count=0,capacity=0;ULONGLONG total=0;BOOL ok=TRUE;
    do{
        if(f.dwFileAttributes&(FILE_ATTRIBUTE_DIRECTORY|FILE_ATTRIBUTE_REPARSE_POINT))continue;
        ULONGLONG bytes=((ULONGLONG)f.nFileSizeHigh<<32)|f.nFileSizeLow;
        if(!cache_filename(f.cFileName)){
            if(abandoned_cache_temp(f.cFileName)&&swprintf(pattern,MAX_PATH,L"%ls\\%ls",dir,f.cFileName)>=0&&!DeleteFileW(pattern))total+=bytes;
            continue;
        }
        if(count==capacity){
            size_t next=capacity?capacity*2:64;void* ptr=realloc(entries,next*sizeof(*entries));
            if(!ptr){ok=FALSE;break;}entries=(MotionCacheEntry*)ptr;capacity=next;
        }
        entries[count].map=!_wcsicmp(wcsrchr(f.cFileName,L'.'),L".map");
        entries[count++].data=f;total+=bytes;
    }while(FindNextFileW(it,&f));
    FindClose(it);
    if(ok&&total+incoming>limit){
        qsort(entries,count,sizeof(*entries),cache_age);
        ULONGLONG target=limit-incoming;
        if(target>limit-limit/8)target=limit-limit/8;
        for(size_t i=0;i<count&&total>target;i++){
            WIN32_FIND_DATAW* e=&entries[i].data;
            if(swprintf(pattern,MAX_PATH,L"%ls\\%ls",dir,e->cFileName)>=0&&DeleteFileW(pattern))
                total-=((ULONGLONG)e->nFileSizeHigh<<32)|e->nFileSizeLow;
        }
        ok=total+incoming<=limit;
    }
    free(entries);return ok;
}
static HANDLE cache_lock(void){
    HANDLE lock=CreateMutexW(NULL,FALSE,L"Local\\RETAS.Stylos.MotionCache.Publish");if(!lock)return NULL;
    DWORD wait=WaitForSingleObject(lock,10000);
    if(wait==WAIT_OBJECT_0||wait==WAIT_ABANDONED)return lock;
    CloseHandle(lock);return NULL;
}
static BOOL store_cache_file(const WCHAR* dir,const WCHAR* dest,const void* header,DWORD hs,const void* data,DWORD size){
    WCHAR temp[MAX_PATH];if(swprintf(temp,MAX_PATH,L"%ls.%lu.%lu.tmp",dest,(unsigned long)GetCurrentProcessId(),(unsigned long)GetCurrentThreadId())<0)return FALSE;
    HANDLE lock=cache_lock();if(!lock)return FALSE;
    RenderSettings cfg=render_settings();
    BOOL ok=cache_trim(dir,((ULONGLONG)cfg.disk_mib)<<20,(ULONGLONG)hs+size);
    if(ok){
        HANDLE file=CreateFileW(temp,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,FILE_ATTRIBUTE_NORMAL,NULL);
        ok=file!=INVALID_HANDLE_VALUE;
        if(ok){DWORD written=0;ok=WriteFile(file,header,hs,&written,NULL)&&written==hs&&WriteFile(file,data,size,&written,NULL)&&written==size;CloseHandle(file);}
        if(ok)ok=MoveFileExW(temp,dest,MOVEFILE_REPLACE_EXISTING);
        if(!ok)DeleteFileW(temp);
    }
    ReleaseMutex(lock);CloseHandle(lock);return ok;
}
