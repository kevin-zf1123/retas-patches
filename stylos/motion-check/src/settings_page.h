// Dialog controls owned by the motion-check plugin.
#include <shlobj.h>
#include "settings_ids.h"
#include "render_settings.h"

static void motion_space(HWND w){
    WCHAR path[MAX_PATH],label[196];ULARGE_INTEGER available;
    BOOL custom=IsDlgButtonChecked(w,MOTION_CUSTOM_DIR)==BST_CHECKED;
    EnableWindow(GetDlgItem(w,MOTION_BROWSE),custom);
    EnableWindow(GetDlgItem(w,MOTION_PATH),custom);
    GetDlgItemTextW(w,MOTION_PATH,path,MAX_PATH);
    if(!custom){
        DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",path,MAX_PATH);
        if(!n||n>=MAX_PATH)path[0]=0;
    }
    if(path[0]&&GetDiskFreeSpaceExW(path,&available,NULL,NULL))
        swprintf(label,196,L"可用空间：%.1f GiB（缓存子目录 StylosMotionRender）",available.QuadPart/1073741824.0);
    else wcscpy(label,L"无法读取该文件夹的可用空间。");
    SetDlgItemTextW(w,MOTION_FREE,label);
}
static INT_PTR CALLBACK motion_page(HWND w,UINT msg,WPARAM wp,LPARAM lp){
    if(msg==WM_INITDIALOG){
        RenderSettings s=*(const RenderSettings*)lp;
        SetDlgItemInt(w,MOTION_THREADS,s.threads,FALSE);SetDlgItemInt(w,MOTION_IDLE,s.idle_seconds,FALSE);
        SetDlgItemInt(w,MOTION_MEMORY,s.memory_mib,FALSE);SetDlgItemInt(w,MOTION_DISK,s.disk_mib,FALSE);
        CheckRadioButton(w,MOTION_DEFAULT_DIR,MOTION_CUSTOM_DIR,s.directory[0]?MOTION_CUSTOM_DIR:MOTION_DEFAULT_DIR);
        WCHAR path[MAX_PATH]={0};GetEnvironmentVariableW(L"LOCALAPPDATA",path,MAX_PATH);
        SendDlgItemMessageW(w,MOTION_PATH,EM_SETLIMITTEXT,MAX_PATH-121,0);
        SetDlgItemTextW(w,MOTION_PATH,s.directory[0]?s.directory:path);
        MEMORYSTATUSEX mem={sizeof(mem)};if(GlobalMemoryStatusEx(&mem)){
            WCHAR buf[128];swprintf(buf,128,L"物理内存：%.1f GiB　可用：%.1f GiB",mem.ullTotalPhys/1073741824.0,mem.ullAvailPhys/1073741824.0);
            SetDlgItemTextW(w,MOTION_PHYSICAL,buf);}
        motion_space(w);return FALSE;
    }
    if(msg!=WM_COMMAND)return FALSE;
    if(LOWORD(wp)==MOTION_DEFAULT_DIR && HIWORD(wp)==BN_CLICKED){
        WCHAR path[MAX_PATH]={0};GetEnvironmentVariableW(L"LOCALAPPDATA",path,MAX_PATH);
        SetDlgItemTextW(w,MOTION_PATH,path);
    }
    if(LOWORD(wp)==MOTION_BROWSE){
        BROWSEINFOW bi={0};bi.hwndOwner=w;bi.lpszTitle=L"选择动检缓存的上级文件夹";
        bi.ulFlags=BIF_RETURNONLYFSDIRS|BIF_NEWDIALOGSTYLE;
        PIDLIST_ABSOLUTE id=SHBrowseForFolderW(&bi);
        if(id){WCHAR path[MAX_PATH];if(SHGetPathFromIDListW(id,path)){
            if(wcslen(path)>=MAX_PATH-120)MessageBoxW(w,L"缓存路径太长。请选择较短的文件夹路径。",L"动检预渲染",MB_ICONINFORMATION);
            else SetDlgItemTextW(w,MOTION_PATH,path);
        }CoTaskMemFree(id);}
    }
    if(LOWORD(wp)==MOTION_DEFAULT_DIR||LOWORD(wp)==MOTION_CUSTOM_DIR||LOWORD(wp)==MOTION_BROWSE||
       (LOWORD(wp)==MOTION_PATH&&HIWORD(wp)==EN_CHANGE))motion_space(w);
    return FALSE;
}
static BOOL read_motion_page(HWND w,const RenderSettings* original,RenderSettings* draft){
    RenderSettings s={0};s.version=1;BOOL ok=TRUE,number=FALSE;
    const int ids[]={MOTION_THREADS,MOTION_IDLE,MOTION_MEMORY,MOTION_DISK};
    DWORD* const values[]={&s.threads,&s.idle_seconds,&s.memory_mib,&s.disk_mib};
    for(int i=0;i<4;++i){*values[i]=GetDlgItemInt(w,ids[i],&number,FALSE);if(!number)ok=FALSE;}
    if(IsDlgButtonChecked(w,MOTION_CUSTOM_DIR)==BST_CHECKED){
        GetDlgItemTextW(w,MOTION_PATH,s.directory,MAX_PATH);
        ok=ok&&s.directory[0]&&
           ((s.directory[0]&&s.directory[1]==L':'&&(s.directory[2]==L'\\'||s.directory[2]==L'/'))||
            (s.directory[0]==L'\\'&&s.directory[1]==L'\\'));
    }
    if(!ok||!valid_render_settings(&s)){
        MessageBoxW(w,L"线程数须为 0–64，空闲时间 1–3600 秒，内存 128–65536 MiB，磁盘 128–1048576 MiB。请选择已有文件夹。",L"动检预渲染",MB_OK|MB_ICONINFORMATION);return FALSE;}
    *draft=s;
    if(!memcmp(&s,original,sizeof(s)))return TRUE;
    WCHAR dir[MAX_PATH],probe[MAX_PATH];
    if(!ensure_render_cache(&s,dir)||!GetTempFileNameW(dir,L"rtp",0,probe)){
        MessageBoxW(w,L"无法在所选位置创建或写入 StylosMotionRender 文件夹。",L"动检预渲染",MB_OK|MB_ICONINFORMATION);return FALSE;}
    DeleteFileW(probe);
    return TRUE;
}
