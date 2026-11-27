#include "plugin.h"
#include "retas.h"

static HWND WINAPI create_page(HWND dialog,RECT area){
    HWND frame=GetAncestor(dialog,GA_ROOTOWNER);
    UINT msg=RegisterWindowMessageW(RETAS_MESSAGE_V1);
    int zoom=(int)SendMessageW(frame,msg,RETAS_ZOOM_BASIS_POINTS,0);
    int angle=(int)SendMessageW(frame,msg,RETAS_ROTATION_DEGREES,0);
    WCHAR text[128];
    wsprintfW(text,L"画布倍率 %d.%02d%%   旋转 %d°",zoom/100,zoom%100,angle);
    HWND label=CreateWindowW(L"Static",text,WS_CHILD,area.left+8,area.top+8,
        area.right-area.left-16,36,dialog,NULL,NULL,NULL);
    SendMessageW(label,WM_SETFONT,SendDlgItemMessageW(dialog,IDOK,WM_GETFONT,0,0),FALSE);
    return label;
}
BOOL WINAPI RetasPluginInit(const RetasHost *host,RetasPlugin *plugin){
    if(host->abi!=RETAS_PLUGIN_ABI || plugin->size<sizeof(*plugin))return FALSE;
    plugin->create_page=create_page;return TRUE;
}
