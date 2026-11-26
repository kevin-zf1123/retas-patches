/* Windows integration fixture. The parent runs the same monitor as the DLL. */
#include "src/engine.c"

int wmain(int argc,wchar_t** argv){
    if(argc!=2)return 2;
    if(!wcscmp(argv[1],L"--helper")){Sleep(60000);return 0;}
    if(lane()<0){
        if(wcslen(argv[1])>=MAX_PATH)return 2;
        wcscpy(scene,argv[1]);InterlockedIncrement(&revision);
        return (int)monitor(NULL);
    }
    const char* scenario=getenv("STYLOS_QA_CASE");
    if(!scenario)return 2;
    if(!strcmp(scenario,"fail") && lane()==0)return 21;
    if(!strcmp(scenario,"timeout") && lane()==0 && GetFileAttributesW(L"allow-timeout-retry")==INVALID_FILE_ATTRIBUTES){
        Sleep(90000);return 5;
    }
    if(!strcmp(scenario,"cancel") && lane()==0 && GetFileAttributesW(L"allow-second")==INVALID_FILE_ATTRIBUTES){
        wchar_t exe[MAX_PATH],cmd[]=L"StylosRenderA.exe --helper";
        STARTUPINFOW si={0};si.cb=sizeof(si);PROCESS_INFORMATION pi={0};
        GetModuleFileNameW(NULL,exe,MAX_PATH);
        if(!CreateProcessW(exe,cmd,NULL,NULL,FALSE,0,NULL,NULL,&si,&pi))return 3;
        trace("test-helper",0,(int)pi.dwProcessId);
        CloseHandle(pi.hThread);CloseHandle(pi.hProcess);Sleep(60000);return 4;
    }
    if(strcmp(scenario,"fast"))Sleep(550);
    signal_render_ready();
    trace("test-render",lane(),0);
    if(strcmp(scenario,"fast"))Sleep(lane()==0?1800:600);
    trace("test-done",lane(),0);
    return 0;
}
