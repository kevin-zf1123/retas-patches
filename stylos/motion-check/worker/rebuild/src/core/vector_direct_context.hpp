#pragma once
#include "retas/vector_render.hpp"
#include "retas/numeric.hpp"
namespace retas::vector_render_detail {
inline VectorRenderContext direct_output_context(const Layer& layer,Rect page,double dpi,int depth,unsigned samples){
    const double scale=numeric_detail::binary64(numeric_detail::binary64(layer.common.resolution/dpi)*16);
    if(!std::isfinite(scale)||scale<=0)throw std::invalid_argument("Direct vector reference DPI scale exceeds bounds");
    VectorRenderContext context{page.width,page.height,depth,dpi,scale,{},std::nullopt,samples};
    if(layer.common.x||layer.common.y){
        //Native Transfer multiplies saved pixel XY by16 in signed int32.
        const auto x=std::int64_t(layer.common.x)*16,y=std::int64_t(layer.common.y)*16;
        if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX)throw std::invalid_argument("Direct vector reference raw XY overflows int32");
        const auto k=numeric_detail::binary64(1/scale),tx=numeric_detail::binary64(double(x)*k),ty=numeric_detail::binary64(double(y)*k);
        const auto det=numeric_detail::binary64(k*k);
        if(!std::isfinite(det)||det<=0)throw std::invalid_argument("Direct vector reference matrix determinant exceeds bounds");
        const auto inverse=numeric_detail::binary64(1/det),diagonal=numeric_detail::binary64(k*inverse);
        context.clipboard_affine=true;context.origin={-tx,-ty};context.clipboard_matrix={k,0,tx,0,k,ty};
        context.clipboard_inverse={diagonal,-0.0,numeric_detail::binary64(numeric_detail::binary64(-tx*k)*inverse),-0.0,diagonal,numeric_detail::binary64(numeric_detail::binary64(-ty*k)*inverse)};
        for(auto v:context.clipboard_matrix)if(!std::isfinite(v))throw std::invalid_argument("Direct vector reference matrix exceeds bounds");
        for(auto v:context.clipboard_inverse)if(!std::isfinite(v))throw std::invalid_argument("Direct vector reference inverse exceeds bounds");
    }
    return context;
}
}
