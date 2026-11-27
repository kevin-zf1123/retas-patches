#include "retas/filter_tools.hpp"
#include "filter_tool_kernels.hpp"
#include "filter_dust.hpp"
#include "native_offscreen.hpp"
#include "modal_document_guard.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace retas {
namespace {
struct Area {
    int left{},top{},right{},bottom{};
    bool empty()const{return left>=right||top>=bottom;}
};
bool raster_family(std::uint32_t family){
    switch(family){
    case 0x10000001:case 0x11110001:case 0x11110002:case 0x11410001:
    case 0x11210001:case 0x11220001:case 0x11240001:case 0x12100001:case 0x12400001:return true;
    default:return false;
    }
}
FillResult reject(const char* message){FillResult result;result.handled=false;result.error=message;return result;}
FillResult cancelled(){FillResult result;result.cancelled=true;return result;}
Area clip(Rect rect,const Raster& raster){
    const auto x=std::int64_t(rect.x),y=std::int64_t(rect.y);
    return {int(std::clamp<std::int64_t>(x,0,raster.width)),int(std::clamp<std::int64_t>(y,0,raster.height)),
        int(std::clamp<std::int64_t>(x+std::max(0,rect.width),0,raster.width)),
        int(std::clamp<std::int64_t>(y+std::max(0,rect.height),0,raster.height))};
}
void validate(const Raster& raster){
    if(!raster_stroke_surface_supported(raster)||raster.width<=0||raster.height<=0||std::uint64_t(raster.width)*raster.height>128ull*1024*1024)throw std::invalid_argument("范围工具需要有效的 canonical 栅格。");
    for(const auto& [position,bytes]:raster.tiles)
        if(!bytes||bytes->size()!=raster.tile_bytes()||position.x<0||position.y<0||
           std::int64_t(position.x)*Raster::tile_side>=raster.width||
           std::int64_t(position.y)*Raster::tile_side>=raster.height)
            throw std::invalid_argument("范围工具的目标图块无效。");
}
struct Changes {
    std::size_t count{};
    int left{std::numeric_limits<int>::max()},top{std::numeric_limits<int>::max()},
        right{std::numeric_limits<int>::min()},bottom{std::numeric_limits<int>::min()};
    void add(int x,int y){++count;left=std::min(left,x);top=std::min(top,y);right=std::max(right,x);bottom=std::max(bottom,y);}
    FillResult result()const{
        FillResult value;value.changed=count!=0;value.changed_pixels=count;
        if(count)value.dirty={left,top,right-left+1,bottom-top+1};
        return value;
    }
};
FillResult blur(Raster& image,int mode,double radius,Area area,const BinaryMask* mask,std::stop_token stop,const std::optional<RasterSelection>& selection,
    filter_detail::FilterCosts* costs,bool retain_backing){
    // PaintMan host CFilterBlurGauss (658650), distinct from GaussBlur.re6:
    // truncate 8-bit weights; cap only the support, not the Gaussian radius.
    const int support=int(std::min(std::ceil(radius),127.0));
    if(costs){
        const auto width=std::uint64_t(area.right-area.left),height=std::uint64_t(area.bottom-area.top),temporary_width=width+2*support;
        const auto temporary_tiles=((temporary_width+255)/256)*((height+255)/256);
        const auto output_tiles=std::uint64_t((area.right-1)/256-area.left/256+1)*((area.bottom-1)/256-area.top/256+1);
        // Bound the dense temporary and every output tile that can acquire a
        // COW copy before allocating either surface. All siblings share costs.
        modal_detail::charge(costs->storage,(temporary_tiles+output_tiles)*(image.tile_bytes()+128)
            +image.tiles.size()*128+(std::uint64_t(support)+1)*sizeof(unsigned),modal_detail::max_bytes,"Gaussian scratch exceeds512MiB");
        modal_detail::charge(costs->work,(temporary_width+width)*height*(2*support+1),modal_detail::max_work,"Gaussian convolution exceeds128Mi work");
    }
    std::vector<unsigned> weights(std::size_t(support)+1);weights[0]=255;
    std::uint64_t sum=255;
    for(int i=1;i<=support;++i){
        const auto weight=std::clamp(255.0*std::exp(-std::log(255.0)*double(i)*i/(radius*radius)),0.0,255.0);
        weights[std::size_t(i)]=unsigned(weight);
        // 6588C0 doubles each floating tap before truncating its contribution
        // to the divisor. Twice the stored integer tap gives a different sum.
        sum+=unsigned(2*weight);
    }
    const auto sample=[&](const Raster& source,std::int64_t x,std::int64_t y,bool horizontal){
        std::array<std::uint64_t,4> lanes{};
        const int scale=source.depth==32?257:1,maximum=source.depth==32?255:65535;
        for(int delta=-support;delta<=support;++delta){
            // Fresh native offscreens are transparent white. Padding retains
            // that payload and its weight; there is no edge renormalization.
            const auto xx=std::int64_t(x)+(horizontal?delta:0),yy=std::int64_t(y)+(horizontal?0:delta);
            const auto p=xx>=0&&yy>=0&&xx<source.width&&yy<source.height?source.pixel(int(xx),int(yy)):Pixel{};
            const auto weight=weights[std::size_t(std::abs(delta))];
            lanes[0]+=std::uint64_t(p.r/scale)*weight;lanes[1]+=std::uint64_t(p.g/scale)*weight;
            lanes[2]+=std::uint64_t(p.b/scale)*weight;lanes[3]+=std::uint64_t(p.a/scale)*weight;
        }
        const auto lane=[&](std::uint64_t value){
            const auto quotient=value/sum;
            return std::uint16_t((quotient>=std::uint64_t(maximum-1)?maximum:int(quotient))*scale);
        };
        return Pixel{lane(lanes[0]),lane(lanes[1]),lane(lanes[2]),lane(lanes[3])};
    };
    const Raster source=image;
    const auto temporary_width=std::int64_t(area.right)-area.left+2*support;
    if(temporary_width>std::numeric_limits<int>::max())return reject("高斯模糊临时范围超出支持范围。");
    Raster vertical(int(temporary_width),area.bottom-area.top,source.depth,source.resolution);
    vertical.color_order=source.color_order;
    // Native 65A590 is the vertical first pass. Its padded scratch also
    // processes the image-exterior columns consumed by the horizontal pass.
    // In 64-bit mode even white padding changes when divisor > stored tap sum.
    for(int y=0;y<vertical.height;++y){
        if(stop.stop_requested())return cancelled();
        for(int x=0;x<vertical.width;++x){
            const auto sx=std::int64_t(area.left)-support+x,sy=std::int64_t(area.top)+y;
            auto p=sample(source,sx,sy,false);
            if(mode==4){const auto original=sx>=0&&sy>=0&&sx<source.width&&sy<source.height?source.pixel(int(sx),int(sy)):Pixel{};
                p.r=original.r;p.g=original.g;p.b=original.b;}
            if(p.r||p.g||p.b||p.a)vertical.set_pixel(x,y,p);
        }
    }
    Raster staged=source;Changes changes;
    for(int y=area.top;y<area.bottom;++y){
        if(stop.stop_requested())return cancelled();
        for(int x=area.left;x<area.right;++x){
            if(mask&&!mask->test(x,y))continue;
            if(selection){selection_detail::MaskView view{*selection,image.width,image.height};const auto offset=view.offset();const auto mx=std::int64_t(x)+offset[0],my=std::int64_t(y)+offset[1];if(mx<0||my<0||mx>=view.width()||my>=view.height()||!view.test(int(mx),int(my)))continue;}
            const auto vx=x-area.left+support,vy=y-area.top;
            auto p=sample(vertical,vx,vy,true);const auto original=source.pixel(x,y);
            if(mode==4){const auto center=vertical.pixel(vx,vy);p.r=center.r;p.g=center.g;p.b=center.b;}
            if((p.r||p.g||p.b||p.a)&&p!=original){staged.set_pixel(x,y,p);changes.add(x,y);}
        }
    }
    if(stop.stop_requested())return cancelled();
    if(changes.count){if(!retain_backing)staged.prune_empty();image=std::move(staged);}
    return changes.result();
}
}

