/* Native cel view picking. A modal tool window captures clicks on the existing
   canvas. Only display flags are changed while collecting landmarks. */
extern __attribute__((visibility("hidden"))) const unsigned char pick_dialog[],preview_dialog[];
typedef Point *(MEMBER *ViewToPaper)(Byte *,Point *,double,double);
typedef struct { Byte *row; int visible; } SavedVisibility;
typedef struct {
    Byte *app,*doc,*palette,*set,*view,*paper,*rows[2];
    HWND canvas,tool,marks[4];
    HDC dot_dc[2];
    HBITMAP dot_bitmap[2];
    HGDIOBJ old_bitmap[2];
    POINT crop[4];
    DocGetter document;
    SetGetter current_set;
    SavedVisibility *saved;
    int count,opacity[2],enabled,group_opacity;
    int used,phase,corner;
    BOOL closing,capturing,pressed;
    Point points[4];
    POINT client[4],down,display[4],last_click,drag_screen;
    Pose drag_pose[2];
    Point drag_source,drag_probe,drag_down_paper;
    BOOL drag_moved,drag_pending;
    DWORD last_time;
    int layer,drag,step,fit_error;
    BOOL preview,skip_preview,provisional,armed_click;
    Pose staged[2];
    unsigned lengths[2][2][6];
    Pose pose[2];
} PickState;
#define KERNEL(name) ((__typeof__(&name))system_api("kernel32.dll",#name))
#include "control_marker.h"
#define PICK_START (WM_APP+119)
#define PICK_CAPTURE (WM_APP+120)

