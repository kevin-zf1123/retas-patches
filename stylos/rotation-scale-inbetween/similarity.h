/* Source coordinates are centimetres relative to each sheet's centre. */
typedef struct { double x, y; } Point;
typedef struct { double scale, angle; Point shift; } Pose;

static Point product(Point a, Point b) {
    Point p = {a.x*b.x - a.y*b.y, a.x*b.y + a.y*b.x};
    return p;
}
static Point quotient(Point a, Point b) {
    double reciprocal = 1/(b.x*b.x + b.y*b.y);
    Point p = {(a.x*b.x + a.y*b.y)*reciprocal, (a.y*b.x - a.x*b.y)*reciprocal};
    return p;
}
static double root(double x) {
    double r;
    __asm__("fsqrt" : "=t"(r) : "0"(x));
    return r;
}
static Point polar(double radius, double degrees) {
    if (degrees == 0) { Point p = {radius,0}; return p; }
    double c, s;
    __asm__("fsincos" : "=t"(c), "=u"(s)
            : "0"(degrees * 0.017453292519943295769));
    Point p = {radius*c, radius*s};
    return p;
}
/* Fixed x87 sequence. F2XM1 only receives the fractional part in (-1,1). */
static double power(double k, double t) {
    double r;
    const float one=1.0f;
    __asm__("fyl2x; fld %%st(0); frndint; fxch; fsub %%st(1),%%st;"
            "f2xm1; fadds %3; fscale; fstp %%st(1)"
            : "=t"(r) : "0"(k), "u"(t), "m"(one) : "st(1)");
    return r;
}

static double scale_at(double k, unsigned n, unsigned d) {
    if (k == 1 || !n) return 1;
    if (n == d) return k;
    if (2*n == d) return root(k);
    if (4*n == d) return root(root(k));
    if (4*n == 3*d) return root(k)*root(root(k));
    /* Standard exposure subdivisions up to sixteenths; at most four iterations. */
    if (d<=16 && (d&(d-1))==0) {
        double result=1;
        for (unsigned bit=d/2;bit;bit>>=1) {
            k=root(k);
            if (n&bit) result*=k;
        }
        return result;
    }
    return power(k, (double)n/d);
}

/* Let T = inverse(B) A = q*z+h. Return T^t and T^t inverse(T).
   Both are calculated from the same relative transform before writing either row. */
static void interpolate(Pose a, Pose b, unsigned n, unsigned d, Pose *left, Pose *right) {
    double k = a.scale/b.scale, theta = a.angle-b.angle;
    if (theta > 180) theta -= 360;
    if (theta < -180) theta += 360;
    double t = (double)n/d;
    double r = scale_at(k,n,d);
    left->scale = r;
    left->angle = theta*t;
    right->scale = r/k;
    right->angle = left->angle-theta;
    Point delta = {a.shift.x-b.shift.x, a.shift.y-b.shift.y};
    if (delta.x==0 && delta.y==0) {
        left->shift = right->shift = delta;
        return;
    }
    Point h = quotient(delta, polar(b.scale, b.angle));
    Point q = polar(k,theta), u = polar(r,theta*t);
    Point factor = {t,0};
    if (n && n < d && (theta != 0 || k != 1)) {
        Point top = {u.x-1,u.y}, bottom = {q.x-1,q.y};
        if (__builtin_fabs(bottom.x)+__builtin_fabs(bottom.y)<1e-4) {
            /* Binomial expansion of ((1+w)^t-1)/w near pure translation. */
            Point square=product(bottom,bottom);
            double linear=t*(t-1)*.5, quadratic=linear*(t-2)/3;
            factor.x=t+linear*bottom.x+quadratic*square.x;
            factor.y=linear*bottom.y+quadratic*square.y;
        } else factor=quotient(top,bottom);
    }
    left->shift = product(factor,h);
    Point bh = product(quotient(u,q),h);
    right->shift.x = left->shift.x-bh.x;
    right->shift.y = left->shift.y-bh.y;
}
