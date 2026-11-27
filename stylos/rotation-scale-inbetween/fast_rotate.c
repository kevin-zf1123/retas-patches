/* Block-wise equivalent of the 32-bit nearest-neighbour light-table rotation.
   Stylos normally changes the virtual-bitmap cursor for every source pixel.
   Consecutive samples in one 256-pixel block can use its locked row directly. */
#include "ui.h"
#include "similarity.h"

typedef unsigned char Byte;
#define MEMBER __attribute__((thiscall))
typedef void (MEMBER *BlockLock)(Byte *,int,int,int);
typedef void (MEMBER *BlockUnlock)(Byte *);
typedef void (MEMBER *CreateSurface)(Byte *,void *,int,int,double);
typedef void *(*MemoryManager)(void);
typedef int (MEMBER *Depth)(Byte *);

static int extent(double a,double b) {
    /* The stock renderer truncates all four projected corners before bounding. */
    int corners[]={0,(int)a,(int)b,(int)(a+b)},low=0,high=0;
    for(int i=1;i<4;++i){
        if(corners[i]<low)low=corners[i];
        if(corners[i]>high)high=corners[i];
    }
    return high-low;
}

__attribute__((visibility("hidden"),force_align_arg_pointer))
int fast_rotate(Byte *dest,Byte *source,double angle) {
    const char *dll="rcwCmn.dll";
    Depth depth=(Depth)system_api(dll,"?GetBitCount@RCVOffscreen@@QBEHXZ");
    if(!depth || depth(source)!=32 || depth(dest)!=32 ||
       *(int *)(source+0x44) || *(int *)(dest+0x44))return 0;
    int w=*(int *)(source+12),h=*(int *)(source+16);
    /* Bounds keep the native signed 20.12 coordinate arithmetic in range. */
    if(w<1 || h<1 || w>65535 || h>65535)return 0;
    BlockLock lock=(BlockLock)system_api(dll,"?Lock@RCVOffscreen@@QAEXHHH@Z");
    BlockUnlock unlock=(BlockUnlock)system_api(dll,"?Unlock@RCVOffscreen@@QAEXXZ");
    MemoryManager memory=(MemoryManager)system_api(dll,"?rcGetVirtualMemory@@YAPAVRCVirtualMemory@@XZ");
    if(!lock || !unlock || !memory)return 0;
    Point rot=polar(1,angle);
    int dw=extent(w*rot.x,h*rot.y),dh=extent(-w*rot.y,h*rot.x);
    if(!dw || !dh)return 0;
    int co=(int)(rot.x*4096),si=(int)(rot.y*4096);
    CreateSurface create=(CreateSurface)(*(void ***)dest)[2];
    create(dest,memory(),dw,dh,*(double *)(source+0x18));
    int src_pitch=*(int *)(source+0x28)/4,dst_pitch=*(int *)(dest+0x28)/4;
    DWORD empty=*(DWORD *)(source+0x80),outside=*(DWORD *)(dest+0x80);
    for(int top=0;top<dh;top+=256)for(int left=0;left<dw;left+=256){
        lock(dest,left,top,1);
        DWORD *out=*(DWORD **)(dest+0x68);
        if(!out){
            unlock(dest);
            ((void (MEMBER *)(Byte *))(*(void ***)dest)[3])(dest);
            return 0;
        }
        int block_x=-1,block_y=-1;
        const DWORD *in=NULL;
        for(int y=top;y<dh && y<top+256;++y){
            int sx=(w/2)*4096+(left-dw/2)*co-(y-dh/2)*si;
            int sy=(h/2)*4096+(left-dw/2)*si+(y-dh/2)*co;
            DWORD *line=out+(y-top)*dst_pitch;
            int end=min(dw,left+256);
            for(int x=left;x<end;){
                int u=sx>>12,v=sy>>12;
                DWORD color=outside;
                if((unsigned)u<(unsigned)w && (unsigned)v<(unsigned)h){
                    if((u>>8)!=block_x || (v>>8)!=block_y){
                        if(block_x>=0)unlock(source);
                        lock(source,u,v,0);
                        block_x=u>>8;block_y=v>>8;
                        in=*(DWORD **)(source+0x68);
                    }
                    const DWORD *pixel=in?in+(v&255)*src_pitch+(u&255):NULL;
                    /* Near zero degrees the inverse map usually advances one
                       source column on the same scanline. Copy up to (but not
                       across) the next fractional carry or block boundary. */
                    if(pixel && co>=4060){
                        int count=min(end-x,min(w-u,256-(u&255)));
                        if(co<4096)count=min(count,1+(sx&4095)/(4096-co));
                        if(si>0)count=min(count,1+(4095-(sy&4095))/si);
                        if(si<0)count=min(count,1+(sy&4095)/-si);
                        __builtin_memcpy(line+x-left,pixel,count*sizeof(DWORD));
                        x+=count;sx+=count*co;sy+=count*si;
                        continue;
                    }
                    color=pixel?*pixel:empty;
                }
                line[x-left]=color;
                ++x;sx+=co;sy+=si;
            }
        }
        if(block_x>=0)unlock(source);
        unlock(dest);
    }
    return 1;
}
