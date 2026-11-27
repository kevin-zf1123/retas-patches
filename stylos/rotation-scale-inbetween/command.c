#include "similarity.h"
#include "native_pose.h"
#include "four_point.h"

#include "ui.h"

#define MEMBER __attribute__((thiscall))
typedef unsigned char Byte;
typedef void (MEMBER *RowLength)(Byte *,const void *);
typedef void (MEMBER *RowValue)(Byte *,int);
typedef double (MEMBER *GetLength)(const void *,int);
typedef void (MEMBER *SetLength)(void *,double,int,double);

/* The installer extends each light-table row with its own fractional values. */
static PoseResidual *residual(Byte *row) { return (PoseResidual *)(row+0xe0); }
static int fractional_rotation_available(void) {
    HMODULE dll=IMPORT(Module,__typeof__(&GetModuleHandleA))("rcwCmn.dll");
    if(!dll)return 0;
    const Byte *base=(const Byte *)dll;
    const IMAGE_NT_HEADERS32 *pe=(const IMAGE_NT_HEADERS32 *)(base+((const IMAGE_DOS_HEADER *)base)->e_lfanew);
    const Byte *rotate=system_api("rcwCmn.dll","?CreateRotationImage@RCVOffscreen@@UAEXAAV1@HH@Z");
    const void *const *vtable=system_api("rcwCmn.dll","??_7RCVOffscreen32Bit@@6B@");
    if(pe->FileHeader.TimeDateStamp!=0x4eb9f91b || pe->OptionalHeader.SizeOfImage!=0xd7000 ||
       rotate!=base+0x5cdd0 || !vtable || vtable[8]!=rotate ||
       *(const Byte *const *)(rotate+6)!=base+0x8e9ca)return 0;
    /* Verify the full frame setup used by precision.S, excluding the relocated
       exception-handler address verified above. */
    unsigned hash=2166136261u;
    for(int i=0;i<0x5e;++i)
        if(i<6 || i>=10)hash=(hash^rotate[i])*16777619u;
    return hash==0x9249f9c2;
}

static Pose read_pose(Byte *row) {
    Pose p;
    p.scale=(*(int *)(row+0x98)+(double)residual(row)->percent)/100.;
    p.angle=*(int *)(row+0x9c)+(double)residual(row)->degrees;
    p.shift.x = IMPORT(LengthGet,GetLength)(row+0x68,1);
    p.shift.y = IMPORT(LengthGet,GetLength)(row+0x80,1);
    return p;
}

static int usable(Pose p) {
    return p.scale>=0.05 && p.scale<=16 && p.angle>=-180 && p.angle<=180 &&
        p.shift.x>=-1e8 && p.shift.x<=1e8 && p.shift.y>=-1e8 && p.shift.y<=1e8;
}

static void write_linear(Byte *row,double scale,double angle) {
    int percent,degrees;
    PoseResidual next=split_linear(scale,angle,&percent,&degrees);
    if(percent==*(int *)(row+0x98) && degrees==*(int *)(row+0x9c) &&
       next.percent==residual(row)->percent && next.degrees==residual(row)->degrees)return;
    *(int *)(row+0x98)=percent;
    *(int *)(row+0x9c)=degrees;
    *residual(row)=next;
    /* Both stock setters rebuild the entire bitmap. Commit both components
       before the one rebuild, including a change of just the fraction. */
    ((void (MEMBER *)(Byte *))address(RebuildRow))(row);
}

static void write_pose(Byte *row,Pose p) {
    for (int axis=0;axis<2;++axis) {
        /* RCLength is a 24-byte value object; copy the source DPI before setting centimetres. */
        unsigned words[6];
        const unsigned *old=(const unsigned *)(row+(axis ? 0x80 : 0x68));
        double value=axis?p.shift.y:p.shift.x;
        if(IMPORT(LengthGet,GetLength)(old,1)==value)continue;
        for (int i=0;i<6;++i) words[i]=old[i];
        IMPORT(LengthSet,SetLength)(words,value,1,0);
        ((RowLength)address(axis ? SetY : SetX))(row,words);
    }
    write_linear(row,p.scale,p.angle);
}

typedef Byte *(*AppGetter)(void);
typedef Byte *(MEMBER *SetGetter)(Byte *);
typedef Byte *(MEMBER *DocGetter)(Byte *,unsigned,int);
typedef void (MEMBER *ModalGate)(Byte *,int,const void *);