static int pick_live(PickState *p) {
    if(p->document(p->app,0x43454c57,0)!=p->doc || p->current_set(p->palette)!=p->set ||
       *(Byte **)(p->doc+0xa0)!=p->view || *(Byte **)(p->view+0xac)!=p->paper ||
       *(Byte **)(p->view+0x4b4)!=p->set || *(HWND *)(p->view+4)!=p->canvas ||
       !USER(IsWindow)(p->canvas) || *(int *)(p->set+0x34)!=p->count)return 0;
    Byte **items=*(Byte ***)(p->set+0x1c);
    for(int i=0;i<p->count;++i)if(items[i]!=p->saved[i].row)return 0;
    return same_pair(p->set,p->rows[0],p->rows[1]);
}
static void pick_refresh(PickState *p) {
    ((RowValue)address(Refresh))(p->palette,0);
    USER(UpdateWindow)(p->canvas);
}
static void pick_phase(PickState *p,int phase) {
    if(p->phase==phase)return;
    p->phase=phase;
    for(int i=0;i<p->count;++i)*(int *)(p->saved[i].row+0x50)=p->saved[i].row==p->rows[phase];
    *(int *)(p->rows[phase]+0x58)=100;
    *(int *)(p->set+0x98)=1;*(int *)(p->set+0x60)=100;
    pick_refresh(p);
}
static BOOL same_pose(Pose a,Pose b) {
    return a.scale==b.scale && a.angle==b.angle && a.shift.x==b.shift.x && a.shift.y==b.shift.y;
}
static void pick_restore(PickState *p) {
    if(p->phase<0)return;
    if(p->current_set(p->palette)!=p->set){
        /* A document can close during a modal session. Only restore a set
           still owned by this light-table manager. */
        Byte *manager=*(Byte **)(p->palette+0x260);
        if(!manager)return;
        int sets=*(int *)(manager+0x24),i=0;Byte **list=*(Byte ***)(manager+0x0c);
        while(i<sets && list[i]!=p->set)++i;
        if(i==sets)return;
    }
    int count=*(int *)(p->set+0x34);Byte **items=*(Byte ***)(p->set+0x1c);
    for(int i=0;i<count;++i){
        int j=i;
        if(j>=p->count || p->saved[j].row!=items[i]){
            for(j=0;j<p->count && p->saved[j].row!=items[i];++j){}
        }
        if(j<p->count)*(int *)(items[i]+0x50)=p->saved[j].visible;
        for(int k=0;k<2;++k)if(items[i]==p->rows[k]){
            Byte *row=items[i];
            *(int *)(row+0x58)=p->opacity[k];
            if(p->provisional){
                Pose now=read_pose(row),last=p->staged[k];
                if(now.shift.x==last.shift.x)((RowLength)address(SetX))(row,p->lengths[k][0]);
                if(now.shift.y==last.shift.y)((RowLength)address(SetY))(row,p->lengths[k][1]);
                if(now.scale==last.scale)((RowValue)address(SetScale))(row,nearest(p->pose[k].scale*100));
                if(now.angle==last.angle)((RowValue)address(SetAngle))(row,(int)p->pose[k].angle);
            }
        }
    }
    *(int *)(p->set+0x98)=p->enabled;*(int *)(p->set+0x60)=p->group_opacity;
    pick_refresh(p);
}
static void pick_label(HWND w,PickState *p) {
    const WCHAR *label=p->used==0?L"起始原画：点第一个特征（1/4）":
        p->used==1?L"起始原画：点第二个特征（2/4）":
        p->used==2?L"结束原画：点对应的第一个特征（3/4）":L"结束原画：点对应的第二个特征（4/4）";
    USER(SetDlgItemTextW)(w,300,label);
    USER(EnableWindow)(USER(GetDlgItem)(w,301),p->used>0);
    clear_marks(p);
    if(p->used&1)place_mark(p,0,p->client[p->used-1]);
}
static void position_prompt(HWND w,PickState *p) {
    RECT frame,canvas;
    USER(GetWindowRect)(w,&frame);USER(GetClientRect)(p->canvas,&canvas);
    USER(MapWindowPoints)(p->canvas,NULL,(POINT *)&canvas,2);
    int x=p->corner?canvas.left+4:canvas.right-(frame.right-frame.left)-4;
    int y=p->corner?canvas.bottom-(frame.bottom-frame.top)-4:canvas.top+4;
    if(p->preview) {
        MONITORINFO monitor={sizeof(monitor)};
        HMONITOR screen=USER(MonitorFromWindow)(p->canvas,MONITOR_DEFAULTTONEAREST);
        if(USER(GetMonitorInfoW)(screen,&monitor)){
            int width=frame.right-frame.left,height=frame.bottom-frame.top;
            RECT work=monitor.rcWork;
            if(!p->corner && canvas.right+8+width<=work.right)x=canvas.right+8;
            else if(canvas.left-8-width>=work.left)x=canvas.left-8-width;
            if(x+width>work.right)x=work.right-width;
            if(x<work.left)x=work.left;
            if(y+height>work.bottom)y=work.bottom-height;
            if(y<work.top)y=work.top;
        }
    }
    USER(SetWindowPos)(w,NULL,x,y,0,0,SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOZORDER);
}
static void finish_pick(HWND w,PickState *p,int result) {
    if(p->closing)return;
    p->closing=TRUE;USER(KillTimer)(w,1);USER(KillTimer)(w,2);USER(KillTimer)(w,3);
    if(USER(GetCapture)()==w)USER(ReleaseCapture)();
    clear_marks(p);
    IMPORT(End,__typeof__(&EndDialog))(w,result);
}
static void undo_pick(HWND w,PickState *p) {
    if(!p->used)return;
    --p->used;pick_phase(p,p->used/2);pick_label(w,p);USER(SetFocus)(w);
}
/* ChildWindowFromPointEx includes disabled windows: a modal picker disables
   its owner. Reject sibling viewports, palettes and controls over the paper. */
