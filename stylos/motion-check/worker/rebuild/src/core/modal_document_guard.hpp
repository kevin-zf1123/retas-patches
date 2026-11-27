#pragma once
#include "retas/document.hpp"
#include "retas/fill.hpp"
#include "adjust_render.hpp"
#include <bit>

namespace retas::modal_detail {
constexpr std::uint64_t max_bytes=512ull*1024*1024,max_work=128ull*1024*1024;
inline void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Modal document cancelled");}
inline void charge(std::uint64_t& n,std::uint64_t amount,std::uint64_t cap,const char* message){if(amount>cap-n)throw std::length_error(message);n+=amount;}
inline auto sessions(const Document& d){return std::array{d.has_text_preview(),d.has_floating_transform(),d.has_frame_move(),d.has_vanishing_point_move()};}
inline bool bits(double a,double b){return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);}
inline bool length(const Length& a,const Length& b){return bits(a.value,b.value)&&a.unit==b.unit&&bits(a.reference,b.reference);}
inline bool common(const CommonElement& a,const CommonElement& b){
    return a.width==b.width&&a.height==b.height&&a.depth==b.depth&&bits(a.resolution,b.resolution)&&a.name==b.name&&a.flags==b.flags&&a.color==b.color&&
        a.x==b.x&&a.y==b.y&&length(a.position[0],b.position[0])&&length(a.position[1],b.position[1])&&a.opacity==b.opacity&&a.opacity_mode==b.opacity_mode&&a.field_14==b.field_14&&a.suffix==b.suffix&&a.runtime_operable==b.runtime_operable;
}
inline bool raster_same(const Raster& a,const Raster& b){
    if(a.width!=b.width||a.height!=b.height||a.depth!=b.depth||!bits(a.resolution,b.resolution)||a.color_order!=b.color_order||a.tiles.size()!=b.tiles.size())return false;
    auto j=b.tiles.begin();for(const auto& [key,tile]:a.tiles){if(key!=j->first||!tile||!j->second||*tile!=*j->second)return false;++j;}return true;
}
inline bool data_same(const DocumentData& a,const DocumentData& b,unsigned depth=0){
    if(depth>8||a.version!=b.version||a.dga!=b.dga||a.current_layer!=b.current_layer||a.layers.size()!=b.layers.size()||a.preview!=b.preview||a.layer_suffix!=b.layer_suffix||a.trailing!=b.trailing||a.unrestored_external_content!=b.unrestored_external_content)return false;
    const auto& x=a.base;const auto& y=b.base;
    if(x.frame_mode!=y.frame_mode||!bits(x.scale,y.scale)||!bits(x.dpi,y.dpi)||!bits(x.dialog_dpi,y.dialog_dpi)||x.flags!=y.flags||x.suffix!=y.suffix)return false;
    for(std::size_t i=0;i<16;++i)if(!length(x.lengths[i],y.lengths[i]))return false;
    if(bool(a.original)!=bool(b.original)||(a.original&&*a.original!=*b.original)||bool(a.original_content)!=bool(b.original_content))return false;
    if(a.original_content&&!data_same(*a.original_content,*b.original_content,depth+1))return false;
    for(std::size_t i=0;i<a.layers.size();++i){const auto& l=a.layers[i];const auto& m=b.layers[i];
        if(l.opaque!=m.opaque||l.family!=m.family||!common(l.common,m.common)||l.specific!=m.specific||l.suffix!=m.suffix||l.plane_suffix!=m.plane_suffix||l.opaque_record!=m.opaque_record||l.current_plane!=m.current_plane||l.visibility_modified!=m.visibility_modified||l.runtime_vector_high_water!=m.runtime_vector_high_water||!vector_runtime_detail::face_colors_equal(l,m)||l.planes.size()!=m.planes.size())return false;
        for(std::size_t k=0;k<l.planes.size();++k){const auto& p=l.planes[k];const auto& q=m.planes[k];
            if(p.opaque!=q.opaque||p.family!=q.family||p.runtime_adjust_cache!=q.runtime_adjust_cache||!common(p.common,q.common)||p.specific!=q.specific||p.suffix!=q.suffix||p.opaque_record!=q.opaque_record||p.vanishing_modified!=q.vanishing_modified||p.frame_modified!=q.frame_modified||p.pixels_changed!=q.pixels_changed||p.raster_modified!=q.raster_modified||p.visibility_modified!=q.visibility_modified||bool(p.raster)!=bool(q.raster)||(p.raster&&!raster_same(*p.raster,*q.raster)))return false;}}
    return true;
}
inline void bytes_charge(std::uint64_t& n,const Bytes& b){charge(n,b.size(),max_bytes,"Modal document source bytes exceed512MiB");}
inline void common_charge(std::uint64_t& n,const CommonElement& c){bytes_charge(n,c.name);bytes_charge(n,c.suffix);}
inline void data_charge(const DocumentData& d,std::uint64_t& n,unsigned depth=0,bool include_shared_backing=true){
    if(depth>8)throw std::length_error("Modal document original-content nesting exceeds8");
    charge(n,sizeof(DocumentData),max_bytes,"Modal document source metadata exceeds512MiB");
    if(d.layers.size()>100000)throw std::length_error("Modal document layer count exceeds100000");
    charge(n,d.layers.size()*sizeof(Layer),max_bytes,"Modal document layer metadata exceeds512MiB");
    bytes_charge(n,d.base.suffix);bytes_charge(n,d.preview);bytes_charge(n,d.layer_suffix);bytes_charge(n,d.trailing);
    if(include_shared_backing){if(d.original)bytes_charge(n,*d.original);if(d.original_content)data_charge(*d.original_content,n,depth+1);}
    for(const auto& l:d.layers){common_charge(n,l.common);charge(n,vector_runtime_detail::face_colors_storage(l),max_bytes,"Runtime Face color backing exceeds512MiB");bytes_charge(n,l.specific);bytes_charge(n,l.suffix);bytes_charge(n,l.plane_suffix);bytes_charge(n,l.opaque_record);
        if(l.planes.size()>100000)throw std::length_error("Modal document plane count exceeds100000");
        charge(n,l.planes.size()*sizeof(Plane),max_bytes,"Modal document plane metadata exceeds512MiB");
        for(const auto& p:l.planes){common_charge(n,p.common);bytes_charge(n,p.specific);bytes_charge(n,p.suffix);bytes_charge(n,p.opaque_record);charge(n,adjust_render_detail::cache_storage(p),max_bytes,"Modal Adjust cache exceeds512MiB");
            if(p.raster){const auto& r=*p.raster;charge(n,r.tiles.size()*96ull,max_bytes,"Modal document tile metadata exceeds512MiB");
                for(const auto& [key,tile]:r.tiles){(void)key;if(!tile)throw std::invalid_argument("Modal document null raster tile");if(include_shared_backing)bytes_charge(n,*tile);}}}
    }
}
inline DocumentData frozen(const DocumentData& d,std::stop_token stop,unsigned depth=0,bool retain_text_caches=true){
    poll(stop);if(depth>8)throw std::length_error("Modal document original-content nesting exceeds8");DocumentData copy=d;
    // Keep the copied owning references alive through allocator reentry. A
    // reentrant source change is rejected by the caller's currency guard.
    const auto original=copy.original;const auto content=copy.original_content;
    if(original)copy.original=std::make_shared<const Bytes>(*original);
    if(content)copy.original_content=std::make_shared<const DocumentData>(frozen(*content,stop,depth+1,retain_text_caches));
    for(auto& l:copy.layers)for(auto& p:l.planes){
        if(!retain_text_caches)p.runtime_text_cache.reset();
        if(p.raster)for(auto& [key,tile]:p.raster->tiles){(void)key;poll(stop);if(!tile)throw std::invalid_argument("Modal document null raster tile");tile=std::make_shared<Bytes>(*tile);}}
    return copy;
}
inline bool selection_same(const RegionSelection& a,const RegionSelection& b,std::stop_token stop={}){
    if(a.state!=b.state||a.bounds!=b.bounds||a.origin!=b.origin||a.carrier_size!=b.carrier_size||!bits(a.carrier_dpi,b.carrier_dpi)||bool(a.mask)!=bool(b.mask))return false;if(!a.mask)return true;
    if(a.mask->width()!=b.mask->width()||a.mask->height()!=b.mask->height())return false;
    for(int y=0;y<a.mask->height();++y){poll(stop);for(int x=0;x<a.mask->width();++x)if(a.mask->test(x,y)!=b.mask->test(x,y))return false;}
    for(int y=0;y<(a.mask->height()+255)/256;++y){poll(stop);for(int x=0;x<(a.mask->width()+255)/256;++x)if(a.mask->block_present(x,y)!=b.mask->block_present(x,y))return false;}return true;
}

}
