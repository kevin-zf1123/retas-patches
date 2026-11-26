#include "filter_lut.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace retas::filter_detail {
namespace {
constexpr std::uint64_t max_storage=512ull*1024*1024,max_work=128ull*1024*1024;
struct Cancelled {};
void charge(std::uint64_t& used,std::uint64_t amount,std::uint64_t limit){
    if(used>limit||amount>limit-used)throw std::length_error("Fixed convolution exceeds cumulative limits");
    used+=amount;
}
Rect clip(Rect r,const Raster& image){
    const auto l=std::clamp<std::int64_t>(r.x,0,image.width),t=std::clamp<std::int64_t>(r.y,0,image.height);
    const auto right=std::clamp<std::int64_t>(std::int64_t(r.x)+std::max(0,r.width),l,image.width);
    const auto bottom=std::clamp<std::int64_t>(std::int64_t(r.y)+std::max(0,r.height),t,image.height);
    return {int(l),int(t),int(right-l),int(bottom-t)};
}
}
FilterResult apply_low_convolution(Raster& image,FilterKind kind,const FilterSettings& settings,
    const std::optional<Rect>& bounds,std::stop_token stop,const std::optional<RasterSelection>& coverage,FilterCosts* shared){
    try{
        const auto poll=[&]{if(stop.stop_requested())throw Cancelled{};};poll();
        if(!fixed_convolution(kind)||!std::holds_alternative<std::monostate>(settings)||!low_convolution_depth(image.depth)
            ||image.color_order!=(image.depth==24?4:0)||image.width<1||image.height<1||image.width>100000||image.height>100000
            ||std::uint64_t(image.width)*image.height>268435456||!std::isfinite(image.resolution)||image.resolution<=0)
            throw std::invalid_argument("Invalid canonical low-depth fixed convolution target or settings");
        const auto columns=(image.width+255)/256,rows=(image.height+255)/256;
        const auto tiles=std::uint64_t(columns)*rows;
        if(image.tiles.size()>tiles)throw std::invalid_argument("Too many fixed convolution tiles");
        FilterCosts local;auto& costs=shared?*shared:local;
        charge(costs.work,tiles+image.tiles.size()+(image.depth==24?2ull*image.width*image.height:0),max_work);
        charge(costs.storage,image.tiles.size()*(image.tile_bytes()+256ull)+65536,max_storage);
        for(const auto& [key,bytes]:image.tiles){poll();
            if(key.x<0||key.y<0||key.x>=columns||key.y>=rows||!bytes||bytes->size()!=image.tile_bytes())
                throw std::invalid_argument("Invalid fixed convolution tile");
        }
        // 651C30 convolves only 32/64 bits. For 1/2/8/16 bits it returns
        // success, and 6579F0 ignores the rejected 656980 copyback.
        if(image.depth!=24){poll();return {};}
        auto region=clip(bounds.value_or(Rect{0,0,image.width,image.height}),image);
        if(coverage){const auto c=clip(coverage->bounds(),image);const int l=std::max(region.x,c.x),t=std::max(region.y,c.y);
            region={l,t,std::max(0,std::min(region.x+region.width,c.x+c.width)-l),std::max(0,std::min(region.y+region.height,c.y+c.height)-t)};}
        Raster staged=image;const bool missing=!image.tiles.contains({0,0});
        if(missing){
            // 656980 write-locks (0,0) before inspecting any ROI or mask.
            charge(costs.storage,image.tile_bytes()+128,max_storage);charge(costs.work,256ull*256,max_work);
            staged.tiles[{0,0}]=std::make_shared<Bytes>(image.tile_bytes(),255);
        }
        poll();
        // The full-surface temporary retains all source bytes. Its RGB24
        // copyback is identity, including p[3]. Only the last physical sample
        // could access an unowned byte; test its selection before refusing.
        for(const auto& [key,unused]:staged.tiles){poll();
            const int x=key.x*256+255,y=key.y*256+255;
            if(x<image.width&&y<image.height&&region.contains(x,y)&&(!coverage||coverage->contains(x,y)))
                throw std::invalid_argument("Fixed convolution RGB24 would access an unowned fourth byte");
        }
        poll();if(!missing)return {};
        const auto changed=std::size_t(std::min(image.width,256))*std::min(image.height,256);
        image=std::move(staged);return {FilterStatus::Applied,changed,{}};
    }catch(const Cancelled&){return {FilterStatus::Cancelled,0,{}};}
    catch(const std::exception& e){return {FilterStatus::Rejected,0,e.what()};}
}
}
