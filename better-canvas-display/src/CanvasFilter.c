#include <windows.h>
#include <math.h>
#include <string.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    unsigned char header[12];
    unsigned char *pixels;
    int width, height, bpp, stride;
} Bitmap;
typedef struct { double x, y; } PointD;
enum { CellShift = 4, CellSize = 1 << CellShift };
typedef struct { DWORD color; BOOL solid; } ColorCell;
typedef struct {
    BYTE *bits;
    int width, height, pitch, cellsAcross;
    ColorCell *cells;
} Surface;
typedef struct { int mode; WCHAR path[MAX_PATH]; } Preference;
static Preference pref = {-1};
static short cubic[256][4];
static void initialize(void)
{
    if (pref.mode >= 0) return;
    DWORD n = GetEnvironmentVariableW(L"APPDATA",pref.path,MAX_PATH);
    const WCHAR suffix[] = L"\\CELSYS\\RETAS STUDIO\\Stylos\\2.0\\Preference\\CanvasZoom.ini";
    if (n && n + sizeof(suffix)/sizeof(WCHAR) <= MAX_PATH) lstrcatW(pref.path,suffix);
    else pref.path[0]=0;
    pref.mode = pref.path[0] ? GetPrivateProfileIntW(L"Canvas",L"Interpolation",1,pref.path) : 1;
    if ((unsigned)pref.mode>2) pref.mode=1;
    for (int i=0;i<256;i++) {
        double f=i/256., f2=f*f, f3=f2*f;
        cubic[i][0]=(short)floor(0.5+256*(-0.5*f+f2-0.5*f3));
        cubic[i][1]=(short)floor(0.5+256*(1-2.5*f2+1.5*f3));
        cubic[i][3]=(short)floor(0.5+256*(-0.5*f2+0.5*f3));
        cubic[i][2]=256-cubic[i][0]-cubic[i][1]-cubic[i][3];
    }
}
__declspec(dllexport) int GetCanvasFilter(void) { initialize(); return pref.mode; }
__declspec(dllexport) int SetCanvasFilter(int mode)
{
    initialize();
    if ((unsigned)mode>2 || !pref.path[0]) return 0;
    WCHAR s[2]={(WCHAR)('0'+mode),0};
    if (!WritePrivateProfileStringW(L"Canvas",L"Interpolation",s,pref.path)) return 0;
    pref.mode=mode;return 1;
}
int NativeTile(void *entry, void *view, void *dst,void *src,void *origin,void *bounds,void *input,void *output);
static BYTE *host(void) {return (BYTE *)GetModuleHandleW(NULL);}
static int field(const void *p,size_t pos){int result;memcpy(&result,(const BYTE*)p+pos,sizeof(result));return result;}
static int clampi(int x,int lo,int hi){return x<lo?lo:x>hi?hi:x;}
static BYTE channel(int sum){return (BYTE)clampi((sum+32768)>>16,0,255);}
static inline int pixel_floor(double value)
{
    int whole = (int)value;
    return whole - (value < whole);
}

static BOOL solid_color(const Surface *s, int left, int top, int right, int bottom, DWORD *color)
{
    const DWORD *first = (const DWORD *)(s->bits + top * s->pitch) + left;
    *color = *first;
    for (int y = top; y < bottom; ++y) {
        const DWORD *row = (const DWORD *)(s->bits + y * s->pitch);
        DWORD different = 0;
        for (int x = left; x < right; ++x)
            different |= row[x] ^ *color;
        if (different)
            return FALSE;
    }
    return TRUE;
}

static void classify_cells(Surface *s, int mode)
{
    int before = mode == 2 ? 1 : 0;
    int after = mode == 2 ? 2 : 1;
    for (int y = 0; y < s->height; y += CellSize) {
        ColorCell *cell = s->cells + (y >> CellShift) * s->cellsAcross;
        for (int x = 0; x < s->width; x += CellSize, ++cell) {
            // Include the interpolation footprint across cell boundaries.
            cell->solid = solid_color(s, max(0, x - before), max(0, y - before),
                min(s->width, x + CellSize + after), min(s->height, y + CellSize + after), &cell->color);
        }
    }
}

