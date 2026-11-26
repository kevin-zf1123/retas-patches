#pragma once
#include "vector_probe.hpp"
#include "retas/numeric.hpp"
#include <cmath>
#include <algorithm>
#include <stdexcept>

namespace retas::vector_menu_scope_detail {
//5C76E0 retains GetExtentRect when SDK IntersectRect returns false: a
//disjoint, touching or degenerate clip does not replace the existing extent.
//Bind this menu profile explicitly; other Scope consumers keep their own clip
//policy. Both the extent scan and mutation use the caller's shared work ledger.
inline vector_probe_detail::Scope mask(const BinaryMask& actual,
    std::array<std::int32_t,2> raw_origin,Rect clip,vector_probe_detail::Budget budget,double forward=16,double bitmap_dpi=0){
    if(clip.width<0||clip.height<0)throw std::invalid_argument("Vector menu mask clip is invalid");
    auto scope=vector_probe_detail::Scope::masked(actual,forward,raw_origin,std::nullopt,budget);
    scope.bitmap_dpi=bitmap_dpi;
    const auto& extent=scope.mask_bounds;
    const auto left=std::max<std::int64_t>(extent.x,clip.x),top=std::max<std::int64_t>(extent.y,clip.y);
    const auto right=std::min<std::int64_t>(std::int64_t(extent.x)+extent.width,std::int64_t(clip.x)+clip.width);
    const auto bottom=std::min<std::int64_t>(std::int64_t(extent.y)+extent.height,std::int64_t(clip.y)+clip.height);
    if(left<right&&top<bottom){
        scope.mask_bounds={int(left),int(top),int(right-left),int(bottom-top)};
        //The positive intersection is a subset of the already checked extent;
        //all translated 16x edges therefore remain in its signed32 range.
        const auto edge=[&](std::int64_t n,int axis){return int(numeric_detail::binary64(std::floor(numeric_detail::binary64(double(n)*forward))+raw_origin[axis]));};
        scope.bounds={edge(left,0),edge(top,1),edge(right,0),edge(bottom,1)};
    }
    return scope;
}
inline vector_probe_detail::Scope selection(const RegionSelection& s,std::array<std::int64_t,2> delta,Rect clip,double numerator,double bitmap_dpi,vector_probe_detail::Budget budget){
    if(!s.mask||!std::isfinite(numerator)||numerator<=0||!std::isfinite(bitmap_dpi)||bitmap_dpi<=0)throw std::invalid_argument("Vector menu actual mask DPI is invalid");
    if(s.carrier_size!=std::array<int,2>{}&&s.carrier_size!=std::array<int,2>{s.mask->width(),s.mask->height()})throw std::invalid_argument("Vector menu mask size differs from actual carrier");
    const double scale=numeric_detail::binary64(numeric_detail::binary64(numerator/bitmap_dpi)*16);
    std::array<int,2> offset;for(int i=0;i<2;++i){const double v=std::floor(numeric_detail::binary64(double(delta[i])*scale));if(!std::isfinite(v)||std::abs(v)>134217727)throw std::invalid_argument("Vector menu offset exceeds bounded raw domain");offset[i]=int(v);}
    return mask(*s.mask,offset,clip,budget,scale,bitmap_dpi);
}

}
