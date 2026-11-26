#include "filter_document.hpp"
#include "filter_table_selection.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace retas::filter_detail {
namespace {
constexpr std::uint64_t max_storage=512ull*1024*1024,max_work=128ull*1024*1024;
struct Cancelled {};
void charge(std::uint64_t& used,std::uint64_t amount,std::uint64_t limit){
    if(used>limit||amount>limit-used)throw std::length_error("Unsharp exceeds cumulative limits");
    used+=amount;
}
Rect clip(Rect r,const Raster& image){
    const auto l=std::clamp<std::int64_t>(r.x,0,image.width),t=std::clamp<std::int64_t>(r.y,0,image.height);
    const auto right=std::clamp<std::int64_t>(std::int64_t(r.x)+std::max(0,r.width),l,image.width);
    const auto bottom=std::clamp<std::int64_t>(std::int64_t(r.y)+std::max(0,r.height),t,image.height);
    return {int(l),int(t),int(right-l),int(bottom-t)};
}
unsigned read(const Bytes& bytes,std::size_t at,bool word){return bytes[at]|(word?unsigned(bytes[at+1])<<8:0);}
void write(Bytes& bytes,std::size_t at,unsigned value,bool word){bytes[at]=std::uint8_t(value);if(word)bytes[at+1]=std::uint8_t(value>>8);}
struct DocumentScope {const Plane& plane;const Layer& owner;const RegionSelection& selection;};
FilterResult run_unsharp(Raster& image,const FilterSettings& settings,const std::optional<Rect>& bounds,
    std::stop_token stop,const std::optional<RasterSelection>& coverage,FilterCosts* shared,
    const DocumentScope* native=nullptr,bool* succeeded=nullptr){
    try{
        const auto poll=[&]{if(stop.stop_requested())throw Cancelled{};};poll();
        const auto* value=std::get_if<UnsharpMaskSettings>(&settings);
        if(!value||value->range<1||value->range>100||value->contain<0||value->contain>255||value->strength<1||value->strength>500
            ||(!low_convolution_depth(image.depth)&&image.depth!=32&&image.depth!=64)
            ||image.color_order!=(image.depth==24?4:image.depth>=32?3:0)
            ||image.width<1||image.height<1||image.width>100000||image.height>100000||std::uint64_t(image.width)*image.height>268435456
            ||!std::isfinite(image.resolution)||image.resolution<=0)throw std::invalid_argument("Invalid canonical Unsharp target or settings");
        const int columns=(image.width+255)/256,rows=(image.height+255)/256;
        const auto tiles=std::uint64_t(columns)*rows;
        if(image.tiles.size()>tiles)throw std::invalid_argument("Too many Unsharp tiles");
        FilterCosts local;auto& costs=shared?*shared:local;
        charge(costs.work,tiles+image.tiles.size()+(!native&&image.depth>=32?2ull*image.width*image.height:0),max_work);
        charge(costs.storage,image.tiles.size()*(image.tile_bytes()+256ull)+65536,max_storage);
        for(const auto& [key,bytes]:image.tiles){poll();if(key.x<0||key.y<0||key.x>=columns||key.y>=rows||!bytes||bytes->size()!=image.tile_bytes())
            throw std::invalid_argument("Invalid Unsharp tile");}
        auto region=clip(bounds.value_or(Rect{0,0,image.width,image.height}),image);
        if(coverage){const auto c=clip(coverage->bounds(),image);const int l=std::max(region.x,c.x),t=std::max(region.y,c.y);
            region={l,t,std::max(0,std::min(region.x+region.width,c.x+c.width)-l),std::max(0,std::min(region.y+region.height,c.y+c.height)-t)};}
        // Native locks both (0,0) blocks before an empty-ROI failure. Keep
        // the public transactional empty-bounds policy explicit.
        if(!native&&(!region.width||!region.height))return {};
        const bool word=image.depth==64;const int lane=word?2:1,stride=image.depth/8,maximum=word?65535:255;
        Raster staged=image;bool modified{};std::size_t changed{};
        const auto clear=[&](TileCoord key) -> Bytes& {
            charge(costs.storage,image.tile_bytes()+128,max_storage);charge(costs.work,256ull*256,max_work);
            auto bytes=std::make_shared<Bytes>(image.tile_bytes(),image.depth>=24?255:0);
            if(image.depth>=32)for(std::size_t at=3*lane;at<bytes->size();at+=stride)write(*bytes,at,0,word);
            auto* result=bytes.get();staged.tiles[key]=std::move(bytes);modified=true;return *result;
        };
        if(!staged.tiles.contains({0,0})){clear({0,0});if(image.depth==24)changed=std::size_t(std::min(image.width,256))*std::min(image.height,256);}
        std::optional<TableSelection> selected;
        if(native){
            selected.emplace(native->plane,native->owner,native->selection,image.depth>=32);
            // 66CE80 skips copyback on false; the source entry lock survives.
            if(selected->empty()){poll();if(!modified)return {};image=std::move(staged);return {FilterStatus::Applied,changed,{}};}
            if(image.depth>=32)charge(costs.work,2ull*image.width*image.height,max_work);
        }
        // Other native depth branches return success without a pixel loop.
        // The binder's full CopyAll preserves this clear block and all input.
        if(image.depth<32){poll();if(succeeded)*succeeded=true;if(!modified)return {};image=std::move(staged);return {FilterStatus::Applied,changed,{}};}
        const auto sample=[&](int x,int y){
            std::array<int,4> result{maximum,maximum,maximum,0};const auto found=image.tiles.find({x/256,y/256});if(found==image.tiles.end())return result;
            const auto at=(std::size_t(y%256)*256+x%256)*stride;const auto& bytes=*found->second;
            for(int c=0;c<3;++c)result[c]=int(read(bytes,at+(2-c)*lane,word));result[3]=int(read(bytes,at+3*lane,word));return result;
        };
        using Sum=std::array<std::int64_t,3>;std::vector<Sum> prefix;bool scratch_charged{};
        for(int ty=region.y/256;ty<=(region.y+region.height-1)/256;++ty)for(int tx=region.x/256;tx<=(region.x+region.width-1)/256;++tx){poll();
            const TileCoord key{tx,ty};const auto original=image.tiles.find(key);const bool present=original!=image.tiles.end();
            Bytes* output=present?nullptr:staged.tiles.contains(key)?staged.tiles.at(key).get():nullptr;
            const auto writable=[&]() -> Bytes& {if(!output){if(present){charge(costs.storage,image.tile_bytes()+128,max_storage);charge(costs.work,256ull*256,max_work);
                        auto bytes=std::make_shared<Bytes>(*original->second);output=bytes.get();staged.tiles[key]=std::move(bytes);}
                    else output=&clear(key);}return *output;};
            const int left=tx*256,top=ty*256,right=std::min(left+256,image.width),bottom=std::min(top+256,image.height),width=right-left;
            const int sample_top=std::max(0,top-value->range),sample_bottom=std::min(image.height,bottom+value->range);
            bool prepared{};
            const auto prepare=[&]{
                if(prepared||word)return;prepared=true;
                if(!scratch_charged){const auto capacity=std::size_t(std::min(image.height,256+2*value->range)+1)*std::min(image.width,256);
                    charge(costs.storage,capacity*sizeof(Sum)+128,max_storage);prefix.reserve(capacity);scratch_charged=true;}
                charge(costs.work,std::uint64_t(sample_bottom-sample_top)*(3*width+2*value->range+1),max_work);
                prefix.assign(std::size_t(sample_bottom-sample_top+1)*width,Sum{});
                for(int yy=sample_top;yy<sample_bottom;++yy){poll();Sum row_sum{};
                    for(int xx=std::max(0,left-value->range);xx<=std::min(image.width-1,left+value->range);++xx){const auto p=sample(xx,yy);for(int c=0;c<3;++c)row_sum[c]+=p[c];}
                    for(int x=left;x<right;++x){const auto column=std::size_t(x-left),row=std::size_t(yy-sample_top);
                        for(int c=0;c<3;++c)prefix[(row+1)*width+column][c]=prefix[row*width+column][c]+row_sum[c];
                        const int leaving=x-value->range,entering=x+value->range+1;
                        if(leaving>=0){const auto p=sample(leaving,yy);for(int c=0;c<3;++c)row_sum[c]-=p[c];}
                        if(entering<image.width){const auto p=sample(entering,yy);for(int c=0;c<3;++c)row_sum[c]+=p[c];}
                    }
                }
            };
            for(int y=std::max(top,region.y);y<std::min(bottom,region.y+region.height);++y){poll();for(int x=std::max(left,region.x);x<std::min(right,region.x+region.width);++x){
                if(selected?!selected->contains(x,y):(coverage&&!coverage->contains(x,y)))continue;
                if(!present)(void)writable();prepare();charge(costs.work,16,max_work);
                const auto before=sample(x,y);auto after=before;
                const int first=std::max(0,y-value->range),past=std::min(image.height,y+value->range+1);
                const int count=(past-first)*(std::min(image.width-1,x+value->range)-std::max(0,x-value->range)+1);
                for(int c=0;c<3;++c){
                    // The actual RCVOffscreen64Bit GetColor virtual slot is
                    // a constant-zero stub. Only its center seed contributes.
                    const int mean=word?before[c]/count:int((prefix[std::size_t(past-sample_top)*width+x-left][c]-prefix[std::size_t(first-sample_top)*width+x-left][c])/count);
                    const int difference=before[c]-mean;
                    const int mapped=std::abs(difference)<=value->contain?before[c]-difference/2:before[c]+value->strength*difference/100;
                    after[c]=std::clamp(mapped,0,maximum);
                }
                if(before==after)continue;
                const auto at=(std::size_t(y%256)*256+x%256)*stride;auto& out=writable();for(int c=0;c<3;++c)write(out,at+(2-c)*lane,unsigned(after[c]),word);
                modified=true;++changed;
            }}
        }
        poll();if(succeeded)*succeeded=true;if(!modified)return {};image=std::move(staged);return {FilterStatus::Applied,changed,{}};
    }catch(const Cancelled&){return {FilterStatus::Cancelled,0,{}};}
    catch(const std::exception& e){return {FilterStatus::Rejected,0,e.what()};}
}
}
FilterResult apply_unsharp(Raster& image,const FilterSettings& settings,const std::optional<Rect>& bounds,
    std::stop_token stop,const std::optional<RasterSelection>& coverage,FilterCosts* shared){
    return run_unsharp(image,settings,bounds,stop,coverage,shared);
}
FilterWorkerOutcome unsharp_document_worker(Plane& plane,const Layer& owner,const RegionSelection& selection,
    const UnsharpMaskSettings& settings,std::stop_token stop,FilterCosts& costs){
    FilterWorkerOutcome outcome;auto& image=*plane.raster;const auto count=image.tiles.size();const DocumentScope native{plane,owner,selection};
    const auto result=run_unsharp(image,settings,{},stop,{},&costs,&native,&outcome.worker_succeeded);
    outcome.result.handled=!result.rejected();outcome.result.cancelled=result.cancelled();outcome.result.error=result.error;
    outcome.result.changed_pixels=result.changed_pixels;outcome.result.changed=result.changed_pixels!=0;
    outcome.result.storage_changed=image.tiles.size()!=count||(result.modified()&&!result.changed_pixels);
    outcome.result.performed=outcome.worker_succeeded;outcome.modified=result.modified();return outcome;
}
}
