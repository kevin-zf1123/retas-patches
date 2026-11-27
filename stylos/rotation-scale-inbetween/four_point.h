/* The four landmarks are expressed in the two source sheets' centred cm
   coordinates. Fit the full similarity first, then round only for the host. */
static Point midpoint(Point a,Point b) {
    Point m={a.x+(b.x-a.x)*.5,a.y+(b.y-a.y)*.5};return m;
}
static double direction(Point q) {
    double angle;
    __asm__("fpatan" : "=t"(angle) : "0"(q.x),"u"(q.y) : "st(1)");
    return angle*57.295779513082320877;
}
/* Invert the native affine view sampled at (0,0), (step,0), and
   (0,step). Both source and destination may be rotated or reflected. */
static int paper_to_client(Point origin,Point h,Point v,double step,Point paper,Point *client) {
    h.x-=origin.x;h.y-=origin.y;v.x-=origin.x;v.y-=origin.y;
    double det=h.x*v.y-h.y*v.x;
    if(!(__builtin_fabs(det)>1e-24))return 0;
    paper.x-=origin.x;paper.y-=origin.y;
    client->x=step*(paper.x*v.y-paper.y*v.x)/det;
    client->y=step*(h.x*paper.y-h.y*paper.x)/det;
    return __builtin_fabs(client->x)<1e8 && __builtin_fabs(client->y)<1e8;
}
enum { FIT_OK, FIT_SEGMENT, FIT_RANGE, FIT_LOCK };
static int fit_four(const Point p[4],unsigned n,unsigned d,int lock,int fractional,Pose *left,Pose *right) {
    Point av={p[1].x-p[0].x,p[1].y-p[0].y}, bv={p[3].x-p[2].x,p[3].y-p[2].y};
    if(!(av.x*av.x+av.y*av.y>1e-16) || !(bv.x*bv.x+bv.y*bv.y>1e-16))return FIT_SEGMENT;
    Point q=quotient(bv,av), ma=midpoint(p[0],p[1]), mb=midpoint(p[2],p[3]);
    double k=root(q.x*q.x+q.y*q.y);
    if(!(k>=5./1600 && k<=1600./5))return FIT_RANGE;
    Point offset=product(q,ma);
    Pose transform={k,direction(q),{mb.x-offset.x,mb.y-offset.y}};
    Pose identity={1,0,{0,0}};
    interpolate(transform,identity,n,d,left,right);
    double a=left->scale*100,b=right->scale*100;
    if(!(a>=5 && a<=1600 && b>=5 && b<=1600))return FIT_RANGE;
    if(fractional)return FIT_OK;
    Point target;
    if(!n)target=ma;
    else if(n==d)target=mb;
    else {
        target=product(polar(left->scale,left->angle),ma);
        target.x+=left->shift.x;target.y+=left->shift.y;
    }
    int ia=nearest(a),ib=nearest(b);
    if(lock) {
        double best=1e20;
        /* The supported native scale range has only 1596 possible denominators. */
        for(int y=5;y<=1600;++y) {
            int x=nearest(k*y);
            if(x<5 || x>1600 || (!n && x!=100) || (n==d && y!=100))continue;
            if(__builtin_fabs(x-k*y)>1e-10*x)continue;
            double cost=__builtin_fabs(y-b);
            if(cost<best){best=cost;ia=x;ib=y;}
        }
        if(best==1e20)return FIT_LOCK;
    } else {
        /* Prefer the closest segment lengths among the integer scales within
           one percentage point of the requested timing. */
        double best=__builtin_fabs(ia-k*ib), movement=__builtin_fabs(ia-a)+__builtin_fabs(ib-b);
        int first=ia,second=ib;
        for(int x=first-1;x<=first+1;++x)for(int y=second-1;y<=second+1;++y){
            if(x<5 || x>1600 || y<5 || y>1600 || (!n && x!=100) || (n==d && y!=100) ||
               __builtin_fabs(x-a)>1 || __builtin_fabs(y-b)>1)continue;
            double error=__builtin_fabs(x-k*y), cost=__builtin_fabs(x-a)+__builtin_fabs(y-b);
            if(error<best || (error==best && cost<movement)){ia=x;ib=y;best=error;movement=cost;}
        }
    }
    left->scale=ia*.01;right->scale=ib*.01;
    int total_angle=nearest(transform.angle);
    left->angle=n==d?total_angle:nearest(left->angle);
    right->angle=left->angle-total_angle;
    Point la=product(polar(left->scale,left->angle),ma);
    Point rb=product(polar(right->scale,right->angle),mb);
    left->shift.x=target.x-la.x;left->shift.y=target.y-la.y;
    right->shift.x=target.x-rb.x;right->shift.y=target.y-rb.y;
    return FIT_OK;
}