namespace detail {
FillResult gaussian_region(Raster& image,int mode,double radius,Rect bounds,const BinaryMask* mask,std::stop_token stop,const std::optional<RasterSelection>& selection,
    filter_detail::FilterCosts* costs,bool retain_backing){
    // The host converts 2*ceil(radius) through signed __ftol2 before capping.
    // Reject its undefined conversion domain instead of reproducing overflow.
    if(!std::isfinite(radius)||radius<0||radius>double(std::numeric_limits<int>::max()/2-1))
        return reject("高斯模糊半径超出支持范围。");
    const auto area=clip(bounds,image);
    if(stop.stop_requested())return cancelled();
    if(area.empty())return {};
    if(image.depth<32){FillResult r;r.performed=true;r.dirty={area.left,area.top,area.right-area.left,area.bottom-area.top};return r;}
    auto result=blur(image,mode,radius,area,mask,stop,selection,costs,retain_backing);if(result.handled&&!result.cancelled)result.performed=true;return result;
}

}
namespace {
bool target(const Plane& plane,FilterToolKind kind){
    return plane.common.flags[1]&&raster_family(plane.family)&&(kind!=FilterToolKind::Blur||(plane.raster&&plane.raster->depth>=8));
}
bool valid_plane(const Plane& plane){
    return !plane.opaque&&plane.raster&&raster_stroke_surface_supported(*plane.raster)&&std::isfinite(plane.common.resolution)&&plane.common.resolution>0&&plane.common.resolution==plane.raster->resolution;
}
int edge(double value){
    value=std::trunc(value);if(!std::isfinite(value)||value<INT32_MIN||value>INT32_MAX)throw std::invalid_argument("Filter DPI geometry exceeds int32");return int(value);
}
Rect scale_rect(Rect rect,double ratio){
    const int x=edge(rect.x*ratio),y=edge(rect.y*ratio),right=edge((double(rect.x)+std::max(0,rect.width))*ratio),bottom=edge((double(rect.y)+std::max(0,rect.height))*ratio);
    if(std::int64_t(right)-x>INT32_MAX||std::int64_t(bottom)-y>INT32_MAX)throw std::invalid_argument("Filter DPI rectangle exceeds int32");return {x,y,right-x,bottom-y};
}
FillResult dust_tool(DocumentData& document,const FilterToolSettings& settings,bool metric,Rect work,
    const BinaryMask* mask,std::stop_token stop,const std::optional<RasterSelection>& selection,
    std::vector<filter_detail::DustBinding>* bound,filter_detail::FilterCosts* shared){
    const auto layer=std::size_t(document.current_layer);const auto& owner=document.layers[layer];
    struct Plan {std::size_t index;Rect roi;int size;};std::vector<Plan> plans;
    for(std::size_t i=0;i<owner.planes.size();++i){
        const auto& plane=owner.planes[i];if(!plane.common.flags[2]||!target(plane,FilterToolKind::Dust))continue;
        validate(*plane.raster);const auto ratio=plane.raster->resolution/owner.common.resolution;
        const auto physical=metric?numeric_detail::binary64(numeric_detail::binary64(settings.size_mm*plane.raster->resolution)*0x1.42850a142850ap-5):
            numeric_detail::binary64(numeric_detail::binary64(double(settings.size_px)/owner.common.resolution)*plane.raster->resolution);
        if(!std::isfinite(physical)||physical<0||physical>INT32_MAX/2)throw std::invalid_argument("Invalid converted Dust size");
        const auto size=metric?legacy_rt_double_to_int(physical):std::optional<int>(int(physical));if(!size)throw std::invalid_argument("Dust size exceeds int32");
        // Bind precedes the worker's page intersection, including empty ROI.
        plans.push_back({i,scale_rect(work,ratio),*size});
    }
    std::vector<filter_detail::DustBinding> prepared_bound;
    if(bound){prepared_bound.reserve(plans.size());for(const auto& plan:plans)prepared_bound.push_back({layer,plan.index,plan.roi});}
    auto staged=document;FillResult result;result.dirty=work;std::optional<Rect> damage;filter_detail::FilterCosts local;auto& costs=shared?*shared:local;
    for(const auto& plan:plans){
        if(stop.stop_requested())return cancelled();auto& plane=staged.layers[layer].planes[plan.index];auto& image=*plane.raster;plane.pixels_changed=true;
        const Raster* companion{};
        if(plane.family==0x12100001)for(const auto& p:staged.layers[layer].planes)if(p.family==0x11110002){
            if(p.common.flags[1]){if(!valid_plane(p))throw std::invalid_argument("Dust companion is not a decoded canonical raster");companion=&*p.raster;}break;
        }
        const auto ratio=image.resolution/owner.common.resolution,inverse=owner.common.resolution/image.resolution;std::vector<filter_detail::DustMask> masks;
        if(mask)masks.push_back({mask->width(),mask->height(),{},inverse,ratio,[mask](int x,int y){return mask->test(x,y);}});
        if(selection){selection_detail::MaskView view{*selection,owner.common.width,owner.common.height};masks.push_back({view.width(),view.height(),view.offset(),inverse,ratio,[view](int x,int y){return view.test(x,y);}});}
        auto current=filter_detail::dust_region(image,settings.dust_type,plan.size,plane.common.opacity_mode==5,plan.roi,masks,stop,companion,&costs).result;
        if(!current.handled||current.cancelled)return current;
        result.changed|=current.changed;result.storage_changed|=current.storage_changed;result.performed|=current.performed;result.changed_pixels+=current.changed_pixels;
        if(current.performed){
            auto r=scale_rect(current.dirty,owner.common.resolution/image.resolution);
            if(r.width==INT32_MAX||r.height==INT32_MAX||std::int64_t(r.x)+r.width+1>INT32_MAX||std::int64_t(r.y)+r.height+1>INT32_MAX)throw std::invalid_argument("Dust damage exceeds int32");
            ++r.width;++r.height;
            if(!damage)damage=r;
            else{const auto right=std::max(std::int64_t(damage->x)+damage->width,std::int64_t(r.x)+r.width),bottom=std::max(std::int64_t(damage->y)+damage->height,std::int64_t(r.y)+r.height);damage->x=std::min(damage->x,r.x);damage->y=std::min(damage->y,r.y);if(right-damage->x>INT32_MAX||bottom-damage->y>INT32_MAX)throw std::invalid_argument("Dust damage union exceeds int32");damage->width=int(right-damage->x);damage->height=int(bottom-damage->y);}
        }
    }
    if(damage){if(damage->x==INT32_MIN||damage->y==INT32_MIN||std::int64_t(damage->x)+damage->width>=INT32_MAX||std::int64_t(damage->y)+damage->height>=INT32_MAX||damage->width>INT32_MAX-2||damage->height>INT32_MAX-2)throw std::invalid_argument("Dust damage inflation exceeds int32");--damage->x;--damage->y;damage->width+=2;damage->height+=2;result.dirty=*damage;}
    if(stop.stop_requested())return cancelled();
    for(const auto& plan:plans){auto& output=document.layers[layer].planes[plan.index];auto& prepared=staged.layers[layer].planes[plan.index];output.raster=std::move(prepared.raster);output.pixels_changed=prepared.pixels_changed;}
    if(bound)*bound=std::move(prepared_bound);
    return result;
}
}
bool can_apply_filter_tool(const DocumentData& document,FilterToolKind kind){
    if(kind!=FilterToolKind::Blur&&kind!=FilterToolKind::Dust)return false;
    if(document.current_layer<0||document.current_layer>=int(document.layers.size()))return false;
    const auto& owner=document.layers[std::size_t(document.current_layer)];const auto* active=document.active_plane();
    if(owner.opaque||!owner.common.flags[1]||!active)return false;
    if(!std::isfinite(document.base.dpi)||document.base.dpi<=0||!std::isfinite(owner.common.resolution)||owner.common.resolution<=0||owner.common.width<=0||owner.common.height<=0)return false;
    if(kind==FilterToolKind::Dust){
        if(!owner.common.runtime_operable||active->common.opacity_mode==4)return false;
        bool eligible{};
        for(const auto& plane:owner.planes)if(plane.common.flags[2]&&target(plane,kind)){
            if(!valid_plane(plane))return false;
            eligible|=plane.common.flags[0]!=1;
        }
        return eligible;
    }
    if((owner.family&0x40000000u)||!target(*active,kind)||!valid_plane(*active))return false;
    for(std::size_t i=0;i<owner.planes.size();++i)if((int(i)==owner.current_plane||owner.planes[i].common.flags[2])&&target(owner.planes[i],kind)&&!valid_plane(owner.planes[i]))return false;
    return true;
}
FillResult filter_detail::dust_tool_data(DocumentData& document,const FilterToolSettings& settings,bool metric,
    const std::optional<Rect>& bounds,const BinaryMask* mask,std::stop_token stop,const std::optional<RasterSelection>& selection,
    std::vector<DustBinding>* bound,FilterCosts* costs){
    try{
        if(stop.stop_requested())return cancelled();
        if(!can_apply_filter_tool(document,FilterToolKind::Dust))return reject("Dust requires a marked canonical raster target");
        const auto& owner=document.layers[std::size_t(document.current_layer)];
        if(mask&&(mask->width()!=owner.common.width||mask->height()!=owner.common.height))return reject("Dust range mask dimensions differ from its owner");
        if(selection&&!selection->has_materialized_mask())return reject("Dust selection has no mask backing");
        if((metric&&(!std::isfinite(settings.size_mm)||settings.size_mm<0.1||settings.size_mm>2))||(!metric&&(settings.size_px<1||settings.size_px>20))||settings.dust_type<0||settings.dust_type>2)return reject("Invalid active Dust settings");
        return dust_tool(document,settings,metric,bounds.value_or(Rect{0,0,owner.common.width,owner.common.height}),mask,stop,selection,bound,costs);
    }catch(const std::exception& error){return reject(error.what());}
}

