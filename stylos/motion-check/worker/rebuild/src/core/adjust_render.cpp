#include "adjust_render.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace retas::adjust_render_detail {
namespace {
constexpr std::uint64_t max_storage=512ull*1024*1024,max_work=128ull*1024*1024;
void charge(std::uint64_t& total,std::uint64_t amount,std::uint64_t limit){
    if(total>limit||amount>limit-total)throw std::length_error("Adjust rendering exceeds cumulative limits");
    total+=amount;
}
Tables identity(){Tables result;for(auto& c:result)for(std::size_t i=0;i<256;++i)c[i]=std::uint8_t(i);return result;}
void compose(Tables& destination,const Tables& next){for(std::size_t c=0;c<3;++c)for(auto& value:destination[c])value=next[c][value];}
bool unsafe_helper(std::uint32_t kind){
    switch(kind){case 8:case 0x10:case 0x20:case 0x40:case 0x80:case 0x100:case 0x200:case 0x1000:case 0x2000:case 0x4000:return true;default:return false;}
}
Tables plane_tables(const Plane& plane,bool active,std::uint64_t& work){
    if(plane.opaque||plane.family!=0x40000010||plane.raster)throw std::invalid_argument("Invalid Adjust plane");
    Reader wrapper(plane.specific);const auto kind=wrapper.u32();
    // A null factory has no child section. Unknown nonzero factories skip
    // one bounded section. Neither replaces the ctor's BYTE identity.
    if(!kind)return identity();
    auto r=wrapper.section();
    if(!active)return identity();
    if(plane.runtime_adjust_cache&&plane.runtime_adjust_cache->specific==plane.specific)return plane.runtime_adjust_cache->tables;
    if(unsafe_helper(kind))throw std::invalid_argument("Active Adjust helper has no safe native RGB table profile");
    if(kind==0x8000){(void)r.i32();(void)r.f64();(void)r.i32();return identity();}
    if(kind!=1&&kind!=2&&kind!=4)return identity();
    FilterKind filter;FilterSettings settings;
    if(kind==1){
        filter=FilterKind::Contrast;auto contrast=r.i32(),brightness=r.i32();
        // Each native stage independently returns no-write outside its range.
        if(contrast < -127||contrast > 127)contrast=0;
        if(brightness < -127||brightness > 127)brightness=0;
        settings=ContrastSettings{contrast,brightness};charge(work,256*4,max_work);
    }
    else if(kind==2){
        filter=FilterKind::Level;LevelSettings value;const auto count=r.i32();r.require_count(count,28,4);
        for(int i=0;i<count;++i){auto c=r.section();value.channels[i]={c.i32(),c.i32(),c.i32(),c.i32(),c.f64()};}
        settings=value;charge(work,256*12,max_work);
    }else{
        filter=FilterKind::ToneCurve;ToneCurveSettings value;value.interpolation=r.i32();const auto count=r.i32();r.require_count(count,4,4);
        for(int i=0;i<count;++i){const auto n=r.i32();r.require_count(n,8,32);auto& points=value.curves[i].points;points.clear();for(int j=0;j<n;++j)points.push_back({r.i32(),r.i32()});}
        for(const auto& curve:value.curves){const auto n=std::uint64_t(curve.points.size());charge(work,256+(value.interpolation?1024*n*n:33*n*16),max_work);}
        settings=std::move(value);
    }
    return filter_detail::byte_rgb_tables(filter,settings);
}
}
std::uint64_t cache_storage(const Plane& p) noexcept{return p.runtime_adjust_cache?sizeof(TableCache)+p.runtime_adjust_cache->specific.size():0;}
std::shared_ptr<const TableCache> fresh_cache(const Plane& p){
    if(p.specific.size()>max_storage-sizeof(TableCache))throw std::length_error("Adjust cache exceeds512MiB");
    return std::make_shared<const TableCache>(TableCache{identity(),p.specific});
}
std::shared_ptr<const TableCache> retain_cache(const Plane& p,const Bytes& next){
    if(!p.runtime_adjust_cache||p.runtime_adjust_cache->specific!=p.specific)return {};
    if(next==p.specific)return p.runtime_adjust_cache;
    if(next.size()>max_storage-sizeof(TableCache))throw std::length_error("Adjust cache exceeds512MiB");
    return std::make_shared<const TableCache>(TableCache{p.runtime_adjust_cache->tables,next});
}
Binding bind(const DocumentData& data,std::size_t layer,const Plane& p,const RegionSelection& selection,std::stop_token stop){
    if(stop.stop_requested())throw std::runtime_error("Adjust binding cancelled");
    const auto& owner=data.layers.at(layer);
    if(owner.opaque||owner.family!=0x40000200||p.opaque||p.family!=0x40000010||p.raster)throw std::invalid_argument("Adjust binding requires an attached Adjust plane");
    if(p.specific.size()>max_storage-sizeof(TableCache))throw std::length_error("Adjust binding exceeds512MiB");
    Reader wrapper(p.specific);const auto kind=wrapper.u32();if(kind)(void)wrapper.section();
    const auto current=p.runtime_adjust_cache&&p.runtime_adjust_cache->specific==p.specific?p.runtime_adjust_cache:std::shared_ptr<const TableCache>{};
    if(kind==0x200){
        const auto state=int(selection.state);if(state<0||state>4)throw std::invalid_argument("Invalid Adjust selection state");
        const auto size=selection_carrier_size(selection,data.base.width(),data.base.height());
        const auto dpi=p.common.resolution,owner_dpi=owner.common.resolution;
        if(!std::isfinite(dpi)||dpi<=0||!std::isfinite(owner_dpi)||owner_dpi<=0)throw std::invalid_argument("Invalid Adjust binding DPI");
        const auto edge=[&](std::int64_t n){const auto v=double(n)*(dpi/owner_dpi);if(!std::isfinite(v)||v<std::numeric_limits<int>::min()||v>std::numeric_limits<int>::max())throw std::invalid_argument("Adjust carrier mapping exceeds int32");return std::int64_t(v);};
        const auto l=edge(-std::int64_t(owner.common.x)),t=edge(-std::int64_t(owner.common.y));
        const auto r=edge(std::int64_t(size[0])-owner.common.x),b=edge(std::int64_t(size[1])-owner.common.y);
        const auto x=std::int64_t(selection.bounds.x)+selection.origin[0]-owner.common.x,y=std::int64_t(selection.bounds.y)+selection.origin[1]-owner.common.y;
        // Native selection bounds are translated only; they are not DPI-scaled.
        if(state&&(selection.bounds.width<=0||selection.bounds.height<=0||r<=l||b<=t||x>=r||y>=b||x+selection.bounds.width<=l||y+selection.bounds.height<=t))return {false,current};
        throw std::invalid_argument("Gaussian Adjust binding reaches an invalid native offscreen access");
    }
    if(kind==0x8000)return {false,current};
    if(kind!=1&&kind!=2&&kind!=4){if(unsafe_helper(kind))throw std::invalid_argument("Adjust binding reaches an invalid native offscreen access");return {false,current};}
    Plane candidate;candidate.family=p.family;candidate.specific=p.specific;std::uint64_t work{};auto tables=plane_tables(candidate,true,work);
    if(stop.stop_requested())throw std::runtime_error("Adjust binding cancelled");
    return {true,std::make_shared<const TableCache>(TableCache{std::move(tables),p.specific})};
}
OwnerTables prepare(const DocumentData& data,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
    if(std::none_of(data.layers.begin(),data.layers.end(),[](const Layer& l){return l.family==0x40000200;}))return {};
    if(data.layers.size()>100000)throw std::length_error("Adjust owner count exceeds limits");
    charge(storage,data.layers.size()*sizeof(Remap),max_storage);charge(work,data.layers.size(),max_work);
    OwnerTables result(data.layers.size());Remap current,saved;bool pending{};
    for(std::size_t i=data.layers.size();i-->0;){
        if(stop.stop_requested())throw std::runtime_error("Adjust rendering cancelled");
        const auto& layer=data.layers[i];
        if(layer.family!=0x40000200){result[i]=current;if(pending){current=saved;pending=false;}continue;}
        if(layer.opaque||layer.planes.size()>100000)throw std::invalid_argument("Invalid Adjust owner");
        charge(storage,layer.specific.size()+sizeof(Tables),max_storage);charge(work,layer.planes.size()+layer.specific.size(),max_work);
        Reader owner(layer.specific);const bool restore=owner.i32()!=0;
        Tables own=identity();
        for(const auto& p:layer.planes){
            if(stop.stop_requested())throw std::runtime_error("Adjust rendering cancelled");
            charge(work,p.specific.size()+768,max_work);
            charge(storage,cache_storage(p),max_storage);
            if(p.runtime_adjust_cache)charge(work,p.runtime_adjust_cache->specific.size(),max_work);
            const auto next=plane_tables(p,layer.common.flags[1]&&p.common.flags[1],work);
            if(layer.common.flags[1]&&p.common.flags[1])compose(own,next);
        }
        if(!layer.common.flags[1])continue;
        // Fresh native owners with no enabled child retain unsafe mode0.
        // A deterministic BYTE identity is the rebuild's explicit safe policy.
        pending=restore;if(pending)saved=current;
        if(current){auto combined=*current;compose(combined,own);own=combined;charge(work,768,max_work);}
        current=std::make_shared<const Tables>(std::move(own));
    }
    return result;
}
OwnerTables prepare(const DocumentData& data,std::stop_token stop){std::uint64_t storage{},work{};return prepare(data,storage,work,stop);}
}
