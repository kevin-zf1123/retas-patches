#include "filter_lut.hpp"
#include <algorithm>
#include <cmath>

namespace retas::filter_detail {
namespace {
constexpr std::uint64_t max_storage=512ull*1024*1024,max_work=128ull*1024*1024;
struct Cancelled {};
void charge(std::uint64_t& total,std::uint64_t amount,std::uint64_t limit){
    if(total>limit||amount>limit-total)throw std::length_error("RGB24 filter exceeds cumulative limits");
    total+=amount;
}
std::uint64_t table_work(FilterKind kind,const FilterSettings& settings){
    if(kind!=FilterKind::ToneCurve)return kind==FilterKind::Level?256*12:256*4;
    const auto* curves=std::get_if<ToneCurveSettings>(&settings);if(!curves)throw std::invalid_argument("RGB24 filter settings mismatch");
    std::uint64_t result{};
    for(const auto& curve:curves->curves){const auto n=curve.points.size();if(n>32)throw std::invalid_argument("Invalid RGB24 curve point count");result+=256+(curves->interpolation?1024*n*n:33*n*16);}
    return result;
}
Rect clip(Rect r,const Raster& image){
    const auto l=std::clamp<std::int64_t>(r.x,0,image.width),t=std::clamp<std::int64_t>(r.y,0,image.height);
    const auto right=std::clamp<std::int64_t>(std::int64_t(r.x)+std::max(0,r.width),l,image.width);
    const auto bottom=std::clamp<std::int64_t>(std::int64_t(r.y)+std::max(0,r.height),t,image.height);
    return {int(l),int(t),int(right-l),int(bottom-t)};
}
}
FilterResult apply_rgb24(Raster& image,FilterKind kind,const FilterSettings& settings,const std::optional<Rect>& bounds,
    const FilterExecutionOptions& options,std::stop_token stop,const std::optional<RasterSelection>& coverage,FilterCosts* shared){
    try{
        const auto poll=[&]{if(stop.stop_requested())throw Cancelled{};};poll();
        if(!table_filter(kind)||image.depth!=24||image.color_order!=4||image.width<1||image.height<1
            ||image.width>100000||image.height>100000||std::uint64_t(image.width)*image.height>268435456
            ||!std::isfinite(image.resolution)||image.resolution<=0)throw std::invalid_argument("Invalid canonical RGB24 filter target");
        FilterCosts local;auto& costs=shared?*shared:local;
        const auto columns=(image.width+255)/256,rows=(image.height+255)/256;
        if(image.tiles.size()>std::uint64_t(columns)*rows)throw std::invalid_argument("Too many RGB24 filter tiles");
        charge(costs.work,2ull*image.width*image.height+std::uint64_t(columns)*rows+image.tiles.size()+table_work(kind,settings),max_work);
        // Include retained source backing, map copies and bounded curve scratch.
        charge(costs.storage,image.tiles.size()*(image.tile_bytes()+256ull)+65536,max_storage);
        for(const auto& [tile,bytes]:image.tiles){poll();
            if(tile.x<0||tile.y<0||tile.x>=columns||tile.y>=rows||!bytes||bytes->size()!=image.tile_bytes())
                throw std::invalid_argument("Invalid RGB24 filter tile");
        }
        const auto tables=byte_rgb_tables(kind,settings);
        const bool selected=bounds.has_value()||coverage.has_value();
        auto region=clip(bounds.value_or(Rect{0,0,image.width,image.height}),image);
        if(coverage){const auto c=clip(coverage->bounds(),image);const auto l=std::max(region.x,c.x),t=std::max(region.y,c.y);
            const auto r=std::min(region.x+region.width,c.x+c.width),b=std::min(region.y+region.height,c.y+c.height);region={l,t,std::max(0,r-l),std::max(0,b-t)};}
        if(!region.width||!region.height)return {};
        Raster staged=image;bool modified{};std::size_t changed{};
        // Native GetBlockArea(mode0) visits every logical tile in row order.
        for(int ty=0;ty<rows;++ty)for(int tx=0;tx<columns;++tx){poll();
            const TileCoord key{tx,ty};const auto found=image.tiles.find(key);const bool present=found!=image.tiles.end();
            const auto left=tx*256,top=ty*256,right=std::min(left+256,image.width),bottom=std::min(top+256,image.height);
            if(!present&&(!options.coverage_only||(!selected&&!tables[0][0])))continue;
            bool tile_changed{};Bytes* destination{};
            const auto writable=[&]() -> Bytes& {
                if(!destination){charge(costs.storage,image.tile_bytes()+128,max_storage);
                    auto bytes=present?std::make_shared<Bytes>(*found->second):std::make_shared<Bytes>(image.tile_bytes(),255);
                    destination=bytes.get();staged.tiles[key]=std::move(bytes);
                }
                return *destination;
            };
            if(!present){(void)writable();tile_changed=true;
                // Whole-image missing mode4 uses RGB24 RectFill white, not
                // the per-pixel fourth-byte path. Selected mode4 materializes
                // every tile before testing its individual selected pixels.
                if(!selected){changed+=std::size_t(right-left)*(bottom-top);modified=true;continue;}
            }
            const Bytes& original=present?*found->second:*destination;
            for(int y=top;y<bottom;++y){poll();for(int x=left;x<right;++x){
                if(selected&&(!region.contains(x,y)||(coverage&&!coverage->contains(x,y))))continue;
                const auto at=(std::size_t(y%256)*256+x%256)*3;
                // Native p[3] crosses the pixel/row, but the final physical
                // BGR sample would read one unowned byte. Refuse atomically.
                if(at+3>=image.tile_bytes())throw std::invalid_argument("RGB24 filter would access an unowned fourth byte");
                const auto& bytes=destination?*destination:original;
                if(options.coverage_only){const auto mapped=tables[0][bytes[at+3]];
                    if(mapped!=bytes[at+3]){writable()[at+3]=mapped;tile_changed=true;}
                }else if(bytes[at+3]){
                    const auto blue=tables[2][bytes[at]],green=tables[1][bytes[at+1]],red=tables[0][bytes[at+2]];
                    if(blue!=bytes[at]||green!=bytes[at+1]||red!=bytes[at+2]){auto& out=writable();out[at]=blue;out[at+1]=green;out[at+2]=red;tile_changed=true;}
                }
            }}
            if(!tile_changed)continue;
            modified=true;
            if(!present)changed+=std::size_t(right-left)*(bottom-top);
            else for(int y=top;y<bottom;++y){poll();for(int x=left;x<right;++x){const auto at=(std::size_t(y%256)*256+x%256)*3;
                changed+=original[at]!=(*destination)[at]||original[at+1]!=(*destination)[at+1]||original[at+2]!=(*destination)[at+2];
            }}
        }
        poll();if(!modified)return {};
        // Preserve RGB24 padding and white backing. Optimize would discard
        // bytes that subsequent fourth-byte operations can observe.
        image=std::move(staged);return {FilterStatus::Applied,changed,{}};
    }catch(const Cancelled&){return {FilterStatus::Cancelled,0,{}};}
    catch(const std::exception& e){return {FilterStatus::Rejected,0,e.what()};}
}
}
