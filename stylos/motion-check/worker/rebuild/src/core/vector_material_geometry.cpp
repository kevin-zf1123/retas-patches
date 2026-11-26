#include "vector_material_geometry.hpp"
#include "retas/numeric.hpp"
#include <cmath>
#include <limits>

namespace retas::vector_material_detail {
namespace {
using numeric_detail::binary64;
constexpr double rad=0x1.1df46a2529d39p-6;
constexpr double deg=double(std::bit_cast<float>(0x42652ee0u));
struct Point {double x,y;};
int integer(double value){const auto v=std::trunc(value);if(!std::isfinite(v)||v<std::numeric_limits<int>::min()||v>std::numeric_limits<int>::max())throw std::overflow_error("Vector material coordinate overflow");return int(v);}
std::uint32_t float_bits(double value){
    if(!std::isfinite(value)||std::abs(value)>std::numeric_limits<float>::max())throw std::overflow_error("Vector material scalar exceeds finite F32");
    return std::bit_cast<std::uint32_t>(static_cast<float>(value));
}
double finite(std::uint32_t bits){const double v=std::bit_cast<float>(bits);if(!std::isfinite(v))throw std::invalid_argument("Non-finite active vector material scalar");return v;}
void validate(const Matrix& m){if(!std::isfinite(m.sx)||!std::isfinite(m.sy)||!std::isfinite(m.xy)||!std::isfinite(m.yx)||!std::isfinite(m.tx)||!std::isfinite(m.ty))throw std::invalid_argument("Invalid vector material affine matrix");}
Point vector(Point p,const Matrix& m){return {m.xy==0?binary64(p.x*m.sx):binary64(binary64(p.x*m.sx)+binary64(p.y*m.xy)),m.yx==0?binary64(p.y*m.sy):binary64(binary64(p.x*m.yx)+binary64(p.y*m.sy))};}
Point point(Point p,const Matrix& m){p=vector(p,m);return {binary64(p.x+m.tx),binary64(p.y+m.ty)};}
double length_squared(Point p){return binary64(binary64(p.x*p.x)+binary64(p.y*p.y));}
double angle(Point p){
    // Host605AF0 uses its rounded F32 degree multiplier and explicit quadrants.
    // The native FPATAN/FSIN/FCOS path is represented by host libm here.
    if(p.x>0){const double a=binary64(std::atan(binary64(p.y/p.x))*deg);return a<0?binary64(a+360):a;}
    if(p.x<0)return binary64(binary64(std::atan(binary64(p.y/p.x))*deg)+180);
    return p.y<0?270:90;
}
Point trig(double degrees){const double theta=binary64(degrees*rad);
    if(!std::isfinite(theta)||std::abs(theta)>=0x1p63)throw std::invalid_argument("Vector material rotation exceeds native trig domain");
    return {binary64(std::cos(theta)),binary64(std::sin(theta))};
}
void coordinates(VectorArchiveGradation& g,Point a,Point b){g.start_x=integer(a.x);g.start_y=integer(a.y);g.end_x=integer(b.x);g.end_y=integer(b.y);}
}
void transform(VectorArchiveGradation& g,const Matrix& m){
    validate(m);if(g.start_x==g.end_x&&g.start_y==g.end_y)return; // Host599C90: entire tail, including origin, stays unchanged.
    const Point origin{double(g.start_x),double(g.start_y)},a=point(origin,m);
    if(g.flags&1){
        const Point d{double(g.end_x)-g.start_x,double(g.end_y)-g.start_y};
        const double radius=binary64(std::sqrt(length_squared(d)));
        const Point normal=vector({-binary64(d.y/radius),binary64(d.x/radius)},m),delta=vector(d,m);
        const double n2=length_squared(normal);if(n2==0)return;
        const double inv=binary64(1.0/std::sqrt(n2));const Point w{binary64(inv*normal.y),-binary64(inv*normal.x)};
        const double projection=binary64(binary64(w.y*delta.y)+binary64(w.x*delta.x));
        coordinates(g,a,{binary64(binary64(w.x*projection)+a.x),binary64(binary64(w.y*projection)+a.y)});
    }else{
        // Native radius subtracts signed DWORDs before converting to floating
        // point. Exclude its wrap domain rather than silently widening it.
        const auto dx=std::int64_t(g.start_x)-g.end_x,dy=std::int64_t(g.start_y)-g.end_y;
        if(dx<std::numeric_limits<int>::min()||dx>std::numeric_limits<int>::max()||dy<std::numeric_limits<int>::min()||dy>std::numeric_limits<int>::max())throw std::overflow_error("Radial material axis subtraction overflow");
        const double radius=binary64(std::sqrt(length_squared({double(dx),double(dy)})));
        const double minor=binary64(finite(g.minor_ratio_bits)*radius);
        const double axis=std::bit_cast<float>(float_bits(angle({double(g.end_x)-g.start_x,double(g.end_y)-g.start_y})));
        const auto rotation=trig(axis);Point maximum{};double max2{},min2{};float theta{};
        // Host599FF1 samples 36 angles. Strict comparisons retain the first
        // equal maximum. This is not an analytic ellipse/SVD reconstruction.
        for(int i=0;i<36;++i){const auto t=trig(theta);const double u=binary64(t.x*radius),v=binary64(t.y*minor);
            const Point sample{binary64(binary64(binary64(u*rotation.x)-binary64(v*rotation.y))+origin.x),binary64(binary64(binary64(u*rotation.y)+binary64(v*rotation.x))+origin.y)};
            const Point p=point(sample,m);const double distance=length_squared({binary64(p.x-a.x),binary64(p.y-a.y)});
            if(!std::isfinite(distance))throw std::overflow_error("Radial material sample overflow");
            if(i==0){max2=min2=distance;maximum=p;}else if(distance>max2){max2=distance;maximum=p;}else if(distance<min2)min2=distance;
            theta=static_cast<float>(theta+5.0f);
        }
        if(max2>0){coordinates(g,a,maximum);g.minor_ratio_bits=float_bits(binary64(std::sqrt(binary64(min2/max2))));}
    }
}
void transform(VectorArchiveTexture& t,const Matrix& m){
    validate(m);const auto origin=point({double(t.origin_x),double(t.origin_y)},m);t.origin_x=integer(origin.x);t.origin_y=integer(origin.y);
    const double sx=finite(t.scale_x_bits),sy=finite(t.scale_y_bits),h=finite(t.shear_x_bits);const auto rotation=trig(finite(t.rotation_bits));
    const Point u{binary64(sx*rotation.x),binary64(sx*rotation.y)};
    const Point v{binary64(binary64(h*u.x)-binary64(sy*rotation.y)),binary64(binary64(u.y*h)+binary64(sy*rotation.x))};
    const auto x=vector(u,m),y=vector(v,m);const double x2=length_squared(x),y2=length_squared(y);
    if(!std::isfinite(x2)||!std::isfinite(y2))throw std::overflow_error("Vector texture basis overflow");
    if(x2==0||y2==0)return; // Origin changed, all four old scalar bit fields remain.
    const double lx=binary64(std::sqrt(x2)),ly=binary64(std::sqrt(y2));
    const double cosine=binary64(binary64(binary64(y.x*x.x)+binary64(y.y*x.y))/binary64(ly*lx));
    const double sine=binary64(std::sqrt(binary64(1.0-binary64(cosine*cosine))));
    // Preserve the native absence of clamping/reflection signs. A non-finite
    // result rejects the staged Document instead of publishing an invalid tail.
    t.scale_x_bits=float_bits(lx);t.rotation_bits=float_bits(angle(x));t.scale_y_bits=float_bits(binary64(sine*ly));
    if(sine!=0)t.shear_x_bits=float_bits(binary64(cosine/sine));
}
}
