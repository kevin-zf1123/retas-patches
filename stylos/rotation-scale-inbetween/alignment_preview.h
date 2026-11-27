/* Refit from the four source-paper coordinates on each release. Provisional
   display poses are never fed back as new source correspondences. */
static const WCHAR *preview_error(int error) {
    if(error==FIT_SEGMENT)return L"同一张原画的两个特征点不能重合。";
    if(error==FIT_LOCK)return L"这个位置无法精确锁定倍率比。请改选位置或关闭倍率比锁定。";
    return L"该位置超出透光台可表示的变换范围。";
}
static BOOL preview_owned(PickState *p) {
    const Pose *expected=p->provisional?p->staged:p->pose;
    return same_pose(read_pose(p->rows[0]),expected[0]) && same_pose(read_pose(p->rows[1]),expected[1]);
}
static int preview_fit(PickState *p) {
    Pose next[2];
    int fit=fit_four(p->points,p->step&0x3fff,((unsigned)p->step>>16)&0x3fff,
                     (p->step&(1<<30))!=0,fractional_rotation_available(),&next[0],&next[1]);
    if(fit!=FIT_OK)return fit;
    if(!usable(next[0]) || !usable(next[1]))return FIT_RANGE;
    for(int k=0;k<2;++k){
        write_pose(p->rows[k],next[k]);
        p->staged[k]=read_pose(p->rows[k]);
    }
    p->provisional=TRUE;return FIT_OK;
}
static void preview_marks(PickState *p) {
    Point origin,h,v;
    if(!paper_point(p,0,0,&origin,FALSE) || !paper_point(p,128,0,&h,FALSE) ||
       !paper_point(p,0,128,&v,FALSE)){clear_marks(p);return;}
    for(int i=0;i<4;++i){
        BOOL guide=i>=2;
        if(guide && (p->drag<0 || !p->drag_moved)){
            if(p->marks[i] && USER(IsWindowVisible)(p->marks[i]))USER(ShowWindow)(p->marks[i],SW_HIDE);
            continue;
        }
        int layer=guide?i-2:p->layer;
        Point source=p->points[layer*2+(guide?p->drag:i)];
        if(guide && layer==p->layer)source=p->drag_probe;
        Pose pose=guide?p->drag_pose[layer]:p->staged[layer];
        Point point=product(polar(pose.scale,pose.angle),source);
        point.x+=pose.shift.x;point.y+=pose.shift.y;
        Point client;
        if(!paper_to_client(origin,h,v,128,point,&client)){clear_marks(p);return;}
        POINT at={nearest(client.x),nearest(client.y)};
        place_mark(p,i,at);
    }
}
typedef const char *(MEMBER *StringData)(const Byte *);
static void preview_list(HWND w,PickState *p) {
    StringData get=(StringData)system_api("rcwCmn.dll","?GetStr@RCString@@QBEPBDXZ");
    void *vt=system_api("rcwCmn.dll","??_7RCString@@6B@");
    for(int i=0;i<2;++i){
        WCHAR text[320];const WCHAR *role=i?L"结束原画":L"起始原画";
        unsigned used=0;while(*role)text[used++]=*role++;
        Byte *row=p->rows[i],*doc=*(Byte **)(row+0x0c),*str=doc?doc+0x374:NULL;
        const char *name=get && str && *(void **)str==vt?get(str):NULL;
        BOOL path=FALSE;
        if(!name || !*name){
            Byte *files=*(Byte **)(row+0x1c);int index=*(int *)(row+0x20);
            if(doc && *(int *)(doc+0x354))index=0;
            if(files && index>=0 && index<*(int *)(files+0x20)){
                Byte **items=*(Byte ***)(files+8);Byte *item=items?items[index]:NULL;
                if(item && get && *(void **)(item+12)==vt){name=get(item+12);path=TRUE;}
            }
        }
        if(name && *name){
            WCHAR wide[512];int n=KERNEL(MultiByteToWideChar)(CP_ACP,0,name,-1,wide,512);
            if(n>1){
                int first=0;if(path)for(int j=0;j<n-1;++j)if(wide[j]==L'\\'||wide[j]==L'/')first=j+1;
                text[used++]=L'　';for(int j=first;j<n-1 && used<319;++j)text[used++]=wide[j];
            }
        }
        text[used]=0;USER(SendDlgItemMessageW)(w,401,LB_ADDSTRING,0,(LPARAM)text);
    }
}
static void preview_abort_drag(HWND w,PickState *p);
static void preview_select(HWND w,PickState *p,int layer) {
    if(p->drag>=0)preview_abort_drag(w,p);
    p->layer=layer;p->drag=-1;p->armed_click=FALSE;
    USER(SendDlgItemMessageW)(w,401,LB_SETCURSEL,layer,0);
    for(int i=0;i<2;++i)*(int *)(p->rows[i]+0x58)=i==layer?100:55;
    pick_refresh(p);preview_marks(p);
    USER(SetDlgItemTextW)(w,300,L"拖动红点实时对位。浅红为对应原点，实红为对位结果。");
}
static BOOL preview_start(HWND w,PickState *p) {
    p->fit_error=preview_fit(p);if(p->fit_error)return FALSE;
    for(int i=0;i<p->count;++i)*(int *)(p->saved[i].row+0x50)=
        p->saved[i].row==p->rows[0] || p->saved[i].row==p->rows[1];
    p->phase=2;preview_select(w,p,0);USER(SetTimer)(w,2,150,NULL);return TRUE;
}
static void preview_press(HWND w,PickState *p) {
    p->drag=-1;
    if(pick_target(w,p,p->down)!=-1)return;
    preview_marks(p);POINT pos=p->down;USER(ScreenToClient)(p->canvas,&pos);
    int best=MARK_HIT_RADIUS*MARK_HIT_RADIUS;
    for(int i=0;i<2;++i){
        if(!p->marks[i] || !USER(IsWindowVisible)(p->marks[i]))continue;
        int dx=pos.x-p->display[i].x,dy=pos.y-p->display[i].y;
        if(__builtin_abs(dx)>MARK_HIT_RADIUS || __builtin_abs(dy)>MARK_HIT_RADIUS)continue;
        int dist=dx*dx+dy*dy;if(dist<=best){p->drag=i;best=dist;}
    }
    if(p->drag>=0){
        if(!paper_point(p,pos.x,pos.y,&p->drag_down_paper,FALSE)){p->drag=-1;return;}
        p->drag_pose[0]=p->staged[0];p->drag_pose[1]=p->staged[1];
        p->drag_probe=p->drag_source=p->points[2*p->layer+p->drag];
        p->drag_moved=FALSE;
    }
}
/* Always invert the mouse displacement through the pose at mouse-down.
   Quantized preview writes on earlier mouse moves must not influence this fit. */
