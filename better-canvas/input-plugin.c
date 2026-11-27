#include "CanvasView.h"
#include "plugin-services.h"
const RetasHost *canvasHost;
BOOL FastCanvasNavigationEnabled(void){FastCanvasAPI *f=fast_api();return f && f->enabled();}
void NoteCanvasNavigation(void *view){FastCanvasAPI *f=fast_api();if(f)f->changed(view);}
void DispatchCanvasInput(MSG *,BOOL,BOOL);
static void WINAPI before(HWND w,UINT lo,UINT hi){BeforeCanvasGetMessage(w,lo,hi);}
static void WINAPI message(MSG *msg){
#ifdef RETAS_ROTATION
    DispatchCanvasInput(msg,FALSE,TRUE);
#else
    DispatchCanvasInput(msg,TRUE,FALSE);
#endif
}
BOOL WINAPI RetasPluginInit(const RetasHost *host,RetasPlugin *p){
    if(host->abi!=RETAS_PLUGIN_ABI || p->size<sizeof(*p))return FALSE;
    canvasHost=host;p->before_message=before;p->message=message;return TRUE;
}
