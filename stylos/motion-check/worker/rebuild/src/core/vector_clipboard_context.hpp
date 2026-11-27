#pragma once
#include "retas/vector_render.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace retas::vector_render_detail {
inline VectorPoint clipboard_transform(VectorPoint p,const std::array<double,6>& m){
    using numeric_detail::binary64;
    return {binary64(binary64(binary64(p.x*m[0])+binary64(p.y*m[1]))+m[2]),binary64(binary64(binary64(p.x*m[3])+binary64(p.y*m[4]))+m[5])};
}
inline int clipboard_integer(double x){
    if(!std::isfinite(x)||x<std::numeric_limits<int>::min()||x>std::numeric_limits<int>::max())throw std::invalid_argument("Clipboard matrix conversion overflow");return int(x);
}
inline VectorPoint clipboard_quantize(VectorPoint p,const VectorRenderContext& ctx,unsigned mode){
    const auto q=clipboard_transform(p,ctx.clipboard_matrix);
    const int x=clipboard_integer(std::floor(numeric_detail::binary64(q.x*mode))),y=clipboard_integer(std::floor(numeric_detail::binary64(q.y*mode)));
    // The router forms signed point differences and neighbor probes. Keep
    // them in the same bounded domain as ordinary non-affine contexts.
    if(std::abs(std::int64_t(x))>=1073741824||std::abs(std::int64_t(y))>=1073741824)
        throw std::invalid_argument("Affine vector point exceeds routing domain");
    return {double(x),double(y)};
}
inline int clipboard_material_coordinate(int sample,const VectorRenderContext& ctx){
    using numeric_detail::binary64;const auto reciprocal=binary64(1.0/ctx.base_scale),aa=binary64(double(ctx.oversample)*reciprocal),scale=binary64(1.0/aa);return clipboard_integer(std::floor(binary64(double(sample)*scale)));
}
inline VectorBounds clipboard_transform_bounds(VectorBounds b,const std::array<double,6>& matrix){
    // RCMatrix2D::MultiplyBound visits LT, RT, LB, RB in binary64, then
    // the integer overload truncates the four extrema toward zero.
    const auto lt=clipboard_transform({double(b.left),double(b.top)},matrix);
    const auto rt=clipboard_transform({double(b.right),double(b.top)},matrix);
    const auto lb=clipboard_transform({double(b.left),double(b.bottom)},matrix);
    const auto rb=clipboard_transform({double(b.right),double(b.bottom)},matrix);
    for(auto p:{lt,rt,lb,rb})if(!std::isfinite(p.x)||!std::isfinite(p.y))
        throw std::invalid_argument("Nonfinite affine vector bounds");
    return {clipboard_integer(std::min({lt.x,rt.x,lb.x,rb.x})),clipboard_integer(std::min({lt.y,rt.y,lb.y,rb.y})),
        clipboard_integer(std::max({lt.x,rt.x,lb.x,rb.x})),clipboard_integer(std::max({lt.y,rt.y,lb.y,rb.y}))};
}
inline VectorBounds clipboard_raw_block(VectorBounds block,const VectorRenderContext& ctx){
    auto raw=clipboard_transform_bounds(block,ctx.clipboard_inverse);
    const auto again=clipboard_transform_bounds(raw,ctx.clipboard_matrix);
    int halo=clipboard_integer(std::trunc(ctx.base_scale*(ctx.depth>=8?(ctx.oversample==4?.75:ctx.oversample==2?.5:0):0)));
    if(again.right<block.right||again.bottom<block.bottom)++halo;
    raw.right=clipboard_integer(double(raw.right)+halo);raw.bottom=clipboard_integer(double(raw.bottom)+halo);
    for(int edge:{raw.left,raw.top,raw.right,raw.bottom})if(std::abs(std::int64_t(edge))>=1073741824)
        throw std::invalid_argument("Affine vector block exceeds routing domain");
    return raw;
}
}