static BOOL preview_drag(HWND w,PickState *p,POINT screen) {
    if(p->drag<0 || pick_target(w,p,screen)!=-1)return FALSE;
    POINT pos=screen;USER(ScreenToClient)(p->canvas,&pos);
    Point now;if(!paper_point(p,pos.x,pos.y,&now,FALSE))return FALSE;
    Pose pose=p->drag_pose[p->layer];Point q=polar(pose.scale,pose.angle);
    Point delta={now.x-p->drag_down_paper.x,now.y-p->drag_down_paper.y};
    Point original=product(q,p->drag_source);GetLength get=IMPORT(LengthGet,GetLength);
    double x=original.x+pose.shift.x+delta.x+get(p->paper+0xc8,1);
    double y=original.y+pose.shift.y+delta.y+get(p->paper+0xe0,1);
    if(!(x>=0 && y>=0 && x<get(p->paper+0x90,1) && y<get(p->paper+0xa8,1))){
        USER(SetDlgItemTextW)(w,300,L"控制点需留在画纸内。");return FALSE;
    }
    Point correction=quotient(delta,q);
    Point desired={p->drag_source.x+correction.x,p->drag_source.y+correction.y};
    int slot=2*p->layer+p->drag;Point before=p->points[slot];
    p->drag_probe=desired;
    if(screen.x!=p->down.x || screen.y!=p->down.y)p->drag_moved=TRUE;
    if(before.x!=desired.x || before.y!=desired.y){
        p->points[slot]=desired;
        int rc=preview_fit(p);
        if(rc){p->points[slot]=before;USER(SetDlgItemTextW)(w,300,preview_error(rc));preview_marks(p);return FALSE;}
        pick_refresh(p);
    }
    preview_marks(p);return TRUE;
}
static void preview_abort_drag(HWND w,PickState *p) {
    USER(KillTimer)(w,3);p->drag_pending=FALSE;
    p->points[2*p->layer+p->drag]=p->drag_source;p->drag=-1;p->armed_click=FALSE;
    if(preview_fit(p)==FIT_OK)pick_refresh(p);
    preview_marks(p);
}
static void preview_release(HWND w,PickState *p,POINT screen) {
    USER(KillTimer)(w,3);p->drag_pending=FALSE;
    int target=pick_target(w,p,screen);
    BOOL moved=screen.x!=p->down.x || screen.y!=p->down.y;
    if(p->drag>=0 && (moved || p->drag_moved)){
        if(!preview_drag(w,p,screen)){preview_abort_drag(w,p);return;}
        p->drag=-1;p->armed_click=FALSE;
        preview_marks(p);USER(SetDlgItemTextW)(w,300,L"已按原中割比对齐。可继续拖动红点，或确认。");
        return;
    }
    p->drag=-1;
    if(target!=pick_target(w,p,p->down) || !target)return;
    if(target==401){
        HWND list=USER(GetDlgItem)(w,401);POINT pt=screen;USER(ScreenToClient)(list,&pt);
        LRESULT hit=USER(SendMessageW)(list,LB_ITEMFROMPOINT,0,MAKELPARAM(pt.x,pt.y));
        if(!HIWORD(hit) && LOWORD(hit)<2)preview_select(w,p,LOWORD(hit));
        return;
    }
    if(target>0){p->armed_click=FALSE;USER(SendMessageW)(w,WM_COMMAND,target,(LPARAM)USER(GetDlgItem)(w,target));return;}
    if(__builtin_abs(screen.x-p->down.x)>USER(GetSystemMetrics)(SM_CXDRAG) ||
       __builtin_abs(screen.y-p->down.y)>USER(GetSystemMetrics)(SM_CYDRAG)){p->armed_click=FALSE;return;}
    DWORD time=(DWORD)USER(GetMessageTime)();
    if(p->armed_click && time-p->last_time<=USER(GetDoubleClickTime)() &&
       __builtin_abs(screen.x-p->last_click.x)<=USER(GetSystemMetrics)(SM_CXDOUBLECLK)/2 &&
       __builtin_abs(screen.y-p->last_click.y)<=USER(GetSystemMetrics)(SM_CYDOUBLECLK)/2){
        finish_pick(w,p,IDOK);return;
    }
    p->last_click=screen;p->last_time=time;p->armed_click=TRUE;
}