FillResult apply_filter_tool(DocumentData& document,FilterToolKind kind,const FilterToolSettings& settings,
    bool metric,const std::optional<Rect>& bounds,const BinaryMask* mask,std::stop_token stop,const std::optional<RasterSelection>& selection){
    if(kind==FilterToolKind::Dust)return filter_detail::dust_tool_data(document,settings,metric,bounds,mask,stop,selection);
    try{
        if(!can_apply_filter_tool(document,kind))return reject("范围工具需要已恢复的可见 canonical 光栅平面。");
        const auto& owner=document.layers[std::size_t(document.current_layer)];
        if(mask&&(mask->width()!=owner.common.width||mask->height()!=owner.common.height))return reject("范围遮罩与图层尺寸不一致。");
        if(selection&&!selection->has_materialized_mask())return reject("范围选区缺少实际遮罩。");
        if(stop.stop_requested())return cancelled();
        constexpr double maximum_mm=10.0;constexpr int maximum_px=100;
        if((metric&&(!std::isfinite(settings.size_mm)||settings.size_mm<0.1||settings.size_mm>maximum_mm))||(!metric&&(settings.size_px<1||settings.size_px>maximum_px)))return reject("范围工具尺寸超出可用范围。");
        const auto work=bounds.value_or(Rect{0,0,owner.common.width,owner.common.height});
        struct Plan {std::size_t index;Area area;double size;};std::vector<Plan> plans;std::uint64_t remaining=128ull*1024*1024;
        for(std::size_t i=0;i<owner.planes.size();++i){
            const auto& plane=owner.planes[i];if((int(i)!=owner.current_plane&&!plane.common.flags[2])||!target(plane,kind))continue;
            const auto& source=*plane.raster;validate(source);const auto area=clip(work,source);if(area.empty())continue;
            const double physical=metric?settings.size_mm*source.resolution/25.4:double(settings.size_px);
            if(!std::isfinite(physical)||physical<0||physical>INT32_MAX/2)throw std::invalid_argument("范围工具换算后的尺寸无效。");
            const auto cost=std::uint64_t(area.right-area.left+2*std::min(127.0,std::ceil(physical)))*(area.bottom-area.top)*std::uint64_t(2*std::min(127.0,std::ceil(physical))+2);
            if(cost>remaining)throw std::invalid_argument("Filter operation exceeds bounded work budget");remaining-=cost;plans.push_back({i,area,physical});
        }
        auto staged=document;FillResult result;
        for(const auto& plan:plans){
            if(stop.stop_requested())return cancelled();auto& plane=staged.layers[std::size_t(staged.current_layer)].planes[plan.index];
            const auto rect=Rect{plan.area.left,plan.area.top,plan.area.right-plan.area.left,plan.area.bottom-plan.area.top};
            const auto current=detail::gaussian_region(*plane.raster,plane.common.opacity_mode,plan.size,rect,mask,stop,selection);
            if(!current.handled||current.cancelled)return current;
            result.changed|=current.changed;result.performed|=current.performed;result.storage_changed|=current.storage_changed;result.changed_pixels+=current.changed_pixels;
            if(current.changed||current.storage_changed)plane.pixels_changed=true;
        }
        if(stop.stop_requested())return cancelled();
        if(result.changed||result.storage_changed)for(const auto& plan:plans){auto& output=document.layers[std::size_t(document.current_layer)].planes[plan.index];auto& prepared=staged.layers[std::size_t(staged.current_layer)].planes[plan.index];output.raster=std::move(prepared.raster);output.pixels_changed=prepared.pixels_changed;}
        if(result.performed)result.dirty=work;
        return result;
    }catch(const std::exception& error){return reject(error.what());}
}

}
