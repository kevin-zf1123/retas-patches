static SRWLOCK sceneLock=SRWLOCK_INIT;
static wchar_t scene[MAX_PATH];
static volatile LONG revision,started;
static DWORD idle_ms(void){DWORD owner=0;GetWindowThreadProcessId(GetForegroundWindow(),&owner);if(owner!=GetCurrentProcessId())return MAXDWORD;LASTINPUTINFO i={sizeof(i)};return GetLastInputInfo(&i)?GetTickCount()-i.dwTime:0;}
static HANDLE start_renderer(const wchar_t* sheet){
    wchar_t exe[MAX_PATH],cwd[MAX_PATH],cache[MAX_PATH],prefs[MAX_PATH];
    DWORD n=GetModuleFileNameW(NULL,exe,MAX_PATH);
    if(!n||n>=MAX_PATH||!cache_dir(cache))return NULL;
    wchar_t* leaf=wcsrchr(exe,L'\\');
    if(!leaf||leaf-exe>=MAX_PATH-20)return NULL;
    *leaf=0;wcscpy(cwd,exe);wcscpy(leaf,L"\\StylosRender64.exe");
    n=GetEnvironmentVariableW(L"APPDATA",prefs,MAX_PATH);
    const wchar_t suffix[]=L"\\CELSYS\\RETAS STUDIO\\Stylos\\2.0\\Preference";
    if(!n||n+sizeof(suffix)/sizeof(*suffix)>MAX_PATH)return NULL;
    wcscat(prefs,suffix);
    wchar_t cmd[4*MAX_PATH+64];
    if(swprintf(cmd,sizeof(cmd)/sizeof(*cmd),L"\"%ls\" \"%ls\" \"%ls\" \"%ls\" %lu",
                exe,sheet,cache,prefs,(unsigned long)GetCurrentProcessId())<0)return NULL;
    STARTUPINFOW si={sizeof(si)};PROCESS_INFORMATION pi={0};
    if(!CreateProcessW(exe,cmd,NULL,NULL,FALSE,CREATE_NO_WINDOW|BELOW_NORMAL_PRIORITY_CLASS,
                       NULL,cwd,&si,&pi))return NULL;
    CloseHandle(pi.hThread);trace("start-worker",0,(int)pi.dwProcessId);return pi.hProcess;
}
typedef struct {
    HANDLE directory;
    OVERLAPPED pending;
    DWORD changes[4096];
} ProjectWatch;
static void close_watch(ProjectWatch* watch){
    if(watch->directory!=INVALID_HANDLE_VALUE){
        DWORD ignored;
        CancelIo(watch->directory);
        GetOverlappedResult(watch->directory,&watch->pending,&ignored,TRUE);
        CloseHandle(watch->directory);
        watch->directory=INVALID_HANDLE_VALUE;
    }
}
static void arm_watch(ProjectWatch* watch){
    HANDLE event=watch->pending.hEvent;memset(&watch->pending,0,sizeof(watch->pending));watch->pending.hEvent=event;
    ResetEvent(event);
    if(!ReadDirectoryChangesW(watch->directory,watch->changes,sizeof(watch->changes),TRUE,
       FILE_NOTIFY_CHANGE_FILE_NAME|FILE_NOTIFY_CHANGE_DIR_NAME|FILE_NOTIFY_CHANGE_SIZE|FILE_NOTIFY_CHANGE_LAST_WRITE,
       NULL,&watch->pending,NULL)){
        CloseHandle(watch->directory);watch->directory=INVALID_HANDLE_VALUE;
    }
}
static BOOL project_change(ProjectWatch* watch){
    if(watch->directory==INVALID_HANDLE_VALUE||WaitForSingleObject(watch->pending.hEvent,0)!=WAIT_OBJECT_0)return FALSE;
    DWORD bytes=0;
    BOOL changed=!GetOverlappedResult(watch->directory,&watch->pending,&bytes,FALSE)||!bytes;
    for(DWORD off=0;!changed&&off<bytes;){
        FILE_NOTIFY_INFORMATION* entry=(FILE_NOTIFY_INFORMATION*)((BYTE*)watch->changes+off);
        // A custom cache can live under the scene. Its files never invalidate the scene.
        const WCHAR* part=entry->FileName;const WCHAR* end=part+entry->FileNameLength/sizeof(WCHAR);
        BOOL cache=FALSE;
        while(part<end){
            const WCHAR* next=part;while(next<end&&*next!=L'\\')++next;
            if(next-part==sizeof(L"StylosMotionRender")/sizeof(WCHAR)-1&&!_wcsnicmp(part,L"StylosMotionRender",next-part)){cache=TRUE;break;}
            part=next<end?next+1:end;
        }
        changed=!cache;
        if(!entry->NextEntryOffset)break;off+=entry->NextEntryOffset;
    }
    arm_watch(watch);return changed;
}
static unsigned __stdcall monitor(void* ignored){
    wchar_t path[MAX_PATH]={0},folder[MAX_PATH];LONG seen=0;
    HANDLE child=NULL;ProjectWatch watch={0};
    watch.directory=INVALID_HANDLE_VALUE;watch.pending.hEvent=CreateEventW(NULL,TRUE,FALSE,NULL);
    ULONGLONG since=0,lastPrune=0,launched=0;BOOL pending=FALSE,cancelling=FALSE;int retry=0;
    RenderSettings config=render_settings();
    wchar_t pref[MAX_PATH];DWORD app=GetEnvironmentVariableW(L"APPDATA",pref,MAX_PATH);
    const wchar_t tail[]=L"\\CELSYS\\RETAS STUDIO\\Stylos\\2.0\\Preference\\Data\\MotionCheckDlg\\pref.pff";
    BOOL monitorPrefs=app>0&&app+sizeof(tail)/sizeof(*tail)<=MAX_PATH;
    if(monitorPrefs)wcscat(pref,tail);
    FILETIME previousPref={0};
    for(;;){
        Sleep(250);
        const ULONGLONG now=GetTickCount64();
        LONG gen=InterlockedCompareExchange(&revision,0,0);BOOL changed=FALSE;
        RenderSettings next=render_settings();
        if(memcmp(&next,&config,sizeof(config))){config=next;changed=TRUE;lastPrune=0;}
        if(monitorPrefs){
            WIN32_FILE_ATTRIBUTE_DATA attr={0};
            GetFileAttributesExW(pref,GetFileExInfoStandard,&attr);
            if(CompareFileTime(&attr.ftLastWriteTime,&previousPref)){previousPref=attr.ftLastWriteTime;changed=TRUE;}
        }
        if(gen!=seen){
            AcquireSRWLockShared(&sceneLock);wcscpy(path,scene);ReleaseSRWLockShared(&sceneLock);
            seen=gen;wcscpy(folder,path);wchar_t* slash=wcsrchr(folder,L'\\');if(slash)*slash=0;
            close_watch(&watch);
            if(watch.pending.hEvent){
                watch.directory=CreateFileW(folder,FILE_LIST_DIRECTORY,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,NULL,
                                            OPEN_EXISTING,FILE_FLAG_BACKUP_SEMANTICS|FILE_FLAG_OVERLAPPED,NULL);
                if(watch.directory!=INVALID_HANDLE_VALUE)arm_watch(&watch);
            }
            changed=TRUE;
        }
        if(project_change(&watch))changed=TRUE;
        if(changed){pending=TRUE;since=now;retry=0;}
        DWORD idle=idle_ms();
        if(child){
            if(WaitForSingleObject(child,0)==WAIT_OBJECT_0){
                DWORD code=1;GetExitCodeProcess(child,&code);CloseHandle(child);child=NULL;
                trace("worker-exit",0,(int)code);
                if(code&&!cancelling&&retry<2){++retry;pending=TRUE;since=now;}
            }else if(!cancelling&&(changed||idle<500||now-launched>900000)){
                // Only our single headless process owns these rendering threads.
                cancelling=TerminateProcess(child,23);
                pending=changed||idle<500||retry<2;since=now;
                if(!changed&&idle>=500)++retry;
            }
        }
        if(!child&&pending&&idle>=config.idle_seconds*1000&&now>=since+config.idle_seconds*1000){
            pending=FALSE;cancelling=FALSE;launched=now;child=start_renderer(path);
            if(!child&&retry<2){++retry;pending=TRUE;since=now;}
        }
        if(now-lastPrune>30000){
            wchar_t dir[MAX_PATH];
            if(cache_dir(dir)){
                HANDLE lock=cache_lock();
                if(lock){cache_trim(dir,(ULONGLONG)config.disk_mib<<20,0);ReleaseMutex(lock);CloseHandle(lock);}
            }
            lastPrune=now;
        }
    }
}
EXPORT void __cdecl OpenSheet(void* doc){
    wchar_t path[MAX_PATH];if(!native_path((char*)doc+16,path)||!isext(path,L".sxf"))return;
    AcquireSRWLockExclusive(&sceneLock);wcscpy(scene,path);InterlockedIncrement(&revision);ReleaseSRWLockExclusive(&sceneLock);
    if(InterlockedCompareExchange(&started,1,0)==0){
        uintptr_t t=_beginthreadex(0,0,monitor,0,0,0);
        if(t)CloseHandle((HANDLE)t);else InterlockedExchange(&started,0);
    }
}
EXPORT int __cdecl FrameDisplayed(void* bp){return 0;}
