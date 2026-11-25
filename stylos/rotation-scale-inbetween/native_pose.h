/* Select the values the stock 2D light-table setters can represent.
   The input and output shifts are in source-centred centimetres. */
static int nearest(double x) { return (int)(x<0 ? x-0.5 : x+0.5); }

static int native_poses(Pose from,Pose to,unsigned n,unsigned d,int lock,Pose *left,Pose *right) {
    int a=nearest(from.scale*100), b=nearest(to.scale*100);
    int gcd=a, divisor=b;
    while(divisor) {
        int remainder=gcd%divisor;
        gcd=divisor;
        divisor=remainder;
    }
    int pa=a/gcd,pb=b/gcd;
    double la=left->scale*100,lb=right->scale*100;
    int steps=nearest(la/pa);
    int smallest=pa<pb?pa:pb,largest=pa>pb?pa:pb;
    if(steps<(5+smallest-1)/smallest) steps=(5+smallest-1)/smallest;
    if(steps>1600/largest) steps=1600/largest;
    int exact_a=pa*steps,exact_b=pb*steps;
    int endpoint_ok=(n!=0 || exact_a==100) && (n!=d || exact_b==100);
    if(lock && !endpoint_ok) return 0;
    if(lock || (endpoint_ok && __builtin_fabs(exact_a-la)<=1 && __builtin_fabs(exact_b-lb)<=1)) {
        left->scale=exact_a/100.;
        right->scale=exact_b/100.;
    } else {
        left->scale=nearest(la)/100.;
        right->scale=nearest(lb)/100.;
    }
    int angle_delta=nearest(left->angle-right->angle);
    left->angle=nearest(left->angle);
    right->angle=left->angle-angle_delta;
    if (from.shift.x!=to.shift.x || from.shift.y!=to.shift.y) {
        Point delta={from.shift.x-to.shift.x,from.shift.y-to.shift.y};
        Point h=quotient(delta,polar(to.scale,to.angle));
        Point displacement=product(polar(right->scale,right->angle),h);
        right->shift.x=left->shift.x-displacement.x;
        right->shift.y=left->shift.y-displacement.y;
    }
    return 1;
}
