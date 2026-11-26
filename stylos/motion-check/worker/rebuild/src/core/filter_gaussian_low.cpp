#include "filter_lut.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace retas::filter_detail {
namespace {
constexpr std::uint64_t max_storage=512ull*1024*1024,max_work=128ull*1024*1024;
struct Cancelled {};
void charge(std::uint64_t& used,std::uint64_t amount,std::uint64_t limit){
    if(used>limit||amount>limit-used)throw std::length_error("Gaussian copy exceeds limits");
    used+=amount;
}
Rect clip(Rect r,const Raster& image){
    const auto l=std::clamp<std::int64_t>(r.x,0,image.width),t=std::clamp<std::int64_t>(r.y,0,image.height);
    const auto right=std::clamp<std::int64_t>(std::int64_t(r.x)+std::max(0,r.width),l,image.width);
    const auto bottom=std::clamp<std::int64_t>(std::int64_t(r.y)+std::max(0,r.height),t,image.height);
    return {int(l),int(t),int(right-l),int(bottom-t)};
}
}
Rect gaussian_copy_footprint(Rect work,const Raster& image){
    const auto floor256=[](std::int64_t v){return (v>=0?v/256:-((-v+255)/256))*256;};
    const auto ceil256=[&](std::int64_t v){return -floor256(-v);};
    const auto left=std::clamp<std::int64_t>(floor256(work.x)-256,0,image.width);
    const auto top=std::clamp<std::int64_t>(floor256(work.y)-256,0,image.height);
    const auto right=std::clamp<std::int64_t>(ceil256(std::int64_t(work.x)+std::max(0,work.width)),left,image.width);
    const auto bottom=std::clamp<std::int64_t>(ceil256(std::int64_t(work.y)+std::max(0,work.height)),top,image.height);
    return {int(left),int(top),int(right-left),int(bottom-top)};
}
FilterResult apply_low_gaussian(Raster& image,const FilterSettings& settings,const std::optional<Rect>& bounds,
    std::stop_token stop,FilterCosts* shared,bool native_bounds){
    try{
        const auto poll=[&]{if(stop.stop_requested())throw Cancelled{};};poll();
        const auto* value=std::get_if<GaussianBlurSettings>(&settings);
        if(!value||!std::isfinite(value->size_mm)||value->size_mm<0||value->size_mm>1000||value->size_px<0||value->size_px>20000
            ||!low_convolution_depth(image.depth)||image.color_order!=(image.depth==24?4:0)
            ||image.width<1||image.height<1||image.width>100000||image.height>100000||std::uint64_t(image.width)*image.height>268435456
            ||!std::isfinite(image.resolution)||image.resolution<=0)throw std::invalid_argument("Invalid canonical low-depth Gaussian target or settings");
        const auto radius=value->use_millimeters?(image.resolution*value->size_mm)*0x1.42850a142850ap-5:double(value->size_px);
        if(!std::isfinite(radius)||radius>double(std::numeric_limits<int>::max()/2-1))
            throw std::invalid_argument("Gaussian radius exceeds the signed support conversion domain");
        const int columns=(image.width+255)/256,rows=(image.height+255)/256;
        const auto tiles=std::uint64_t(columns)*rows;
        if(image.tiles.size()>tiles)throw std::invalid_argument("Too many Gaussian tiles");
        FilterCosts local;auto& costs=shared?*shared:local;
        charge(costs.work,tiles+image.tiles.size(),max_work);
        charge(costs.storage,image.tiles.size()*(image.tile_bytes()+256ull)+65536,max_storage);
        for(const auto& [key,bytes]:image.tiles){poll();if(key.x<0||key.y<0||key.x>=columns||key.y>=rows||!bytes||bytes->size()!=image.tile_bytes())
            throw std::invalid_argument("Invalid Gaussian tile");}
        const auto requested=bounds.value_or(Rect{0,0,image.width,image.height});
        const auto region=native_bounds?requested:clip(requested,image);
        if(!native_bounds&&(!region.width||!region.height))return {};
        // 6583B0 copies the identity scratch image over a tile-rounded region
        // with one 256-pixel tile added on the left and top only. Actual mask
        // bits and the Gaussian radius do not gate this low-depth final Copy.
        const auto footprint=gaussian_copy_footprint(region,image);if(!footprint.width||!footprint.height)return {};
        const int left=footprint.x,top=footprint.y,right=left+footprint.width,bottom=top+footprint.height;
        Raster staged=image;bool modified{};std::size_t changed{};
        const auto materialize=[&](TileCoord key){
            poll();if(staged.tiles.contains(key))return;
            charge(costs.storage,image.tile_bytes()+128,max_storage);charge(costs.work,256ull*256,max_work);
            staged.tiles[key]=std::make_shared<Bytes>(image.tile_bytes(),image.depth==24?255:0);modified=true;
            if(image.depth==24)changed+=std::size_t(std::min(256,image.width-key.x*256))*std::min(256,image.height-key.y*256);
        };
        for(int y=top/256;y<=(bottom-1)/256;++y)for(int x=left/256;x<=(right-1)/256;++x){poll();charge(costs.work,1,max_work);
            // Copy1/2/8 retains missing complete blocks but initializes a
            // partial page-edge block. CommonCopy16/24 initializes both.
            if(image.depth>=16||std::min(256,image.width-x*256)<256||std::min(256,image.height-y*256)<256)
                materialize({x,y});
        }
        // Present partial-width Copy1/2 calls normal NextY after its last row,
        // including the one-past tile. Copy8 has no such cursor movement.
        if(image.depth<=2&&image.width%256&&right==image.width&&bottom<image.height)
            materialize({image.width/256,bottom/256});
        poll();if(!modified)return {};image=std::move(staged);return {FilterStatus::Applied,changed,{}};
    }catch(const Cancelled&){return {FilterStatus::Cancelled,0,{}};}
    catch(const std::exception& e){return {FilterStatus::Rejected,0,e.what()};}
}
}