static void mix(BYTE *out,const Surface *s,double x,double y,int mode)
{
    int ix = pixel_floor(x), iy = pixel_floor(y);
    int cx = clampi(ix, 0, s->width - 1) >> CellShift;
    int cy = clampi(iy, 0, s->height - 1) >> CellShift;
    const ColorCell *cell = &s->cells[cy * s->cellsAcross + cx];
    if (cell->solid) {
        memcpy(out, &cell->color, sizeof(cell->color));
        return;
    }
    int fx=clampi((int)((x-ix)*256),0,255), fy=clampi((int)((y-iy)*256),0,255);
    short lx[2]={(short)(256-fx),(short)fx},ly[2]={(short)(256-fy),(short)fy};
    int taps=mode==1?2:4;
    const short *wx=mode==1?lx:cubic[fx],*wy=mode==1?ly:cubic[fy];
    if (mode==2) {--ix;--iy;}
    int columns[4],sums[4]={0};
    for(int i=0;i<taps;i++)columns[i]=4*clampi(ix+i,0,s->width-1);
    for(int j=0;j<taps;j++){
        const BYTE *row=s->bits+clampi(iy+j,0,s->height-1)*s->pitch;
        for(int i=0;i<taps;i++){
            const BYTE *pixel=row+columns[i];
            int weight=wx[i]*wy[j];
            for(int c=0;c<4;c++)sums[c]+=weight*pixel[c];
        }
    }
    for(int c=0;c<4;c++)out[c]=channel(sums[c]);
}
static int read_region(BYTE *view,void *source,RECT bounds,Surface *out)
{
    int w=bounds.right-bounds.left,h=bounds.bottom-bounds.top;
    if(w<=0||h<=0 || (size_t)w*h>4*1024*1024) return 0;
    out->width=w; out->height=h; out->pitch=w*4;
    size_t bytes = (size_t)h * out->pitch;
    out->cellsAcross = (w + CellSize - 1) >> CellShift;
    size_t cells = (size_t)out->cellsAcross * ((h + CellSize - 1) >> CellShift);
    out->bits=HeapAlloc(GetProcessHeap(),HEAP_ZERO_MEMORY,bytes + cells * sizeof(ColorCell));
    if(!out->bits)return 0;
    out->cells = (ColorCell *)(out->bits + bytes);
    // The native 1:1 compositor only consumes one 256-pixel source block per call.
    Bitmap dst={{0},out->bits,w,h,32,w*4};
    BYTE plain[0x410];
    memcpy(plain,view,sizeof(plain));
    memset(plain+0x2d4,0,8); // scratch conversion always runs in source orientation
    POINT origin={0,0};
    RECT local={0,0,w,h};
    for(int y=bounds.top;y<bounds.bottom;){
        int bottom=min(bounds.bottom,(y|255)+1);
        for(int x=bounds.left;x<bounds.right;){
            int right=min(bounds.right,(x|255)+1);
            RECT src={x,y,right,bottom},dstrect={x-bounds.left,y-bounds.top,right-bounds.left,bottom-bounds.top};
            if(!NativeTile(host()+0x62200,plain,&dst,source,&origin,&local,&src,&dstrect)) return 0;
            x=right;
        }
        y=bottom;
    }
    return 1;
}
static PointD inverse(BYTE *v,double x,double y)
{
    x+=field(v,0x308)-field(v,0x2c4);
    y+=field(v,0x30c)-field(v,0x2c8);
    double scale=field(v,0x290);
    PointD p={(x*field(v,0x2dc)+y*field(v,0x2e0)+field(v,0x2fc))/scale-.5,
        (x*field(v,0x2e4)+y*field(v,0x2e8)+field(v,0x300))/scale-.5};
    return p;
}
static void smooth(BYTE *view,Bitmap *dst,void *source,const RECT *buffer,RECT *updated)
{
    RECT area=*updated;
    int sw=field(source,12),sh=field(source,16);
    int scale=field(view,0x290);
    if(scale<=0||sw<=0||sh<=0||dst->bpp!=32||!dst->pixels) return;
    RECT limit={0,0,dst->width,dst->height};
    if(!IntersectRect(&area,&area,&limit))return;
    int pad=(2*scale+4095)/4096;
    InflateRect(&area,pad,pad);
    IntersectRect(&area,&area,&limit);
    int x0=area.left,y0=area.top,x1=area.right-1,y1=area.bottom-1;
    PointD start=inverse(view,buffer->left+x0+.5,buffer->top+y0+.5);
    double ax=(double)field(view,0x2dc)/scale,ay=(double)field(view,0x2e4)/scale;
    double bx=(double)field(view,0x2e0)/scale,by=(double)field(view,0x2e8)/scale;
    double dx=ax*(x1-x0),dy=ay*(x1-x0),ex=bx*(y1-y0),ey=by*(y1-y0);
    RECT src={clampi((int)floor(start.x+min(0.,dx)+min(0.,ex))-1,0,sw),
              clampi((int)floor(start.y+min(0.,dy)+min(0.,ey))-1,0,sh),
              clampi((int)ceil(start.x+max(0.,dx)+max(0.,ex))+3,0,sw),
              clampi((int)ceil(start.y+max(0.,dy)+max(0.,ey))+3,0,sh)};
    Surface s={0};
    if(read_region(view,source,src,&s)){
        DWORD solid;
        BOOL uniform = solid_color(&s, 0, 0, s.width, s.height, &solid);
        if (!uniform)
            classify_cells(&s, pref.mode);
        // Fully interior flat areas need only a row fill, even after rotation.
        BOOL interior = start.x + min(0.,dx) + min(0.,ex) > 0 &&
            start.y + min(0.,dy) + min(0.,ey) > 0 &&
            start.x + max(0.,dx) + max(0.,ex) < sw - 1 &&
            start.y + max(0.,dy) + max(0.,ey) < sh - 1;
        for(int y=y0;y<=y1;y++){
            double px=start.x+(y-y0)*bx,py=start.y+(y-y0)*by;
            BYTE *d=dst->pixels+y*dst->stride+4*x0;
            if (uniform && interior) {
                DWORD *row = (DWORD *)d;
                for (int x = 0; x <= x1-x0; ++x)
                    row[x] = solid;
            } else {
                for(int x=x0;x<=x1;x++,d+=4,px+=ax,py+=ay) {
                    if (interior || (px>=-.5&&px<sw-.5&&py>=-.5&&py<sh-.5)) {
                        if (uniform)
                            memcpy(d, &solid, sizeof(solid));
                        else
                            mix(d,&s,px-src.left,py-src.top,pref.mode);
                    }
                }
            }
        }
        *updated=area;
    }
    if(s.bits)HeapFree(GetProcessHeap(),0,s.bits);
}
int WINAPI CanvasScale(BYTE *view,Bitmap *dst,void *source,POINT *origin,RECT *buffer,RECT *src,RECT *out)
{
    int ok=NativeTile(host()+0x61060,view,dst,source,origin,buffer,src,out);
    if(ok&&GetCanvasFilter())smooth(view,dst,source,buffer,out);
    return ok;
}
int WINAPI CanvasRotate(BYTE *view,Bitmap *dst,void *source,POINT *origin,RECT *buffer,RECT *out,int block)
{
    int ok=NativeTile(host()+0x629d0,view,dst,source,origin,buffer,out,(void*)block);
    if(ok&&GetCanvasFilter())smooth(view,dst,source,buffer,out);
    return ok;
}
