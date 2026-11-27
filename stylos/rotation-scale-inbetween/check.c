#define _WIN32_WINNT 0x0600
#include <windows.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "similarity.h"
#include "native_pose.h"
#include "four_point.h"

/* The Windows CRT reference uses the finite spiral centre, independently of the hot path. */
static double compare(Pose a, Pose b, unsigned n, unsigned d) {
    Pose x,y;
    interpolate(a,b,n,d,&x,&y);
    double phi=(a.angle-b.angle);
    if(phi>180)phi-=360;
    if(phi< -180)phi+=360;
    double t=(double)n/d,k=a.scale/b.scale,rad=acos(-1.0)/180;
    double dx=a.shift.x-b.shift.x,dy=a.shift.y-b.shift.y;
    double hx=(dx*cos(b.angle*rad)+dy*sin(b.angle*rad))/b.scale;
    double hy=(dy*cos(b.angle*rad)-dx*sin(b.angle*rad))/b.scale;
    double real=1-k*cos(phi*rad),imag=-k*sin(phi*rad);
    double square=real*real+imag*imag;
    double error=0;
    for(int i=0;i<2;++i) {
        double progress=t-i,scale=exp(progress*log(k)),angle=progress*phi;
        double tx=progress*hx,ty=progress*hy;
        if(square>0) {
            double cx=(hx*real+hy*imag)/square,cy=(hy*real-hx*imag)/square;
            double co=scale*cos(angle*rad),si=scale*sin(angle*rad);
            tx=cx-(co*cx-si*cy);
            ty=cy-(si*cx+co*cy);
        }
        Pose p=i?y:x;
        if (!isfinite(p.scale) || !isfinite(p.angle) || !isfinite(p.shift.x) ||
            !isfinite(p.shift.y) || !isfinite(tx) || !isfinite(ty)) return INFINITY;
        error=fmax(error,fabs(p.scale/scale-1));
        error=fmax(error,fabs(p.angle-angle)/180);
        error=fmax(error,hypot(p.shift.x-tx,p.shift.y-ty)/fmax(1,hypot(hx,hy)));
    }
    return error;
}
static volatile double sink;
__attribute__((noinline))
static void sample(const Pose *a,const Pose *b,unsigned n,unsigned d) {
    Pose x,y;
    interpolate(*a,*b,n,d,&x,&y);
    __asm__ volatile("" : : "m"(x), "m"(y) : "memory");
    sink=x.scale+y.shift.x+x.angle;
}
__attribute__((noinline))
static void sample_scale(double ratio,unsigned n,unsigned d) {
    sink=scale_at(ratio,n,d);
}
static int order(const void *a,const void *b) {
    double x=*(const double *)a,y=*(const double *)b;
    return (x>y)-(x<y);
}
static double match_error(Pose a,Point x,Pose b,Point y) {
    Point l=product(polar(a.scale,a.angle),x),r=product(polar(b.scale,b.angle),y);
    return hypot(l.x+a.shift.x-r.x-b.shift.x,l.y+a.shift.y-r.y-b.shift.y);
}
static int check_four(void) {
    Point points[]={{2,3},{7,6},{-3,5},{-9,15}}; /* T(z) = 2 i z + (3,1) */
    Pose a,b;
    if(fit_four(points,1,2,1,0,&a,&b)!=FIT_OK || a.angle!=45 || b.angle!= -45 ||
       fabs(a.scale-1.42)>1e-12 || fabs(b.scale-.71)>1e-12)return 0;
    for(int i=0;i<2;++i)if(match_error(a,points[i],b,points[i+2])>1e-11)return 0;
    for(int end=0;end<2;++end){
        if(fit_four(points,end,1,0,0,&a,&b)!=FIT_OK)return 0;
        Pose anchor=end?b:a;
        if(anchor.angle!=0 || anchor.scale!=1 || hypot(anchor.shift.x,anchor.shift.y)>1e-11)return 0;
    }
    Point move[]={{1,4},{5,6},{8,-10},{12,-8}};
    if(fit_four(move,2,7,0,0,&a,&b)!=FIT_OK || fabs(a.shift.x-2)>1e-12 ||
       fabs(a.shift.y+4)>1e-12 || fabs(b.shift.x+5)>1e-12 || fabs(b.shift.y-10)>1e-12)return 0;
    move[3].x+=.37;
    if(fit_four(move,1,2,1,0,&a,&b)!=FIT_LOCK || fit_four(move,1,2,0,0,&a,&b)!=FIT_OK)return 0;
    if(match_error(a,midpoint(move[0],move[1]),b,midpoint(move[2],move[3]))>1e-11)return 0;
    move[1]=move[0];return fit_four(move,1,2,0,0,&a,&b)==FIT_SEGMENT;
}
/* A relative-overlap test alone misses the common rotation of both sheets.
   Check the stored pose against the independently computed midpoint of an
   actual off-centre paper point on the C0076 2340 x 1654, 200 dpi sheets. */
