#include "filter_document.hpp"
#include "filter_table_selection.hpp"
#include "selection_raster.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <cmath>
#include <span>
#include <stdexcept>

namespace retas::filter_detail {
namespace {
constexpr std::uint64_t max_storage=512ull*1024*1024,max_work=128ull*1024*1024;
struct Cancelled {};
void charge(std::uint64_t& used,std::uint64_t amount,std::uint64_t limit){
    if(used>limit||amount>limit-used)throw std::length_error("AutoContrast exceeds cumulative limits");
    used+=amount;
}
struct Range {int low{},high{};bool clustered{true},wide{true};};
Range locate(std::span<const std::uint32_t> histogram,std::uint32_t threshold,int maximum,Range previous){
    const int count=maximum==255?250:63750;
    int first=-1,last=-1,gap_length{};bool active{},gap{};
    std::vector<std::pair<int,int>> clusters;
    for(int i=0;i<count;++i){const auto n=histogram[i];
        if(n){if(first<0)first=i;last=i;}
        if(gap){if(n>threshold)gap=false;else if(++gap_length>=30)active=false;continue;}
        if(n>threshold){if(!active){clusters.emplace_back(i,249);active=true;}else clusters.back().second=i;}
        else if(active){gap=true;gap_length=0;clusters.back().second=i;}
    }
    Range result{previous.low,previous.high};
    if(clusters.empty()){
        // This fallback always examines only 250 bins, even for WORD tables.
        result.low=249;result.high=0;
        for(int i=0;i<250;++i)if(histogram[i]){result.low=std::min(result.low,i);result.high=std::max(result.high,i);}
    }else if(clusters.size()<=2){
        auto selected=clusters.front();
        if(clusters.size()==2){const auto a=clusters[0],b=clusters[1];
            const bool aw=std::uint32_t(a.second-a.first)>=10,bw=std::uint32_t(b.second-b.first)>=10;
            selected=aw&&!bw?a:!aw&&bw?b:std::pair{a.first,b.second};
        }
        result.low=selected.first;result.high=selected.second;
        if(result.high-result.low<=100){
            const auto fallback=clusters.size()==1?std::pair{first,last}:clusters.front();
            result.low=fallback.first;result.high=fallback.second;result.clustered=false;
            result.wide=result.high-result.low>100;
            // Native early cleanup bypasses the 0..1 expansion below.
            return result;
        }
    }else{
        if(std::abs(clusters[0].second-clusters[1].first)>30)result.low=clusters[0].first;
        result.high=clusters.back().second;
    }
    if(result.low==0&&result.high==1)result.high=maximum;
    return result;
}
int signed_word(int value){const auto low=std::uint32_t(value)&65535u;return low<32768u?int(low):int(low)-65536;}
struct Mapping {int low{},scale{};};
Mapping mapping(const std::array<std::vector<std::uint32_t>,3>& histogram,std::uint64_t total,int maximum){
    const auto base=std::uint32_t(numeric_detail::binary64(double(total)*0.0001));
    std::array<Range,3> ranges;for(auto& r:ranges)r={maximum,maximum};double factor=0.9;
    for(int pass=0;pass<3;++pass){const auto threshold=std::uint32_t(numeric_detail::binary64(double(base)*factor));
        for(int c=0;c<3;++c)ranges[c]=locate(histogram[c],threshold,maximum,ranges[c]);
        if(ranges[0].clustered||ranges[1].clustered||ranges[2].clustered)break;
        factor=numeric_detail::binary64(factor-0.2);
    }
    if(!ranges[0].wide&&!ranges[1].wide&&!ranges[2].wide){for(auto& r:ranges){r.low=0;r.high=maximum;}}
    else if(ranges[0].clustered||ranges[1].clustered||ranges[2].clustered){for(auto& r:ranges)if(!r.clustered)r.low=r.high=0;}
    int chosen{};for(int c=1;c<3;++c)if(ranges[c].high-ranges[c].low>=ranges[chosen].high-ranges[chosen].low)chosen=c;
    const auto span=ranges[chosen].high-ranges[chosen].low;
    // +Inf -> rtDouble2Short -> masked CRT FISTP indefinite -> AX == 0.
    if(!span)return {ranges[chosen].low,0};
    const auto ratio=numeric_detail::binary64(double(maximum)/span);
    const auto converted=legacy_rt_double_to_int(numeric_detail::binary64(ratio*100.0));
    if(!converted)throw std::invalid_argument("Invalid AutoContrast scale");
    return {ranges[chosen].low,signed_word(*converted)};
}
int mapped(Mapping table,int value,int channel,int maximum){
    auto result=int((std::int64_t(value)-table.low)*table.scale/100);
    if(maximum==255&&channel==2)result=signed_word(result);
    return std::clamp(result,0,maximum);
}
unsigned read(const Bytes& bytes,std::size_t offset,bool word){return bytes[offset]|(word?unsigned(bytes[offset+1])<<8:0);}
void write(Bytes& bytes,std::size_t offset,unsigned value,bool word){bytes[offset]=std::uint8_t(value);if(word)bytes[offset+1]=std::uint8_t(value>>8);}
Rect clip(Rect r,const Raster& image){
    const auto l=std::clamp<std::int64_t>(r.x,0,image.width),t=std::clamp<std::int64_t>(r.y,0,image.height);
    const auto right=std::clamp<std::int64_t>(std::int64_t(r.x)+std::max(0,r.width),l,image.width);
    const auto bottom=std::clamp<std::int64_t>(std::int64_t(r.y)+std::max(0,r.height),t,image.height);
    return {int(l),int(t),int(right-l),int(bottom-t)};
}
struct DocumentScope {const Plane& plane;const Layer& owner;const RegionSelection& selection;};
FilterResult run_auto_contrast(Raster& image,const FilterSettings& settings,const std::optional<Rect>& bounds,
    const FilterExecutionOptions& options,std::stop_token stop,const std::optional<RasterSelection>& coverage,FilterCosts* shared,
    const DocumentScope* native=nullptr,bool* succeeded=nullptr){
    try{
        const auto poll=[&]{if(stop.stop_requested())throw Cancelled{};};poll();
        const bool low=image.depth==1||image.depth==2||image.depth==8||image.depth==16;
        if(!std::holds_alternative<std::monostate>(settings)||(!(native&&low)&&image.depth!=24&&image.depth!=32&&image.depth!=64)
            ||image.color_order!=(image.depth==24?4:image.depth>=32?3:0)||image.width<1||image.height<1||image.width>100000||image.height>100000
            ||std::uint64_t(image.width)*image.height>268435456||!std::isfinite(image.resolution)||image.resolution<=0)
            throw std::invalid_argument("Invalid canonical AutoContrast target or settings");
        const bool word=image.depth==64;const int maximum=word?65535:255,lane=word?2:1,stride=image.depth/8;
        const auto columns=(image.width+255)/256,rows=(image.height+255)/256;
        const std::uint64_t total=std::uint64_t(image.width)*image.height,tiles=std::uint64_t(columns)*rows;
        if(image.tiles.size()>tiles)throw std::invalid_argument("Too many AutoContrast tiles");
        FilterCosts local;auto& costs=shared?*shared:local;
        charge(costs.work,4*total+tiles+image.tiles.size()+std::uint64_t(maximum+1)*32,max_work);
        charge(costs.storage,image.tiles.size()*(image.tile_bytes()+256ull)+std::uint64_t(maximum+1)*64+65536,max_storage);
        for(const auto& [key,bytes]:image.tiles){poll();if(key.x<0||key.y<0||key.x>=columns||key.y>=rows||!bytes||bytes->size()!=image.tile_bytes())
            throw std::invalid_argument("Invalid AutoContrast tile");}
        auto region=clip(bounds.value_or(Rect{0,0,image.width,image.height}),image);
        if(coverage){const auto c=clip(coverage->bounds(),image);const int l=std::max(region.x,c.x),t=std::max(region.y,c.y);
            region={l,t,std::max(0,std::min(region.x+region.width,c.x+c.width)-l),std::max(0,std::min(region.y+region.height,c.y+c.height)-t)};}
        if(!native&&(!region.width||!region.height))return {};
        Raster staged=image;bool modified{};
        // Both native statistics passes use a write lock. Their combined
        // valid cursor visits materialize every missing logical tile.
        for(int ty=0;ty<rows;++ty)for(int tx=0;tx<columns;++tx){poll();const TileCoord key{tx,ty};if(staged.tiles.contains(key))continue;
            charge(costs.storage,image.tile_bytes()+128,max_storage);charge(costs.work,256ull*256,max_work);
            auto bytes=std::make_shared<Bytes>(image.tile_bytes(),low?0:255);
            if(image.depth>=32)for(std::size_t at=3*lane;at<bytes->size();at+=stride)write(*bytes,at,0,word);
            staged.tiles[key]=std::move(bytes);modified=true;
        }
        const auto color=[&](int x,int y){std::array<unsigned,3> result;
            if(x>=image.width||y>=image.height){result.fill(image.depth==24?maximum:0);return result;}
            if(low){const auto raw=selection_detail::raw_pixel(staged,x,y);
                // GetColor returns raw AL for1/2/8, but RGB gray for16.
                if(image.depth==16)result.fill(unsigned(raw>>8));
                else result={unsigned(image.depth==2&&raw==1?0:raw),0,0};
                return result;}
            const auto& bytes=*staged.tiles.at({x/256,y/256});const auto at=(std::size_t(y%256)*256+x%256)*stride;
            for(int c=0;c<3;++c)result[c]=read(bytes,at+(2-c)*lane,word);return result;
        };
        std::array<std::vector<std::uint32_t>,3> histogram;for(auto& h:histogram)h.resize(maximum+1);
        for(int outer=0;outer<image.height;++outer){poll();for(int inner=0;inner<image.width;++inner){const auto rgb=color(outer,inner);for(int c=0;c<3;++c)++histogram[c][rgb[c]];}}
        poll();const auto transform=mapping(histogram,total,maximum);poll();
        std::array<std::vector<std::uint16_t>,3> tables;
        for(auto& table:tables){table.resize(maximum+1);for(int i=0;i<=maximum;++i)table[i]=std::uint16_t(i);}
        // Only values encountered in the second, ordinary row scan replace
        // identity entries. This matters when mode 4 indexes alpha/padding.
        for(int y=0;y<image.height;++y){poll();for(int x=0;x<image.width;++x){const auto rgb=color(x,y);for(int c=0;c<3;++c)tables[c][rgb[c]]=std::uint16_t(mapped(transform,rgb[c],c,maximum));}}
        // 65F230 runs both statistics passes before654220 checks depth/ROI.
        // A normal false discards every private statistic write for this child.
        if(native&&low){poll();return {};}
        std::optional<TableSelection> selected;
        if(native){selected.emplace(native->plane,native->owner,native->selection);if(selected->empty()){poll();return {};}}
        for(int ty=0;ty<rows;++ty)for(int tx=0;tx<columns;++tx){poll();const TileCoord key{tx,ty};
            const auto original=image.tiles.find(key);const bool present=original!=image.tiles.end();
            Bytes* output=present?nullptr:staged.tiles.at(key).get();
            const auto writable=[&]() -> Bytes& {if(!output){charge(costs.storage,image.tile_bytes()+128,max_storage);charge(costs.work,256ull*256,max_work);
                auto bytes=std::make_shared<Bytes>(*original->second);output=bytes.get();staged.tiles[key]=std::move(bytes);}return *output;};
            const int left=tx*256,top=ty*256,right=std::min(left+256,image.width),bottom=std::min(top+256,image.height);
            for(int y=top;y<bottom;++y){poll();for(int x=left;x<right;++x){if(selected?!selected->contains(x,y):(!region.contains(x,y)||(coverage&&!coverage->contains(x,y))))continue;
                const auto at=(std::size_t(y%256)*256+x%256)*stride,alpha=at+3*lane;
                if(alpha+lane>image.tile_bytes())throw std::invalid_argument("AutoContrast RGB24 would access an unowned fourth byte");
                const auto& bytes=output?*output:*original->second;const auto a=read(bytes,alpha,word);
                if(options.coverage_only){const auto value=tables[0][a];if(value!=a){write(writable(),alpha,value,word);modified=true;}}
                else if(a)for(int c=0;c<3;++c){const auto offset=at+(2-c)*lane;const auto value=read(bytes,offset,word);const auto after=tables[c][value];
                    if(value!=after){write(writable(),offset,after,word);modified=true;}}
            }}
        }
        poll();if(succeeded)*succeeded=true;if(!modified)return {};
        std::size_t changed{};
        for(int ty=0;ty<rows;++ty)for(int tx=0;tx<columns;++tx){poll();const TileCoord key{tx,ty};const auto original=image.tiles.find(key);const auto& bytes=*staged.tiles.at(key);
            if(original!=image.tiles.end()&&original->second==staged.tiles.at(key))continue;
            for(int y=ty*256;y<std::min(ty*256+256,image.height);++y){poll();for(int x=tx*256;x<std::min(tx*256+256,image.width);++x){
                const auto at=(std::size_t(y%256)*256+x%256)*stride;
                if(original!=image.tiles.end())changed+=!std::equal(bytes.begin()+at,bytes.begin()+at+stride,original->second->begin()+at);
                else if(image.depth==24)++changed;
                else changed+=read(bytes,at,word)!=unsigned(maximum)||read(bytes,at+lane,word)!=unsigned(maximum)
                    ||read(bytes,at+2*lane,word)!=unsigned(maximum)||read(bytes,at+3*lane,word)!=0;
            }}
        }
        poll();image=std::move(staged);return {FilterStatus::Applied,changed,{}};
    }catch(const Cancelled&){return {FilterStatus::Cancelled,0,{}};}
    catch(const std::exception& e){return {FilterStatus::Rejected,0,e.what()};}
}
}
FilterResult apply_auto_contrast(Raster& image,const FilterSettings& settings,const std::optional<Rect>& bounds,
    const FilterExecutionOptions& options,std::stop_token stop,const std::optional<RasterSelection>& coverage,FilterCosts* shared){
    return run_auto_contrast(image,settings,bounds,options,stop,coverage,shared);
}
FilterWorkerOutcome auto_contrast_document_worker(Plane& plane,const Layer& owner,const RegionSelection& selection,
    std::stop_token stop,FilterCosts& costs){
    FilterWorkerOutcome outcome;auto& image=*plane.raster;const auto count=image.tiles.size();
    const DocumentScope native{plane,owner,selection};
    const auto result=run_auto_contrast(image,std::monostate{}, {},{false,plane.common.opacity_mode==4},stop,{},&costs,&native,&outcome.worker_succeeded);
    outcome.result.handled=!result.rejected();outcome.result.cancelled=result.cancelled();outcome.result.error=result.error;
    outcome.result.changed_pixels=result.changed_pixels;outcome.result.changed=result.changed_pixels!=0;
    outcome.result.storage_changed=image.tiles.size()!=count||(result.modified()&&!result.changed_pixels);
    outcome.result.performed=outcome.worker_succeeded;outcome.modified=result.modified();return outcome;
}
}
