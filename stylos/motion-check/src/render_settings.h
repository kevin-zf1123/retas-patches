#pragma once
#include <windows.h>
#include <wchar.h>
#include <string.h>
typedef struct {
    DWORD version,threads,idle_seconds,memory_mib,disk_mib;
    WCHAR directory[MAX_PATH];  // empty selects LOCALAPPDATA
} RenderSettings;
#define MOTION_SETTINGS_KEY L"Software\\CELSYS\\RETAS STUDIO\\Stylos\\MotionPreRender"
static DWORD render_cpus(void){SYSTEM_INFO si;GetSystemInfo(&si);return si.dwNumberOfProcessors?si.dwNumberOfProcessors:1;}
static BOOL valid_render_settings(const RenderSettings* s){
    return s->version==1&&s->threads<=64&&s->idle_seconds>=1&&s->idle_seconds<=3600&&
           s->memory_mib>=128&&s->memory_mib<=65536&&s->disk_mib>=128&&s->disk_mib<=1048576&&
           s->directory[MAX_PATH-1]==0&&wcslen(s->directory)<MAX_PATH-120;
}
static RenderSettings render_settings(void){
    RenderSettings s={1,0,2,1024,8192,{0}},saved;HKEY k;DWORD n=sizeof(saved),type=0;
    if(RegOpenKeyExW(HKEY_CURRENT_USER,MOTION_SETTINGS_KEY,0,KEY_QUERY_VALUE|KEY_WOW64_32KEY,&k)==ERROR_SUCCESS){
        LONG status=RegQueryValueExW(k,L"Settings",0,&type,(BYTE*)&saved,&n);RegCloseKey(k);
        if(status==ERROR_SUCCESS&&type==REG_BINARY&&n==sizeof(saved)&&valid_render_settings(&saved))return saved;
    }
    return s;
}
static BOOL save_render_settings(const RenderSettings* s){
    HKEY k;
    if(!valid_render_settings(s)||RegCreateKeyExW(HKEY_CURRENT_USER,MOTION_SETTINGS_KEY,0,NULL,0,
      KEY_SET_VALUE|KEY_WOW64_32KEY,NULL,&k,NULL)!=ERROR_SUCCESS)return FALSE;
    LONG status=RegSetValueExW(k,L"Settings",0,REG_BINARY,(const BYTE*)s,sizeof(*s));
    RegCloseKey(k);return status==ERROR_SUCCESS;
}
static BOOL render_cache_path(const RenderSettings* s,WCHAR out[MAX_PATH]){
    if(s->directory[0])wcscpy(out,s->directory);
    else {DWORD n=GetEnvironmentVariableW(L"LOCALAPPDATA",out,MAX_PATH);if(!n||n>=MAX_PATH-120)return FALSE;}
    size_t n=wcslen(out);
    if(n&&out[n-1]!=L'\\'&&out[n-1]!=L'/')wcscat(out,L"\\");
    wcscat(out,L"StylosMotionRender");return TRUE;
}

static BOOL ensure_render_cache(const RenderSettings* s,WCHAR path[MAX_PATH]){
    if(!render_cache_path(s,path))return FALSE;
    if(CreateDirectoryW(path,NULL))return TRUE;
    DWORD attr=GetFileAttributesW(path);
    return attr!=INVALID_FILE_ATTRIBUTES&&(attr&FILE_ATTRIBUTE_DIRECTORY)!=0;
}