static int check_c0076(void) {
    Pose a={1,0,{0,0}},b={1,1,{-.15,-.17}},l,r;
    interpolate(a,b,1,2,&l,&r);
    Point point={1170*2.54/200,827*2.54/200};
    double half=acos(-1.)/360;
    Point turn={cos(half),-sin(half)};
    /* B maps sheet 077 to sheet 075. The displacement of the square root
       of inverse(B) is inverse(B)'s translation divided by 1+turn. */
    Point h=quotient((Point){.15,.17},(Point){cos(2*half),sin(2*half)});
    Point shift=quotient(h,(Point){1+turn.x,turn.y});
    Point expected=product(turn,point);
    int lp,ld,rp,rd;
    PoseResidual lf=split_linear(l.scale,l.angle,&lp,&ld),rf=split_linear(r.scale,r.angle,&rp,&rd);
    l.angle=ld+(double)lf.degrees;r.angle=rd+(double)rf.degrees;
    Point result=product(polar(l.scale,l.angle),point);
    if(ld-rd!=-1 || l.angle!=-.5 || r.angle!=.5 || lp!=100 || rp!=100 ||
       hypot(result.x+l.shift.x-expected.x-shift.x,result.y+l.shift.y-expected.y-shift.y)>1e-10)return 0;
    Point partner=product(polar(1,-1),(Point){point.x+.15,point.y+.17});
    if(match_error(l,point,r,partner)>1e-10)return 0;
    Point pts[4]={point,{0,0},partner,h};
    for(int lock=0;lock<=1;++lock){
        if(fit_four(pts,1,2,lock,1,&l,&r)!=FIT_OK ||
           fabs(l.angle+.5)>1e-10 || fabs(r.angle-.5)>1e-10 ||
           match_error(l,pts[0],r,pts[2])>1e-10)return 0;
    }
    return 1;
}
int main(void) {
    if(!check_c0076())return 11;
    puts("C0076 stored half-degree, paper corner and four-point midpoint passed.");
    Point origin={7,-2},horizontal={5,3},vertical={10,-1},sheet={7.75,-6},client;
    /* client=(-96,-32) on a rotated/reflected, translated view. */
    if(!paper_to_client(origin,horizontal,vertical,128,sheet,&client) ||
       fabs(client.x+96)>1e-10 || fabs(client.y+32)>1e-10 ||
       paper_to_client(origin,horizontal,horizontal,128,sheet,&client))return 10;
    if(!check_four())return 9;
    puts("Four-landmark similarity, translation, endpoints, quantization and rejection cases passed.");
    Pose a={1.01,1,{0,10}},b={1,0,{0,0}},left,right;
    interpolate(a,b,0,2,&left,&right);
    if (!native_poses(a,b,0,2,0,&left,&right) || left.scale!=1 || left.angle!=0) return 4;
    interpolate(b,a,2,2,&left,&right);
    if (!native_poses(b,a,2,2,0,&left,&right) || right.scale!=1 || right.angle!=0) return 5;
    interpolate(a,b,0,2,&left,&right);
    if (native_poses(a,b,0,2,1,&left,&right)) return 6;
    interpolate(a,b,1,2,&left,&right);
    if (!native_poses(a,b,1,2,1,&left,&right)) return 7;
    /* Both distinct source features must coincide after native rounding. */
    for(int i=0;i<2;++i) {
        Point z={2+7*i,-3+4*i};
        Point target=product(polar(a.scale,a.angle),z);
        target.x+=a.shift.x; target.y+=a.shift.y;
        Point l=product(polar(left.scale,left.angle),z);
        Point r=product(polar(right.scale,right.angle),target);
        if (hypot(l.x+left.shift.x-r.x-right.shift.x,l.y+left.shift.y-r.y-right.shift.y)>1e-11) return 8;
    }
    double worst=0;
    unsigned count=0;
    int angles[]={-180,-179,-61,-1,0,1,60,179,180};
    int scales[]={5,25,64,99,100,101,144,400,1599,1600};
    unsigned denominators[]={1,2,3,4,7,8,16,17,10000};
    for(unsigned i=0;i<10;++i)for(unsigned j=0;j<10;++j)
        for(unsigned ai=0;ai<9;++ai)for(unsigned bi=0;bi<9;++bi)
            for(unsigned f=0;f<9;++f) {
                Pose a={scales[i]*.01,angles[ai],{2.75,-17.25}};
                Pose b={scales[j]*.01,angles[bi],{-4.25,9}};
                unsigned d=denominators[f],ns[]={0,1,d/2,d-1,d};
                for(unsigned k=0;k<5;++k) {double e=compare(a,b,ns[k],d);if(!isfinite(e))return 2;worst=fmax(worst,e);++count;}
            }
    printf("%u cases, maximum normalized error %.10g\n",count,worst);
    HANDLE thread=GetCurrentThread();
    DWORD_PTR mask,system;
    if(GetProcessAffinityMask(GetCurrentProcess(),&mask,&system))
        SetThreadAffinityMask(thread,mask & (~mask+1));
    for(int mode=0;mode<3;++mode) {
        Pose a={1.44,63,{5,-2}},b={.91,-3,{-6,9}};
        if(mode==1)b.shift=a.shift;
        unsigned ns[]={1,1,5,2},ds[]={2,4,16,7};
        for(int f=0;f<4;++f) {
            double trials[9];
            for(int trial=0;trial<10;++trial) {
                ULONG64 start,end;
                if(!QueryThreadCycleTime(thread,&start))return 3;
                if(mode==2) {
                    double ratio=a.scale/b.scale;
                    __asm__ volatile("" : "+m"(ratio));
                    for(int r=0;r<100000;++r)sample_scale(ratio,ns[f],ds[f]);
                } else {
                    for(int r=0;r<100000;++r)sample(&a,&b,ns[f],ds[f]);
                }
                if(!QueryThreadCycleTime(thread,&end))return 3;
                if(trial)trials[trial-1]=(end-start)/100000.;
            }
            qsort(trials,9,sizeof(double),order);
            const char *kind=mode==2?"scale only":(mode==1?"pair, zero offset":"pair, offset");
            printf("%s %u/%u %.1f thread cycles/call (median)\n",kind,ns[f],ds[f],trials[4]);
        }
    }
    return worst<1e-4?0:1;
}