static int same_pair(Byte *set,Byte *a,Byte *b) {
    if (!set) return 0;
    int count=*(int *)(set+0x34),selected=0;
    Byte **items=*(Byte ***)(set+0x1c);
    for (int i=0;i<count;++i) {
        if (count!=2 && !*(int *)(items[i]+0x54)) continue;
        if (selected>=2 || items[i]!=(selected ? b : a)) return 0;
        ++selected;
    }
    return selected==2;
}
#include "canvas_pick.h"
__attribute__((force_align_arg_pointer))
void align_pair(Byte *a, Byte *b, Byte *palette) {
    Byte *app=((AppGetter)address(GetApp))();
    void **vtable=*(void ***)app;
    DocGetter document=(DocGetter)vtable[0x38/4];
    Byte *doc=document(app,0x43454c57,0);
    SetGetter current_set=(SetGetter)address(GetSet);
    Byte *set=current_set(palette);
    if (!doc || !same_pair(set,a,b)) return;
    HWND owner=USER(GetAncestor)(USER(GetActiveWindow)(),GA_ROOTOWNER);
    if (!owner) return;
    if (!a || !b || a==b || *(void **)(a+0xd0) || *(void **)(b+0xd0) || ((*(unsigned *)(a+0xa4)|*(unsigned *)(b+0xa4))&3)) {
        explain(owner,L"请选择两张不同的、未镜像的二维透光台参考图。");
        return;
    }
    unsigned modal_context[4]={7,0x48414e44,(unsigned)doc,0};
    ModalGate gate=(ModalGate)vtable[0x0c/4];
    gate(app,1,modal_context);
    BOOL skip_preview=FALSE;
    int step=choose_step(owner,&skip_preview);
    PickState pick={0};
    int four=step>0 && (step&FOUR_POINTS);
    Pose from,to,left,right;
    if(step>0 && document(app,0x43454c57,0)==doc && current_set(palette)==set && same_pair(set,a,b)){
        from=read_pose(a);to=read_pose(b);
        if(!usable(from) || !usable(to)){
            explain(owner,L"所选参考图的变换参数超出范围。");step=0;
        } else if(four){
            pick.app=app;pick.doc=doc;pick.palette=palette;pick.set=set;
            pick.rows[0]=a;pick.rows[1]=b;pick.pose[0]=from;pick.pose[1]=to;
            pick.document=document;pick.current_set=current_set;
            pick.step=step;pick.skip_preview=skip_preview;
            if(!pick_four(owner,&pick))step=0;
        }
    } else step=0;
    gate(app,0,0);
    if(step<=0 || document(app,0x43454c57,0)!=doc || current_set(palette)!=set ||
       !same_pair(set,a,b))return;
    unsigned n=step&0x3fff,d=((unsigned)step>>16)&0x3fff;
    int lock=(step&(1<<30))!=0;
    if(four){
        for(int i=0;i<2;++i){
            Pose now=read_pose(pick.rows[i]),saved=pick.pose[i];
            if(now.scale!=saved.scale || now.angle!=saved.angle || now.shift.x!=saved.shift.x || now.shift.y!=saved.shift.y){
                explain(owner,L"拾取期间参考图的变换发生变化，请重新点选。");return;
            }
        }
        int result=fit_four(pick.points,n,d,lock,fractional_rotation_available(),&left,&right);
        if(result!=FIT_OK){
            explain(owner,result==FIT_LOCK?L"测得的倍率比无法用整数百分比精确表示。请关闭倍率比锁定后重试。":
                result==FIT_SEGMENT?L"同一张原画上的两个特征点必须分开。":L"两段特征的长度比超出透光台可表示的范围。");
            return;
        }
    } else {
        interpolate(from,to,n,d,&left,&right);
        if(!usable(left) || !usable(right)){
            explain(owner,L"结果超出透光台的变换范围。可用倍率为 5% 至 1600%。");return;
        }
        if(!fractional_rotation_available() && !native_poses(from,to,n,d,lock,&left,&right)){
            explain(owner,L"此端点无法同时保持 100% 倍率和精确倍率比。可在环境设置中取消倍率比锁定。");return;
        }
    }
    if (!usable(left) || !usable(right)) {
        explain(owner,L"取整后的结果超出透光台的变换范围。");
        return;
    }
    write_pose(a,left);
    write_pose(b,right);
    ((RowValue)address(Refresh))(palette,0);
}
