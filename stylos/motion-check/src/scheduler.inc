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
static HANDLE spawn_lane(const wchar_t* sheet,int id,HDESK desk,const wchar_t* desktop,HANDLE job){
    wchar_t exe[MAX_PATH],cmd[2*MAX_PATH+12];
    if(!desk||!GetModuleFileNameW(0,exe,MAX_PATH))return NULL;
    wchar_t* last=wcsrchr(exe,L'\\');if(!last||last-exe>MAX_PATH-22)return NULL;
    wcscpy(last+1,id?L"StylosRenderB.exe":L"StylosRenderA.exe");
    if(swprintf(cmd,sizeof(cmd)/sizeof(*cmd),L"\"%ls\" \"%ls\"",exe,sheet)<0)return NULL;
    STARTUPINFOW si={0};si.cb=sizeof(si);si.lpDesktop=(PWSTR)desktop;
    wchar_t cwd[MAX_PATH];wcscpy(cwd,exe);*wcsrchr(cwd,L'\\')=0;
    PROCESS_INFORMATION pi={0};
    if(!CreateProcessW(exe,cmd,NULL,NULL,FALSE,CREATE_SUSPENDED|BELOW_NORMAL_PRIORITY_CLASS,NULL,cwd,&si,&pi))return NULL;
    if(!AssignProcessToJobObject(job,pi.hProcess)||ResumeThread(pi.hThread)==(DWORD)-1){
        TerminateProcess(pi.hProcess,22);CloseHandle(pi.hProcess);pi.hProcess=NULL;
    }
    CloseHandle(pi.hThread);return pi.hProcess;
}
static DWORD idle_ms(void){DWORD owner=0;GetWindowThreadProcessId(GetForegroundWindow(),&owner);if(owner!=GetCurrentProcessId())return 60000;LASTINPUTINFO i={sizeof(i)};return GetLastInputInfo(&i)?GetTickCount()-i.dwTime:0;}
static void stop_lane(HANDLE* p){
    if(!*p)return;if(WaitForSingleObject(*p,0)==WAIT_TIMEOUT)TerminateProcess(*p,23);
    WaitForSingleObject(*p,3000);CloseHandle(*p);*p=NULL;
}
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
    HDESK desk=CreateDesktopW(desktop,NULL,NULL,0,GENERIC_ALL,NULL);HANDLE job=CreateJobObjectW(NULL,NULL);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION settings={0};settings.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if(!desk||!job||!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&settings,sizeof(settings)))return 0;
    HANDLE children[2]={0},watch=INVALID_HANDLE_VALUE;LONG seen=0;BOOL pending=FALSE;
    ULONGLONG since=0,launched=0,lastPrune=0;int retry=0;
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
        if(children[0]||children[1]){
            if(changed||idle<500||now-launched>900000){
                stop_lane(children);stop_lane(children+1);pending=TRUE;since=now;
            }else for(int i=0;i<2;i++)if(children[i]&&WaitForSingleObject(children[i],0)==WAIT_OBJECT_0){
                DWORD code;GetExitCodeProcess(children[i],&code);CloseHandle(children[i]);children[i]=NULL;
                trace("worker-exit",i,code);if(code&&retry++<2){pending=TRUE;since=now+2000;}
            }
        }
        if(pending&&path[0]&&!children[0]&&!children[1]&&idle>=1800&&now>=since+1000){
            pending=FALSE;launched=now;children[0]=spawn_lane(path,0,desk,desktop,job);children[1]=spawn_lane(path,1,desk,desktop,job);
            trace("launch",!!children[0],!!children[1]);if((!children[0]||!children[1])&&retry++<2){pending=TRUE;since=now+2000;}
        }
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
