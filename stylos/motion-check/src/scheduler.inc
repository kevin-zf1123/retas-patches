static SRWLOCK sceneLock=SRWLOCK_INIT;
static wchar_t scene[MAX_PATH];
static volatile LONG revision,started;
static HWND wf;
static BOOL CALLBACK find_frame(HWND w,LPARAM unused){
    DWORD pid;wchar_t cls[48];GetWindowThreadProcessId(w,&pid);
    if(pid==GetCurrentProcessId()&&GetClassNameW(w,cls,48)&&!wcscmp(cls,L"DrawFrameWnd")){wf=w;return FALSE;}return TRUE;
}
static unsigned __stdcall start_worker_ui(void* ignored){
    for(int i=0;i<250;i++){
        EnumWindows(find_frame,0);
        if(wf){
            HWND mdi=FindWindowExW(wf,NULL,L"MDIClient",NULL);
            if(mdi&&FindWindowExW(mdi,NULL,L"CScoreWindow",NULL)){
                Sleep(120);PostMessageW(wf,WM_COMMAND,41000,0);return 0;
            }
        }
        Sleep(80);
    }
    ExitProcess(21);return 0;
}
// The native startup helper is extracted to one shared %TEMP% filename.
// Signal at the first render callback, after native startup and sheet opening.
static void ready_name(wchar_t name[64],DWORD pid){
    swprintf(name,64,L"Local\\StylosMotionReady_%lu",(unsigned long)pid);
}
static void signal_render_ready(void){
    static LONG signalled;
    if(InterlockedCompareExchange(&signalled,1,0))return;
    wchar_t name[64];ready_name(name,GetCurrentProcessId());
    HANDLE event=OpenEventW(EVENT_MODIFY_STATE,FALSE,name);
    if(event){SetEvent(event);CloseHandle(event);}
}
typedef struct {
    HANDLE job,children[2],ready;
    int starting;
    ULONGLONG launched,startDeadline;
    BOOL draining;
} RenderBatch;
enum BatchResult { BatchRunning, BatchDone, BatchFailed };
static HANDLE render_job(void){
    HANDLE job=CreateJobObjectW(NULL,NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits={0};
    limits.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(job&&!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&limits,sizeof(limits))){
        CloseHandle(job);return NULL;
    }
    return job;
}
static BOOL spawn_lane(RenderBatch* batch,const wchar_t* sheet,int id,const wchar_t* desktop){
    wchar_t exe[MAX_PATH],cmd[2*MAX_PATH+12];
    DWORD n=GetModuleFileNameW(NULL,exe,MAX_PATH);
    if(!n||n>=MAX_PATH)return FALSE;
    wchar_t* last=wcsrchr(exe,L'\\');if(!last||last-exe>MAX_PATH-22)return FALSE;
    wcscpy(last+1,id?L"StylosRenderB.exe":L"StylosRenderA.exe");
    if(swprintf(cmd,sizeof(cmd)/sizeof(*cmd),L"\"%ls\" \"%ls\"",exe,sheet)<0)return FALSE;
    STARTUPINFOW si={0};si.cb=sizeof(si);si.lpDesktop=(PWSTR)desktop;
    wchar_t cwd[MAX_PATH];wcscpy(cwd,exe);*wcsrchr(cwd,L'\\')=0;
    PROCESS_INFORMATION pi={0};
    if(!CreateProcessW(exe,cmd,NULL,NULL,FALSE,CREATE_SUSPENDED|BELOW_NORMAL_PRIORITY_CLASS,NULL,cwd,&si,&pi))return FALSE;
    wchar_t name[64];ready_name(name,pi.dwProcessId);
    SetLastError(ERROR_SUCCESS);
    HANDLE ready=CreateEventW(NULL,TRUE,FALSE,name);
    BOOL ok=ready && GetLastError()!=ERROR_ALREADY_EXISTS && AssignProcessToJobObject(batch->job,pi.hProcess);
    if(ok){
        trace("start-lane",id,(int)pi.dwProcessId);
        ok=ResumeThread(pi.hThread)!=(DWORD)-1;
    }
    CloseHandle(pi.hThread);
    if(!ok){
        TerminateProcess(pi.hProcess,22);CloseHandle(pi.hProcess);
        if(ready)CloseHandle(ready);
        return FALSE;
    }
    batch->children[id]=pi.hProcess;batch->ready=ready;batch->starting=id;
    batch->startDeadline=GetTickCount64()+60000;
    return TRUE;
}
static enum BatchResult advance_batch(RenderBatch* batch,const wchar_t* sheet,const wchar_t* desktop,ULONGLONG now){
    if(batch->ready){
        int id=batch->starting;
        BOOL ended=WaitForSingleObject(batch->children[id],0)==WAIT_OBJECT_0;
        if(WaitForSingleObject(batch->ready,0)==WAIT_OBJECT_0){
            CloseHandle(batch->ready);batch->ready=NULL;
            trace("render-ready",id,(int)GetProcessId(batch->children[id]));
            if(!id && !spawn_lane(batch,sheet,1,desktop))return BatchFailed;
        }else if(ended||now>=batch->startDeadline){
            DWORD code=0;
            if(ended)GetExitCodeProcess(batch->children[id],&code);
            trace(ended?"startup-exit":"startup-timeout",id,(int)code);
            return BatchFailed;
        }
    }
    for(int id=0;id<2;id++){
        HANDLE child=batch->children[id];
        // An event remains signalled even if a very short sheet has already finished.
        if(!child || (batch->ready && id==batch->starting) || WaitForSingleObject(child,0)!=WAIT_OBJECT_0)continue;
        DWORD code=1;GetExitCodeProcess(child,&code);trace("worker-exit",id,(int)code);
        CloseHandle(child);batch->children[id]=NULL;
        if(code)return BatchFailed;
    }
    return batch->children[0]||batch->children[1]?BatchRunning:BatchDone;
}
static void stop_batch(RenderBatch* batch){
    if(batch->ready){CloseHandle(batch->ready);batch->ready=NULL;}
    for(int id=0;id<2;id++)if(batch->children[id]){
        CloseHandle(batch->children[id]);batch->children[id]=NULL;
    }
    // Include native helper descendants when cancelling a partially started lane.
    TerminateJobObject(batch->job,23);
    batch->draining=TRUE;
}
static void reap_batch(RenderBatch* batch){
    JOBOBJECT_BASIC_ACCOUNTING_INFORMATION info;
    if(QueryInformationJobObject(batch->job,JobObjectBasicAccountingInformation,&info,sizeof(info),NULL) && !info.ActiveProcesses){
        CloseHandle(batch->job);*batch=(RenderBatch){0};
    }
}
static DWORD idle_ms(void){DWORD owner=0;GetWindowThreadProcessId(GetForegroundWindow(),&owner);if(owner!=GetCurrentProcessId())return 60000;LASTINPUTINFO i={sizeof(i)};return GetLastInputInfo(&i)?GetTickCount()-i.dwTime:0;}
typedef struct {WIN32_FIND_DATAW data;} CacheItem;
static int by_age(const void* a,const void* b){return CompareFileTime(&((const CacheItem*)a)->data.ftLastWriteTime,&((const CacheItem*)b)->data.ftLastWriteTime);}
static void prune(void){
    wchar_t dir[MAX_PATH],pattern[MAX_PATH],path[MAX_PATH];if(!cache_dir(dir))return;
    if(swprintf(pattern,MAX_PATH,L"%ls\\*",dir)<0)return;
    WIN32_FIND_DATAW f;HANDLE it=FindFirstFileW(pattern,&f);if(it==INVALID_HANDLE_VALUE)return;
    CacheItem* list=NULL;size_t count=0,cap=0;uint64_t total=0;FILETIME now;GetSystemTimeAsFileTime(&now);
    do{
        if(f.dwFileAttributes&FILE_ATTRIBUTE_DIRECTORY)continue;
        if(isext(f.cFileName,L".tmp")&&CompareFileTime(&now,&f.ftLastWriteTime)>0){
            ULARGE_INTEGER a,b;a.LowPart=now.dwLowDateTime;a.HighPart=now.dwHighDateTime;b.LowPart=f.ftLastWriteTime.dwLowDateTime;b.HighPart=f.ftLastWriteTime.dwHighDateTime;
            if(a.QuadPart-b.QuadPart>3600ULL*10000000&&swprintf(path,MAX_PATH,L"%ls\\%ls",dir,f.cFileName)>0)DeleteFileW(path);
        }
        if(!isext(f.cFileName,L".frm"))continue;
        if(count==cap){size_t next=cap?cap*2:64;CacheItem* resized=realloc(list,next*sizeof(*list));if(!resized)break;list=resized;cap=next;}
        list[count++].data=f;total+=((uint64_t)f.nFileSizeHigh<<32)|f.nFileSizeLow;
    }while(FindNextFileW(it,&f));FindClose(it);
    if(total>512ULL*1024*1024){
        qsort(list,count,sizeof(*list),by_age);
        for(size_t i=0;i<count&&total>448ULL*1024*1024;i++){
            WIN32_FIND_DATAW* item=&list[i].data;
            if(swprintf(path,MAX_PATH,L"%ls\\%ls",dir,item->cFileName)>0&&DeleteFileW(path))total-=((uint64_t)item->nFileSizeHigh<<32)|item->nFileSizeLow;
        }
    }free(list);
}
static unsigned __stdcall monitor(void* ignored){
    wchar_t desktop[64],path[MAX_PATH]={0},folder[MAX_PATH];swprintf(desktop,64,L"StylosRender_%lu",(unsigned long)GetCurrentProcessId());
    HDESK desk=CreateDesktopW(desktop,NULL,NULL,0,GENERIC_ALL,NULL);
    if(!desk)return 0;
    RenderBatch batch={0};
    HANDLE watch=INVALID_HANDLE_VALUE;LONG seen=0;BOOL pending=FALSE;
    ULONGLONG since=0,lastPrune=0;int retry=0;
    for(;;){
        Sleep(250);ULONGLONG now=GetTickCount64();LONG gen=InterlockedCompareExchange(&revision,0,0);BOOL changed=FALSE;
        if(gen!=seen){
            AcquireSRWLockShared(&sceneLock);wcscpy(path,scene);ReleaseSRWLockShared(&sceneLock);
            seen=gen;wcscpy(folder,path);wchar_t* slash=wcsrchr(folder,L'\\');if(slash)*slash=0;
            if(watch!=INVALID_HANDLE_VALUE)FindCloseChangeNotification(watch);
            watch=FindFirstChangeNotificationW(folder,TRUE,FILE_NOTIFY_CHANGE_LAST_WRITE|FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_SIZE);
            changed=TRUE;
        }
        if(watch!=INVALID_HANDLE_VALUE&&WaitForSingleObject(watch,0)==WAIT_OBJECT_0){changed=TRUE;FindNextChangeNotification(watch);}
        if(changed){since=now;pending=TRUE;retry=0;}
        DWORD idle=idle_ms();
        BOOL failed=FALSE;
        if(batch.job && !batch.draining){
            if(changed||idle<500){
                stop_batch(&batch);pending=TRUE;since=now;
            }else{
                enum BatchResult result=now-batch.launched>900000?BatchFailed:advance_batch(&batch,path,desktop,now);
                if(result!=BatchRunning){stop_batch(&batch);failed=result==BatchFailed;}
            }
        }
        if(batch.draining)reap_batch(&batch);
        if(pending&&path[0]&&!batch.job&&idle>=1800&&now>=since+1000){
            pending=FALSE;batch.launched=now;batch.job=render_job();
            if(!batch.job || !spawn_lane(&batch,path,0,desktop)){
                if(batch.job)stop_batch(&batch);
                failed=TRUE;
            }
        }
        if(failed && retry<2){++retry;pending=TRUE;since=now+2000;}
        if(now-lastPrune>30000){prune();lastPrune=now;}
    }
}
EXPORT void __cdecl OpenSheet(void* doc){
    wchar_t path[MAX_PATH];if(!native_path((char*)doc+16,path)||!isext(path,L".sxf"))return;
    if(lane()>=0){
        if(InterlockedCompareExchange(&started,1,0)==0){uintptr_t t=_beginthreadex(0,0,start_worker_ui,0,0,0);if(t)CloseHandle((HANDLE)t);}
        return;
    }
    AcquireSRWLockExclusive(&sceneLock);wcscpy(scene,path);InterlockedIncrement(&revision);ReleaseSRWLockExclusive(&sceneLock);
    if(InterlockedCompareExchange(&started,1,0)==0){
        uintptr_t t=_beginthreadex(0,0,monitor,0,0,0);if(t)CloseHandle((HANDLE)t);else InterlockedExchange(&started,0);
    }
}
EXPORT int __cdecl FrameDisplayed(void* bp){
    if(lane()<0)return 0;if(V(bp,-0x14)>=V(bp,0x10)){trace("complete",V(bp,-0x14),0);ExitProcess(0);}return 1;
}
