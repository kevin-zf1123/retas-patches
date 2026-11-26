/* Exercise the production scheduler in a disposable Windows installation. */
#include "src/engine.c"
int wmain(int argc,wchar_t** argv){
    if(argc==2){
        if(wcslen(argv[1])>=MAX_PATH)return 2;
        wcscpy(scene,argv[1]);InterlockedIncrement(&revision);return (int)monitor(NULL);
    }
    if(argc!=5)return 2;
    trace("test-start",0,(int)GetCurrentProcessId());
    if(GetEnvironmentVariableW(L"STYLOS_QA_FAIL",NULL,0))return 21;
    Sleep(1800);trace("test-complete",0,(int)GetCurrentProcessId());return 0;
}
