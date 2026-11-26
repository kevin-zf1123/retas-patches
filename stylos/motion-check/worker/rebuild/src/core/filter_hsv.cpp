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
    if(used>limit||amount>limit-used)throw std::length_error("HSV exceeds cumulative limits");
    used+=amount;
}
using numeric_detail::binary64;
std::array<unsigned,3> adjusted(const std::array<unsigned,3>& rgb,int maximum,const HsvSettings& settings){
    const double r=rgb[0],g=rgb[1],b=rgb[2],v0=std::max({r,g,b}),low=std::min({r,g,b});
    const auto delta=binary64(v0-low);double h{};
    // 662E10 chooses G-max, then B-max, then R-max, with six distinct
    // expressions. Preserve division before multiplication and every store.
    if(delta){
        if(g==v0)h=b==low?binary64(binary64(binary64(binary64(v0-r)/delta)+1.0)*60.0)
            :binary64(binary64(binary64(binary64(b-low)/delta)*60.0)+120.0);
        else if(b==v0)h=r==low?binary64(binary64(binary64(binary64(v0-g)/delta)*60.0)+180.0)
            :binary64(binary64(binary64(binary64(r-low)/delta)*60.0)+240.0);
        else h=b==low?binary64(binary64(binary64(g-low)/delta)*60.0)
            :binary64(binary64(binary64(binary64(v0-b)/delta)*60.0)+300.0);
    }
    double s=v0?binary64(binary64(delta/v0)*maximum):0.0,v=v0;
    h=binary64(h+settings.hue);
    if(h>360.0)h=binary64(h-360.0);else if(h<0.0)h=binary64(h+360.0);
    h=std::clamp(h,0.0,359.0);
    const auto adjust=[maximum](double value,double amount){
        constexpr double percent=0x1.47ae147ae147bp-7;
        if(amount>0.0)return binary64(value+binary64(binary64(binary64(maximum-value)*amount)*percent));
        if(amount<0.0)return binary64(value*binary64(binary64(amount*percent)+1.0));
        return value;
    };
    s=adjust(s,settings.saturation);v=adjust(v,settings.brightness);
    const auto m=binary64(v-binary64(binary64(s*v)/maximum)),c=binary64(v-m);
    const auto ramp=[&](double base){return binary64(binary64(c*binary64(h-base))*0x1.1111111111111p-6);};
    std::array<double,3> out;
    if(h<60.0)out={v,binary64(ramp(0.0)+m),m};
    else if(h<120.0)out={binary64(v-ramp(60.0)),v,m};
    else if(h<180.0)out={m,v,binary64(ramp(120.0)+m)};
    else if(h<240.0)out={m,binary64(v-ramp(180.0)),v};
    else if(h<300.0)out={binary64(ramp(240.0)+m),m,v};
    else out={v,m,binary64(v-ramp(300.0))};
    // The admitted finite settings keep every conversion within int32.
    // Native __ftol2 truncates; the caller stores AL/AX without RT rounding.
    return {unsigned(int(out[0]))&unsigned(maximum),unsigned(int(out[1]))&unsigned(maximum),unsigned(int(out[2]))&unsigned(maximum)};
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
FilterResult run_hsv(Raster& image,const FilterSettings& settings,const std::optional<Rect>& bounds,
    std::stop_token stop,const std::optional<RasterSelection>& coverage,FilterCosts* shared,
    const DocumentScope* native=nullptr,bool* succeeded=nullptr){
    try{
        const auto poll=[&]{if(stop.stop_requested())throw Cancelled{};};poll();
        const auto* value=std::get_if<HsvSettings>(&settings);
        const bool low=image.depth==1||image.depth==2||image.depth==8||image.depth==16;
        if(!value||!std::isfinite(value->hue)||!std::isfinite(value->saturation)||!std::isfinite(value->brightness)
            ||value->hue<-180||value->hue>180||value->saturation<-100||value->saturation>100||value->brightness<-100||value->brightness>100
            ||(!(native&&low)&&image.depth!=24&&image.depth!=32&&image.depth!=64)||image.color_order!=(image.depth==24?4:image.depth>=32?3:0)
            ||image.width<1||image.height<1||image.width>100000||image.height>100000||std::uint64_t(image.width)*image.height>268435456
            ||!std::isfinite(image.resolution)||image.resolution<=0)throw std::invalid_argument("Invalid canonical HSV target or settings");
        const bool word=image.depth==64;const int lane=word?2:1,stride=image.depth/8,maximum=word?65535:255;
        const int columns=(image.width+255)/256,rows=(image.height+255)/256;
        const auto tiles=std::uint64_t(columns)*rows;
        if(image.tiles.size()>tiles)throw std::invalid_argument("Too many HSV tiles");
        FilterCosts local;auto& costs=shared?*shared:local;
        charge(costs.work,(native?0:2ull*image.width*image.height)+tiles+image.tiles.size(),max_work);
        charge(costs.storage,image.tiles.size()*(image.tile_bytes()+256ull)+65536,max_storage);
        for(const auto& [key,bytes]:image.tiles){poll();if(key.x<0||key.y<0||key.x>=columns||key.y>=rows||!bytes||bytes->size()!=image.tile_bytes())
            throw std::invalid_argument("Invalid HSV tile");}
        // 663810 refuses packed/gray targets before locking either surface.
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
            // 664380 copies back only on true. Unlike table-filter rollback,
            // normal false retains the source lock's new entry00 backing.
            if(selected->empty()){poll();if(!modified)return {};image=std::move(staged);return {FilterStatus::Applied,changed,{}};}
            charge(costs.work,2ull*image.width*image.height,max_work);
        }
        for(auto& [key,backing]:staged.tiles){poll();const auto original=image.tiles.find(key);const bool present=original!=image.tiles.end();
            const auto& source=present?*original->second:*backing;Bytes* output=present?nullptr:backing.get();
            const auto writable=[&]() -> Bytes& {if(!output){charge(costs.storage,image.tile_bytes()+128,max_storage);charge(costs.work,256ull*256,max_work);
                auto bytes=std::make_shared<Bytes>(*original->second);output=bytes.get();backing=std::move(bytes);}return *output;};
            const int left=std::max(key.x*256,region.x),top=std::max(key.y*256,region.y);
            const int right=std::min({key.x*256+256,image.width,region.x+region.width}),bottom=std::min({key.y*256+256,image.height,region.y+region.height});
            for(int y=top;y<bottom;++y){poll();for(int x=left;x<right;++x){if(selected?!selected->contains(x,y):(coverage&&!coverage->contains(x,y)))continue;
                const auto at=(std::size_t(y%256)*256+x%256)*stride;
                if(image.depth==24&&at+3>=image.tile_bytes())throw std::invalid_argument("HSV RGB24 would access an unowned fourth byte");
                charge(costs.work,64,max_work);
                const std::array<unsigned,3> before{read(source,at+2*lane,word),read(source,at+lane,word),read(source,at,word)};
                const auto after=adjusted(before,maximum,*value);if(before==after)continue;
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
FilterResult apply_hsv(Raster& image,const FilterSettings& settings,const std::optional<Rect>& bounds,
    std::stop_token stop,const std::optional<RasterSelection>& coverage,FilterCosts* shared){
    return run_hsv(image,settings,bounds,stop,coverage,shared);
}
FilterWorkerOutcome hsv_document_worker(Plane& plane,const Layer& owner,const RegionSelection& selection,
    const HsvSettings& settings,std::stop_token stop,FilterCosts& costs){
    FilterWorkerOutcome outcome;auto& image=*plane.raster;const auto count=image.tiles.size();
    const DocumentScope native{plane,owner,selection};
    const auto result=run_hsv(image,settings,{},stop,{},&costs,&native,&outcome.worker_succeeded);
    outcome.result.handled=!result.rejected();outcome.result.cancelled=result.cancelled();outcome.result.error=result.error;
    outcome.result.changed_pixels=result.changed_pixels;outcome.result.changed=result.changed_pixels!=0;
    outcome.result.storage_changed=image.tiles.size()!=count||(result.modified()&&!result.changed_pixels);
    outcome.result.performed=outcome.worker_succeeded;outcome.modified=result.modified();return outcome;
}
}
