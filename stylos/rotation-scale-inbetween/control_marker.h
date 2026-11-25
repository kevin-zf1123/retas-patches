/* Each dot is a nonactivating overlay owned by the tool dialog. Its alpha
   surface stays visible over host canvas repaints and never alters the cel. */
enum { MARK_RADIUS=7, MARK_SIZE=2*MARK_RADIUS+1, MARK_HIT_RADIUS=13 };
#define MARK_GDI(name) ((__typeof__(&name))system_api("gdi32.dll",#name))

static void clear_marks(PickState *p) {
    for(int i=0;i<4;++i){
        HWND dot=p->marks[i];
        if(dot && USER(IsWindow)(dot) && USER(GetWindow)(dot,GW_OWNER)==p->tool)
            USER(DestroyWindow)(dot);
        p->marks[i]=NULL;
    }
    for(int style=0;style<2;++style){
        if(p->dot_dc[style]){
            if(p->old_bitmap[style])MARK_GDI(SelectObject)(p->dot_dc[style],p->old_bitmap[style]);
            MARK_GDI(DeleteDC)(p->dot_dc[style]);
        }
        if(p->dot_bitmap[style])MARK_GDI(DeleteObject)(p->dot_bitmap[style]);
        p->dot_dc[style]=NULL;p->dot_bitmap[style]=NULL;p->old_bitmap[style]=NULL;
    }
}
static BOOL prepare_dot(PickState *p,int style) {
    if(p->dot_dc[style])return TRUE;
    BITMAPINFO info={0};
    info.bmiHeader.biSize=sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth=MARK_SIZE;info.bmiHeader.biHeight=-MARK_SIZE;
    info.bmiHeader.biPlanes=1;info.bmiHeader.biBitCount=32;info.bmiHeader.biCompression=BI_RGB;
    DWORD *pixels=NULL;
    p->dot_dc[style]=MARK_GDI(CreateCompatibleDC)(NULL);
    p->dot_bitmap[style]=MARK_GDI(CreateDIBSection)(NULL,&info,DIB_RGB_COLORS,(void **)&pixels,NULL,0);
    if(!p->dot_dc[style] || !p->dot_bitmap[style] || !pixels){clear_marks(p);return FALSE;}
    p->old_bitmap[style]=MARK_GDI(SelectObject)(p->dot_dc[style],p->dot_bitmap[style]);
    if(!p->old_bitmap[style] || p->old_bitmap[style]==HGDI_ERROR){p->old_bitmap[style]=NULL;clear_marks(p);return FALSE;}
    for(int y=0;y<MARK_SIZE;++y)for(int x=0;x<MARK_SIZE;++x){
        unsigned inside=0;
        for(int j=-3;j<=3;j+=2)for(int i=-3;i<=3;i+=2){
            int dx=8*(x-MARK_RADIUS)+i,dy=8*(y-MARK_RADIUS)+j;
            if(dx*dx+dy*dy<=64*MARK_RADIUS*MARK_RADIUS)++inside;
        }
        unsigned alpha=(inside*(style?155:255)+8)/16;
        unsigned other=alpha*(style?90:36)/255;
        pixels[y*MARK_SIZE+x]=(alpha<<24)|(alpha<<16)|(other<<8)|other;
    }
    return TRUE;
}
static void place_mark(PickState *p,int index,POINT at) {
    p->display[index]=at;
    RECT bounds;USER(GetClientRect)(p->canvas,&bounds);
    RECT box={at.x-MARK_RADIUS,at.y-MARK_RADIUS,at.x+MARK_RADIUS+1,at.y+MARK_RADIUS+1},clip;
    HWND dot=p->marks[index];
    if(!USER(IntersectRect)(&clip,&bounds,&box)){
        if(dot)USER(ShowWindow)(dot,SW_HIDE);
        return;
    }
    int style=index>=2;
    POINT position={clip.left,clip.top};USER(ClientToScreen)(p->canvas,&position);
    SIZE size={clip.right-clip.left,clip.bottom-clip.top};
    RECT panel,overlap,on_screen={position.x,position.y,position.x+size.cx,position.y+size.cy};
    if(USER(GetWindowRect)(p->tool,&panel) && USER(IntersectRect)(&overlap,&on_screen,&panel)){
        if(dot)USER(ShowWindow)(dot,SW_HIDE);
        return;
    }
    if(!prepare_dot(p,style))return;
    if(!dot) {
        dot=USER(CreateWindowExW)(WS_EX_TOOLWINDOW|WS_EX_NOACTIVATE|WS_EX_LAYERED|WS_EX_TRANSPARENT,
            L"STATIC",index==0?L"对位控制点 1":index==1?L"对位控制点 2":
            index==2?L"起始参考点":L"结束参考点",WS_POPUP,0,0,0,0,p->tool,NULL,
            IMPORT(Module,__typeof__(&GetModuleHandleA))(NULL),NULL);
        if(!dot)return;
        p->marks[index]=dot;
    }
    POINT offset={clip.left-box.left,clip.top-box.top};
    RECT current;
    if(USER(IsWindowVisible)(dot) && USER(GetWindowRect)(dot,&current) &&
       current.left==position.x && current.top==position.y && current.right-current.left==size.cx &&
       current.bottom-current.top==size.cy && p->crop[index].x==offset.x && p->crop[index].y==offset.y)return;
    BLENDFUNCTION blend={AC_SRC_OVER,0,255,AC_SRC_ALPHA};
    if(USER(UpdateLayeredWindow)(dot,NULL,&position,&size,p->dot_dc[style],&offset,0,&blend,ULW_ALPHA)){
        p->crop[index]=offset;
        if(!USER(IsWindowVisible)(dot)){
            USER(ShowWindow)(dot,SW_SHOWNOACTIVATE);
            if(style)for(int k=0;k<2;++k)if(p->marks[k] && USER(IsWindowVisible)(p->marks[k]))
                USER(SetWindowPos)(p->marks[k],HWND_TOP,0,0,0,0,SWP_NOMOVE|SWP_NOSIZE|SWP_NOACTIVATE|SWP_NOOWNERZORDER);
        }
    }else USER(ShowWindow)(dot,SW_HIDE);
}