static int pick_target(HWND w,PickState *p,POINT screen) {
    RECT frame;USER(GetWindowRect)(w,&frame);
    if(USER(PtInRect)(&frame,screen)){
        int ids[]={301,302,IDCANCEL,401,IDOK};
        for(unsigned i=0;i<sizeof(ids)/sizeof(*ids);++i){
            HWND button=USER(GetDlgItem)(w,ids[i]);if(!button)continue;RECT r;USER(GetWindowRect)(button,&r);
            if(USER(PtInRect)(&r,screen) && USER(IsWindowEnabled)(button))return ids[i];
        }
        return 0;
    }
    HWND parent=USER(GetDesktopWindow)();
    while(parent){
        POINT client=screen;USER(ScreenToClient)(parent,&client);
        HWND child=USER(ChildWindowFromPointEx)(parent,client,CWP_SKIPINVISIBLE|CWP_SKIPTRANSPARENT);
        if(parent==p->canvas)return (!child || child==parent || child==p->marks[0] || child==p->marks[1])?-1:0;
        if(!child || child==parent)return 0;
        parent=child;
    }
    return 0;
}
static BOOL paper_point(PickState *p,double x,double y,Point *out,BOOL within_sheet) {
    Point px;
    ((ViewToPaper)address(ViewToPixels))(p->view,&px,x,y);
    double dpi=*(double *)(p->paper+0x88);
    if(!(dpi>0 && dpi<=1e8))return FALSE;
    GetLength get=IMPORT(LengthGet,GetLength);
    double cm=2.54/dpi;
    if(within_sheet && !(px.x>=0 && px.y>=0 && px.x*cm<get(p->paper+0x90,1) &&
                         px.y*cm<get(p->paper+0xa8,1)))return FALSE;
    out->x=px.x*cm-get(p->paper+0xc8,1);out->y=px.y*cm-get(p->paper+0xe0,1);
    return __builtin_fabs(out->x)<=1e8 && __builtin_fabs(out->y)<=1e8;
}
static void canvas_click(HWND w,PickState *p,POINT screen) {
    int target=pick_target(w,p,screen);
    if(!target || target!=pick_target(w,p,p->down))return;
    if(target>0){
        USER(SendMessageW)(w,WM_COMMAND,target,(LPARAM)USER(GetDlgItem)(w,target));return;
    }
    POINT pos=screen;RECT bounds;USER(ScreenToClient)(p->canvas,&pos);USER(GetClientRect)(p->canvas,&bounds);
    if(!USER(PtInRect)(&bounds,pos))return;
    if(p->used&1){
        int dx=pos.x-p->client[p->used-1].x,dy=pos.y-p->client[p->used-1].y;
        if(dx==0 && dy==0){USER(SetDlgItemTextW)(w,300,L"两点重合，请另选第二个特征。");return;}
    }
    Point rel;
    if(!paper_point(p,pos.x,pos.y,&rel,TRUE)){USER(SetDlgItemTextW)(w,300,L"请在当前画纸内点选特征。");return;}
    Pose row=p->pose[p->used/2];rel.x-=row.shift.x;rel.y-=row.shift.y;
    Point source=quotient(rel,polar(row.scale,row.angle));
    if(!(__builtin_fabs(source.x)<=1e8 && __builtin_fabs(source.y)<=1e8)){finish_pick(w,p,0);return;}
    p->points[p->used]=source;p->client[p->used++]=pos;
    if(p->used==4){finish_pick(w,p,IDOK);return;}
    pick_phase(p,p->used/2);pick_label(w,p);USER(SetFocus)(w);
}
#include "alignment_preview.h"
static INT_PTR CALLBACK pick_proc(HWND w,UINT msg,WPARAM wp,LPARAM lp) {
    if(msg==WM_INITDIALOG){
        PickState *p=(PickState *)lp;p->tool=w;USER(SetWindowLongW)(w,DWLP_USER,lp);
        position_prompt(w,p);
        if(p->preview)preview_list(w,p);
        USER(PostMessageW)(w,PICK_START,0,0);return TRUE;
    }
    PickState *p=(PickState *)USER(GetWindowLongW)(w,DWLP_USER);
    if(!p || p->closing)return FALSE;
    if(msg==WM_CAPTURECHANGED && p->capturing && (HWND)lp!=w) {
        p->capturing=FALSE;p->pressed=FALSE;p->armed_click=FALSE;
        if(p->drag>=0 && p->preview && p->provisional && pick_live(p) && preview_owned(p))
            preview_abort_drag(w,p);
        p->drag=-1;
        if(lp && USER(IsChild)(w,(HWND)lp)){
            USER(SetTimer)(w,1,100,NULL);return FALSE;
        }
        finish_pick(w,p,0);return TRUE;
    }
    if(msg==WM_TIMER && wp==1)msg=PICK_CAPTURE;
    if(msg==WM_TIMER && wp==2){
        if(!pick_live(p) || !preview_owned(p))finish_pick(w,p,0);
        else if(p->preview)preview_marks(p);
        else if(p->used&1)place_mark(p,0,p->client[p->used-1]);
        return TRUE;
    }
    if(msg==WM_TIMER && wp==3){
        if(!p->drag_pending)return TRUE;
        USER(KillTimer)(w,3);p->drag_pending=FALSE;
        if(!pick_live(p) || !preview_owned(p))finish_pick(w,p,0);
        else if(p->drag>=0 && p->pressed && p->capturing)preview_drag(w,p,p->drag_screen);
        return TRUE;
    }
    if(msg==WM_CANCELMODE || msg==WM_CLOSE || (msg==WM_COMMAND && LOWORD(wp)==IDCANCEL && HIWORD(wp)==BN_CLICKED) ||
       (msg==WM_ACTIVATE && LOWORD(wp)==WA_INACTIVE && p->phase>=0)){
        finish_pick(w,p,0);return TRUE;
    }
    if(msg==PICK_START || msg==WM_LBUTTONDOWN || msg==WM_LBUTTONDBLCLK || msg==WM_LBUTTONUP || msg==WM_RBUTTONUP || msg==WM_COMMAND || msg==WM_KEYDOWN) {
        if(!pick_live(p) || (p->preview && !preview_owned(p))){finish_pick(w,p,0);return TRUE;}
    }
    switch(msg){
        case PICK_START:
            if(p->preview){
                if(!preview_start(w,p)){finish_pick(w,p,0);return TRUE;}
            }else {pick_phase(p,0);pick_label(w,p);USER(SetTimer)(w,2,150,NULL);}
            /* fall through */
        case PICK_CAPTURE:
            if(p->capturing && USER(GetCapture)()==w)return TRUE;
            if(USER(IsChild)(w,USER(GetCapture)()))return TRUE;
            USER(KillTimer)(w,1);USER(SetFocus)(w);USER(SetCapture)(w);
            p->capturing=USER(GetCapture)()==w;
            if(!p->capturing)finish_pick(w,p,0);
            return TRUE;
        case WM_MOUSEMOVE:
            if(p->preview && p->pressed && p->drag>=0 && p->capturing){
                p->drag_screen.x=(short)LOWORD(lp);p->drag_screen.y=(short)HIWORD(lp);
                USER(ClientToScreen)(w,&p->drag_screen);
                if(!p->drag_pending) {
                    p->drag_pending=USER(SetTimer)(w,3,16,NULL)!=0;
                    if(!p->drag_pending && pick_live(p) && preview_owned(p))preview_drag(w,p,p->drag_screen);
                }
            }
            /* fall through */
        case WM_SETCURSOR:
            USER(SetCursor)(USER(LoadCursorW)(NULL,MAKEINTRESOURCEW(32515)));return TRUE;
        case WM_LBUTTONDOWN:case WM_LBUTTONDBLCLK:
            p->down.x=(short)LOWORD(lp);p->down.y=(short)HIWORD(lp);
            USER(ClientToScreen)(w,&p->down);p->pressed=TRUE;
            if(p->preview)preview_press(w,p);
            return TRUE;
        case WM_LBUTTONUP:
            if(p->pressed && p->capturing){
                p->pressed=FALSE;
                POINT screen={(short)LOWORD(lp),(short)HIWORD(lp)};
                USER(ClientToScreen)(w,&screen);
                if(p->preview){preview_release(w,p,screen);return TRUE;}
                if(__builtin_abs(screen.x-p->down.x)>USER(GetSystemMetrics)(SM_CXDRAG) ||
                   __builtin_abs(screen.y-p->down.y)>USER(GetSystemMetrics)(SM_CYDRAG))return TRUE;
                canvas_click(w,p,screen);
            }return TRUE;
        case WM_RBUTTONUP:if(!p->preview)undo_pick(w,p);return TRUE;
        case WM_COMMAND:
            if(HIWORD(wp)==BN_KILLFOCUS && !p->capturing){USER(PostMessageW)(w,PICK_CAPTURE,0,0);return FALSE;}
            if(p->preview && LOWORD(wp)==401 && HIWORD(wp)==LBN_SELCHANGE){
                int layer=USER(SendDlgItemMessageW)(w,401,LB_GETCURSEL,0,0);
                if(layer==0 || layer==1)preview_select(w,p,layer);
                return TRUE;
            }
            if(HIWORD(wp)!=BN_CLICKED)return FALSE;
            if(p->preview && LOWORD(wp)==IDOK && p->drag<0){finish_pick(w,p,IDOK);return TRUE;}
            if(LOWORD(wp)==301){undo_pick(w,p);USER(PostMessageW)(w,PICK_CAPTURE,0,0);return TRUE;}
            if(LOWORD(wp)==302){p->corner=!p->corner;position_prompt(w,p);
                if(p->preview)preview_marks(p);else pick_label(w,p);
                USER(PostMessageW)(w,PICK_CAPTURE,0,0);return TRUE;}
            break;
        case WM_KEYDOWN:
            if(wp==VK_ESCAPE){finish_pick(w,p,0);return TRUE;}
            if(wp==VK_RETURN && p->preview && p->drag<0){finish_pick(w,p,IDOK);return TRUE;}
            if(wp==VK_BACK && !p->preview){undo_pick(w,p);return TRUE;}
            break;
    }
    return FALSE;
}
static int pick_four(HWND owner,PickState *p) {
    p->view=*(Byte **)(p->doc+0xa0);p->count=*(int *)(p->set+0x34);
    if(!p->view || !(p->paper=*(Byte **)(p->view+0xac)) || *(Byte **)(p->view+0x4b4)!=p->set ||
       !(p->canvas=*(HWND *)(p->view+4)) || !USER(IsWindowVisible)(p->canvas)) {
        explain(owner,L"请先在当前画纸显示选中的透光台组。");return 0;
    }
    if(p->count<2 || (unsigned)p->count>0x7fffffff/sizeof(*p->saved))return 0;
    p->saved=(SavedVisibility *)KERNEL(LocalAlloc)(LMEM_FIXED,p->count*sizeof(*p->saved));
    if(!p->saved){explain(owner,L"内存不足，无法开始四点对位。");return 0;}
    Byte **items=*(Byte ***)(p->set+0x1c);
    for(int i=0;i<p->count;++i){
        p->saved[i].row=items[i];p->saved[i].visible=*(int *)(items[i]+0x50);
    }
    p->phase=-1;p->enabled=*(int *)(p->set+0x98);p->group_opacity=*(int *)(p->set+0x60);
    for(int i=0;i<2;++i)p->opacity[i]=*(int *)(p->rows[i]+0x58);
    int result=USER(DialogBoxIndirectParamW)(IMPORT(Module,__typeof__(&GetModuleHandleA))(NULL),
        (LPCDLGTEMPLATEW)pick_dialog,owner,pick_proc,(LPARAM)p);
    int valid=result==IDOK && p->used==4 && pick_live(p);
    if(valid && !p->skip_preview){
        p->preview=TRUE;p->closing=FALSE;p->capturing=FALSE;p->pressed=FALSE;p->drag=-1;
        for(int k=0;k<2;++k)for(int axis=0;axis<2;++axis)
            __builtin_memcpy(p->lengths[k][axis],p->rows[k]+(axis?0x80:0x68),24);
        result=USER(DialogBoxIndirectParamW)(IMPORT(Module,__typeof__(&GetModuleHandleA))(NULL),
            (LPCDLGTEMPLATEW)preview_dialog,owner,pick_proc,(LPARAM)p);
        valid=result==IDOK && pick_live(p) && preview_owned(p);
    }
    clear_marks(p);pick_restore(p);KERNEL(LocalFree)((HLOCAL)p->saved);
    if(p->fit_error)explain(owner,preview_error(p->fit_error));
    return valid;
}
