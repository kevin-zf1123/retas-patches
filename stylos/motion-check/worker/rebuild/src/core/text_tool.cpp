#include "retas/text_tool.hpp"
#include "retas/document.hpp"
#include "text_font_backend_internal.hpp"
#include <cmath>
namespace retas {
const Raster* text_raster_cache(const Plane& p) noexcept {
    const auto& cache=p.runtime_text_cache;
    return !p.opaque&&p.family==0x40000004&&cache&&!cache->image_geometry&&cache->specific==p.specific&&cache->source_width==p.common.width&&cache->source_height==p.common.height&&cache->source_dpi==p.common.resolution?&cache->raster:nullptr;
}
const Raster* text_raster_cache(const Layer& owner,const Plane& p,double dpi) noexcept {
    const auto* image=text_raster_cache(p);if(!image||image->resolution!=dpi)return nullptr;
    if(p.runtime_text_cache->static_geometry&&
       ((owner.family!=0x40000800&&owner.family!=0x48040800)||owner.opaque||!std::isfinite(owner.common.resolution)||owner.common.resolution<=0||p.common.resolution!=dpi
        ||owner.common.opacity!=100||p.raster))return nullptr;
    if(p.runtime_text_cache->static_geometry){const auto& bounds=p.runtime_text_cache->coordinate_bounds;
        if(std::int64_t(owner.common.x)+bounds[0]<INT32_MIN||std::int64_t(owner.common.y)+bounds[1]<INT32_MIN
           ||std::int64_t(owner.common.x)+bounds[2]>INT32_MAX||std::int64_t(owner.common.y)+bounds[3]>INT32_MAX)return nullptr;}
    return image;
}
const text_font_detail::ExportMasks* text_font_detail::display_masks(const Layer& owner,const Plane& p,double dpi) noexcept {
    const auto* image=text_raster_cache(p);if(!image||image->resolution!=dpi)return nullptr;
    const auto& cache=*p.runtime_text_cache;
    if(!cache.static_geometry||!cache.masks||(owner.family!=0x40000800&&owner.family!=0x48040800)||owner.opaque
       ||!std::isfinite(owner.common.resolution)||owner.common.resolution<=0||p.common.resolution!=dpi
       ||owner.common.opacity<0||owner.common.opacity>100||p.raster)return nullptr;
    const auto& bounds=cache.coordinate_bounds;
    if(std::int64_t(owner.common.x)+bounds[0]<INT32_MIN||std::int64_t(owner.common.y)+bounds[1]<INT32_MIN
       ||std::int64_t(owner.common.x)+bounds[2]>INT32_MAX||std::int64_t(owner.common.y)+bounds[3]>INT32_MAX)return nullptr;
    return cache.masks.get();
}
const text_font_detail::ExportMasks* text_font_detail::image_masks(const Layer& owner,const Plane& p,double dpi) noexcept {
    const auto& cache=p.runtime_text_cache;if(!cache||!cache->image_geometry)return display_masks(owner,p,dpi);
    if(p.opaque||p.family!=0x40000004||!cache->static_geometry||!cache->masks||cache->specific!=p.specific
       ||cache->source_width!=p.common.width||cache->source_height!=p.common.height||cache->source_dpi!=p.common.resolution
       ||cache->raster.resolution!=dpi||!std::isfinite(p.common.resolution)||p.common.resolution<=0
       ||(owner.family!=0x40000800&&owner.family!=0x48040800)||owner.opaque||p.raster
       ||!std::isfinite(owner.common.resolution)||owner.common.resolution<=0||owner.common.opacity<0||owner.common.opacity>100
       ||cache->source_owner_origin!=std::array<int,2>{owner.common.x,owner.common.y})return nullptr;
    return cache->masks.get();
}
bool text_font_detail::requires_native_image(const DocumentData& data) noexcept {
    for(const auto& owner:data.layers)for(const auto& p:owner.planes)
        if(p.runtime_text_cache&&p.runtime_text_cache->static_geometry&&(p.runtime_text_cache->image_geometry||owner.common.opacity!=100))return true;
    return false;
}

TextParameters decode_text_parameters(std::span<const std::uint8_t> bytes){
    Reader r(bytes);TextParameters p;p.text=r.string();p.font_name=r.string();p.font_size=r.i32();
    p.text_color=r.u32();p.line_color=r.u32();p.line_enabled=r.i32();
    for(auto& n:p.text_anchor)n=r.i32();for(auto& n:p.line_anchor)n=r.i32();p.suffix=r.rest();return p;
}
Bytes encode_text_parameters(const TextParameters& p){
    Writer w;w.string(p.text);w.string(p.font_name);w.i32(p.font_size);w.u32(p.text_color);w.u32(p.line_color);w.i32(p.line_enabled);
    for(auto n:p.text_anchor)w.i32(n);for(auto n:p.line_anchor)w.i32(n);w.raw(p.suffix);return std::move(w.data);
}
}
