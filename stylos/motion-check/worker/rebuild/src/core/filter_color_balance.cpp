#include "filter_document.hpp"
#include "filter_table_selection.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace retas::filter_detail {
namespace {
constexpr std::uint64_t max_storage=512ull*1024*1024,max_work=128ull*1024*1024;
struct Cancelled {};
void charge(std::uint64_t& used,std::uint64_t amount,std::uint64_t limit){
    if(used>limit||amount>limit-used)throw std::length_error("ColorBalance exceeds cumulative limits");
    used+=amount;
}
using numeric_detail::binary64;
struct Gamma {
    double reciprocal{};
    std::array<double,3> powers{};
    unsigned maximum{};
};
Gamma prepare(int maximum,const ColorBalanceSettings& settings){
    Gamma result{binary64(1.0/maximum),{},unsigned(maximum)};
    const std::array amounts{settings.red,settings.green,settings.blue};
    for(int c:{0,2,1}){
        double amount=amounts[c];
        if(std::abs(amount)>100.0){
            // Native PC53 retains the x87 exponent through amount*100/abs.
            // Cancel an exact power of two to avoid host binary64 overflow.
            int exponent{};const auto mantissa=std::frexp(amount,&exponent);
            amount=binary64(binary64(mantissa*100.0)/std::abs(mantissa));
        }
        result.powers[c]=binary64(std::pow(0.5,binary64(amount*0x1.47ae147ae147bp-7)));
    }
    return result;
}
std::array<unsigned,3> adjusted(const std::array<unsigned,3>& rgb,const Gamma& gamma){
    std::array<unsigned,3> out{};
    for(int c:{0,2,1}){
        // 65C6E0 multiplies by the stored reciprocal. Division by maximum
        // loses observable neutral-parameter truncation differences.
        const auto normalized=binary64(gamma.reciprocal*rgb[c]);
        const auto mapped=binary64(binary64(std::pow(normalized,gamma.powers[c]))*gamma.maximum);
        // Finite admitted settings keep mapped in [0,maximum]. __ftol2
        // truncates before the worker narrows the result through AL or AX.
        out[c]=unsigned(int(mapped))&gamma.maximum;
    }
    return out;
}
unsigned read(const Bytes& bytes,std::size_t at,bool word){return bytes[at]|(word?unsigned(bytes[at+1])<<8:0);}
void write(Bytes& bytes,std::size_t at,unsigned value,bool word){bytes[at]=std::uint8_t(value);if(word)bytes[at+1]=std::uint8_t(value>>8);}
Rect clip(Rect r,const Raster& image){
    const auto l=std::clamp<std::int64_t>(r.x,0,image.width),t=std::clamp<std::int64_t>(r.y,0,image.height);
    const auto right=std::clamp<std::int64_t>(std::int64_t(r.x)+std::max(0,r.width),l,image.width);
    const auto bottom=std::clamp<std::int64_t>(std::int64_t(r.y)+std::max(0,r.height),t,image.height);
    return {int(l),int(t),int(right-l),int(bottom-t)};
}
struct DocumentScope {const Plane& plane;const Layer& owner;const RegionSelection& selection;};
FilterResult run_color_balance(Raster& image,const FilterSettings& settings,const std::optional<Rect>& bounds,
    std::stop_token stop,const std::optional<RasterSelection>& coverage,FilterCosts* shared,
    const DocumentScope* native=nullptr,bool* succeeded=nullptr){
    try{
        const auto poll=[&]{if(stop.stop_requested())throw Cancelled{};};poll();
        const auto* value=std::get_if<ColorBalanceSettings>(&settings);
        const bool low=image.depth==1||image.depth==2||image.depth==8||image.depth==16;
        if(!value||!std::isfinite(value->red)||!std::isfinite(value->green)||!std::isfinite(value->blue)
            ||(!(native&&low)&&image.depth!=24&&image.depth!=32&&image.depth!=64)||image.color_order!=(image.depth==24?4:image.depth>=32?3:0)
            ||image.width<1||image.height<1||image.width>100000||image.height>100000||std::uint64_t(image.width)*image.height>268435456
            ||!std::isfinite(image.resolution)||image.resolution<=0)throw std::invalid_argument("Invalid canonical ColorBalance target or settings");
        const bool word=image.depth==64;const int lane=word?2:1,stride=image.depth/8,maximum=word?65535:255;
        const int columns=(image.width+255)/256,rows=(image.height+255)/256;
        const auto tiles=std::uint64_t(columns)*rows;
        if(image.tiles.size()>tiles)throw std::invalid_argument("Too many ColorBalance tiles");
        FilterCosts local;auto& costs=shared?*shared:local;
        charge(costs.work,(native?0:2ull*image.width*image.height)+tiles+image.tiles.size(),max_work);
        charge(costs.storage,image.tiles.size()*(image.tile_bytes()+256ull)+65536,max_storage);
        for(const auto& [key,bytes]:image.tiles){poll();if(key.x<0||key.y<0||key.x>=columns||key.y>=rows||!bytes||bytes->size()!=image.tile_bytes())
            throw std::invalid_argument("Invalid ColorBalance tile");}
        if(native&&low)return {};
        auto region=clip(bounds.value_or(Rect{0,0,image.width,image.height}),image);
        if(coverage){const auto c=clip(coverage->bounds(),image);const int l=std::max(region.x,c.x),t=std::max(region.y,c.y);
            region={l,t,std::max(0,std::min(region.x+region.width,c.x+c.width)-l),std::max(0,std::min(region.y+region.height,c.y+c.height)-t)};}
        // Keep the public empty-bounds no-op policy. Native returns false
        // here after its locks have already materialized both top-left tiles.
        if(!native&&(!region.width||!region.height))return {};
        Raster staged=image;bool modified{};std::size_t changed{};
        if(!staged.tiles.contains({0,0})){
            charge(costs.storage,image.tile_bytes()+128,max_storage);charge(costs.work,256ull*256,max_work);
            auto bytes=std::make_shared<Bytes>(image.tile_bytes(),255);
            if(image.depth!=24)for(std::size_t at=3*lane;at<bytes->size();at+=stride)write(*bytes,at,0,word);
            staged.tiles[{0,0}]=std::move(bytes);modified=true;
            if(image.depth==24)changed=std::size_t(std::min(image.width,256))*std::min(image.height,256);
        }
        std::optional<TableSelection> selected;
        if(native){
            selected.emplace(native->plane,native->owner,native->selection);
            // 65D450 skips copyback on false, retaining source00 allocation.
            if(selected->empty()){poll();if(!modified)return {};image=std::move(staged);return {FilterStatus::Applied,changed,{}};}
            charge(costs.work,2ull*image.width*image.height,max_work);
        }
        const auto gamma=prepare(maximum,*value);
        for(auto& [key,backing]:staged.tiles){poll();const auto original=image.tiles.find(key);const bool present=original!=image.tiles.end();
            const auto& source=present?*original->second:*backing;Bytes* output=present?nullptr:backing.get();
            const auto writable=[&]() -> Bytes& {if(!output){charge(costs.storage,image.tile_bytes()+128,max_storage);charge(costs.work,256ull*256,max_work);
                auto bytes=std::make_shared<Bytes>(*original->second);output=bytes.get();backing=std::move(bytes);}return *output;};
            const int left=std::max(key.x*256,region.x),top=std::max(key.y*256,region.y);
            const int right=std::min({key.x*256+256,image.width,region.x+region.width}),bottom=std::min({key.y*256+256,image.height,region.y+region.height});
            for(int y=top;y<bottom;++y){poll();for(int x=left;x<right;++x){if(selected?!selected->contains(x,y):(coverage&&!coverage->contains(x,y)))continue;
                const auto at=(std::size_t(y%256)*256+x%256)*stride;
                if(image.depth==24&&at+3>=image.tile_bytes())throw std::invalid_argument("ColorBalance RGB24 would access an unowned fourth byte");
                charge(costs.work,64,max_work);
                const std::array<unsigned,3> before{read(source,at+2*lane,word),read(source,at+lane,word),read(source,at,word)};
                const auto after=adjusted(before,gamma);if(before==after)continue;
                auto& out=writable();for(int c=0;c<3;++c)write(out,at+(2-c)*lane,after[c],word);
                modified=true;if(present||image.depth!=24)++changed;
                // The native p[3] or alpha copy is identity from the immutable
                // source. Omitting it preserves owned padding and other pixels.
            }}
        }
        poll();if(succeeded)*succeeded=true;if(!modified)return {};
        image=std::move(staged);return {FilterStatus::Applied,changed,{}};
    }catch(const Cancelled&){return {FilterStatus::Cancelled,0,{}};}
    catch(const std::exception& e){return {FilterStatus::Rejected,0,e.what()};}
}
}
FilterResult apply_color_balance(Raster& image,const FilterSettings& settings,const std::optional<Rect>& bounds,
    std::stop_token stop,const std::optional<RasterSelection>& coverage,FilterCosts* shared){
    return run_color_balance(image,settings,bounds,stop,coverage,shared);
}
FilterWorkerOutcome color_balance_document_worker(Plane& plane,const Layer& owner,const RegionSelection& selection,
    const ColorBalanceSettings& settings,std::stop_token stop,FilterCosts& costs){
    FilterWorkerOutcome outcome;auto& image=*plane.raster;const auto count=image.tiles.size();const DocumentScope native{plane,owner,selection};
    const auto result=run_color_balance(image,settings,{},stop,{},&costs,&native,&outcome.worker_succeeded);
    outcome.result.handled=!result.rejected();outcome.result.cancelled=result.cancelled();outcome.result.error=result.error;
    outcome.result.changed_pixels=result.changed_pixels;outcome.result.changed=result.changed_pixels!=0;
    outcome.result.storage_changed=image.tiles.size()!=count||(result.modified()&&!result.changed_pixels);
    outcome.result.performed=outcome.worker_succeeded;outcome.modified=result.modified();return outcome;
}
}
