#pragma once
#include "vector_material_geometry.hpp"
#include "retas/numeric.hpp"
#include <optional>
#include <map>
#include <cmath>

namespace retas::vector_copy_detail {
using Matrix=vector_material_detail::Matrix;
using numeric_detail::binary64;
inline int integer(double v){
    v=std::trunc(v);
    if(!std::isfinite(v)||std::abs(v)>134217727)throw std::invalid_argument("Vector copy coordinate exceeds bounded raw domain");
    return int(v);
}
inline void validate(const Matrix& m){
    for(double v:{m.sx,m.sy,m.tx,m.ty,m.xy,m.yx})if(!std::isfinite(v))throw std::invalid_argument("Nonfinite vector copy matrix");
}
inline std::array<double,2> point(double x,double y,const Matrix& m){
    return {binary64(binary64(binary64(x*m.sx)+binary64(y*m.xy))+m.tx),
            binary64(binary64(binary64(x*m.yx)+binary64(y*m.sy))+m.ty)};
}
inline std::uint32_t width(std::uint32_t bits,const std::optional<Matrix>& matrix){
    if(!matrix)return bits;
    const double scale=binary64(std::sqrt(binary64(binary64(matrix->sx*matrix->sx)+binary64(matrix->yx*matrix->yx))));
    const double value=binary64(double(std::bit_cast<float>(bits))*scale);
    if(!std::isfinite(value)||std::abs(value)>std::numeric_limits<float>::max())throw std::invalid_argument("Vector copied width exceeds finite F32");
    return std::bit_cast<std::uint32_t>(float(value));
}
// Ruler/Shape5CDEC0: F32 scale and angle stores precede length/angle use.
inline void extension(int& x,int& y,int& l1,int& l2,std::uint32_t& angle,const Matrix& m){
    validate(m);const double scale=binary64(std::sqrt(binary64(binary64(m.sx*m.sx)+binary64(m.yx*m.yx))));
    if(!std::isfinite(scale)||scale>std::numeric_limits<float>::max())throw std::invalid_argument("Invalid extension copy scale");
    const float f=float(scale);float a=std::bit_cast<float>(angle);if(!std::isfinite(a))throw std::invalid_argument("Invalid extension copy angle");
    const double deg=double(std::bit_cast<float>(0x42652ee0u));
    double theta=m.sx>0?binary64(std::atan(binary64(m.yx/m.sx))*deg):m.sx<0?binary64(binary64(std::atan(binary64(m.yx/m.sx))*deg)+180):m.yx<0?270:90;
    if(theta<0)theta=binary64(theta+360);const float delta=float(theta);
    a=binary64(binary64(m.sx*m.sy)-binary64(m.xy*m.yx))<0?delta-a:a+delta;
    if(a<0)a+=360.f;else if(a>=360)a-=360.f;
    const auto p=point(x,y,m);x=integer(p[0]);y=integer(p[1]);l1=integer(binary64(double(l1)*f));l2=integer(binary64(double(l2)*f));angle=std::bit_cast<std::uint32_t>(a);
}
inline VectorBounds bounds(VectorBounds b,const std::optional<Matrix>& matrix){
    if(!matrix)return b;
    validate(*matrix);
    const auto a=point(b.left,b.top,*matrix),c=point(b.right,b.top,*matrix),d=point(b.left,b.bottom,*matrix),e=point(b.right,b.bottom,*matrix);
    return {integer(std::min({a[0],c[0],d[0],e[0]})),integer(std::min({a[1],c[1],d[1],e[1]})),
            integer(std::max({a[0],c[0],d[0],e[0]})),integer(std::max({a[1],c[1],d[1],e[1]}))};
}
inline std::optional<Matrix> transform(std::array<int,2> delta,const std::optional<Matrix>& matrix={}){
    if(matrix){if(delta!=std::array<int,2>{})throw std::invalid_argument("Vector copy has competing matrix and translation");validate(*matrix);return matrix;}
    if(delta==std::array<int,2>{})return {};
    return Matrix{1,1,double(delta[0]),double(delta[1])};
}
// CVectorPlane63B950. The absent-matrix branch is observable for materials:
// even an identity texture reconstruction may change its stored F32 fields.
inline std::optional<Matrix> between(const CommonElement& source,double source_child,
                                    const CommonElement& target,double target_child){
    for(double dpi:{source.resolution,target.resolution,source_child,target_child})if(!std::isfinite(dpi)||dpi<=0)throw std::invalid_argument("Invalid vector copy owner/child DPI");
    if(source_child==target_child&&source.resolution==target.resolution){
        if(source.x==target.x&&source.y==target.y)return {};
        const int target_ratio=integer(binary64(target_child/target.resolution));
        return Matrix{1,1,double(integer((double(source.x)-target.x)*target_ratio)),double(integer((double(source.y)-target.y)*target_ratio))};
    }
    const int target_ratio=integer(binary64(target_child/target.resolution));
    const int source_ratio=integer(binary64(source_child/source.resolution));
    const double scale=binary64(target_child/source_child);
    const double tx=binary64(binary64(double(integer(double(source.x)*source_ratio))*scale)-integer(double(target.x)*target_ratio));
    const double ty=binary64(binary64(double(integer(double(source.y)*source_ratio))*scale)-integer(double(target.y)*target_ratio));
    Matrix m{scale,scale,tx,ty};validate(m);return m;
}
inline std::optional<Matrix> between_planes(const Layer& source,const Layer& target,const std::map<std::size_t,std::size_t>& planes){
    std::optional<Matrix> result;bool first=true;
    const auto equal=[](const std::optional<Matrix>& a,const std::optional<Matrix>& b){if(bool(a)!=bool(b))return false;if(!a)return true;return std::bit_cast<std::array<std::uint64_t,6>>(*a)==std::bit_cast<std::array<std::uint64_t,6>>(*b);};
    for(auto [from,to]:planes){const auto matrix=between(source.common,source.planes.at(from).common.resolution,target.common,target.planes.at(to).common.resolution);
        if(!first&&!equal(result,matrix))throw std::invalid_argument("Shared Paint copy matrix depends on unavailable native Plane pointer order");result=matrix;first=false;}
    return result;
}

}
