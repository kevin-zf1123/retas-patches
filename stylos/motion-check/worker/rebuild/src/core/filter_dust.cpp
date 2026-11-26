#include "filter_dust.hpp"
#include "selection_raster.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>

namespace retas::filter_detail {
namespace {
constexpr std::uint64_t max_storage=512ull*1024*1024,max_work=128ull*1024*1024;
struct Cancelled {};
struct Budget {
    FilterCosts& costs;std::stop_token stop;
    void poll() const {if(stop.stop_requested())throw Cancelled{};}
    void add(std::uint64_t& used,std::uint64_t amount,std::uint64_t limit){poll();if(used>limit||amount>limit-used)throw std::length_error("Dust exceeds cumulative limits");used+=amount;}
    void work(std::uint64_t n=1){add(costs.work,n,max_work);}
    void storage(std::uint64_t n){add(costs.storage,n,max_storage);}
};
Rect clip(Rect r,const Raster& image){
    const auto l=std::clamp<std::int64_t>(r.x,0,image.width),t=std::clamp<std::int64_t>(r.y,0,image.height);
    const auto right=std::clamp<std::int64_t>(std::int64_t(r.x)+std::max(0,r.width),l,image.width);
    const auto bottom=std::clamp<std::int64_t>(std::int64_t(r.y)+std::max(0,r.height),t,image.height);
    return {int(l),int(t),int(right-l),int(bottom-t)};
}
void validate(const Raster& image,Budget& budget){
    if((image.depth!=1&&image.depth!=2&&image.depth!=8&&image.depth!=16&&image.depth!=24&&image.depth!=32&&image.depth!=64)
        ||image.color_order!=(image.depth==24?4:image.depth>=32?3:0)||image.width<1||image.height<1||image.width>100000||image.height>100000
        ||std::uint64_t(image.width)*image.height>268435456||!std::isfinite(image.resolution)||image.resolution<=0)
        throw std::invalid_argument("Invalid canonical Dust target");
    budget.work(std::uint64_t(image.width)*image.height+image.tiles.size());
    budget.storage(image.tiles.size()*(image.tile_bytes()+256ull));
    for(const auto& [key,bytes]:image.tiles){budget.poll();if(key.x<0||key.y<0||key.x>=(image.width+255)/256||key.y>=(image.height+255)/256||!bytes||bytes->size()!=image.tile_bytes())throw std::invalid_argument("Invalid Dust tile");}
}
// 5F68D0 includes a zero guard DWORD even for widths divisible by 32.
// Scanline lookahead probes x==width before checking the span's right edge.
struct Bits {
    int width,height;std::size_t stride;std::vector<std::uint32_t> words;
    Bits(int w,int h,Budget& b):width(w),height(h),stride(std::size_t(w/32)+1){b.storage(stride*h*4);b.work(stride*h);words.resize(stride*h);}
    bool get(int x,int y) const {return x>=0&&x<=width&&y>=0&&y<height&&(words[std::size_t(y)*stride+x/32]&(1u<<(x%32)));}
    void set(int x,int y,bool value=true){auto& word=words[std::size_t(y)*stride+x/32];const auto bit=1u<<(x%32);if(value)word|=bit;else word&=~bit;}
    void clear(Budget& b){b.work(words.size());std::fill(words.begin(),words.end(),0);}
};
bool transparent(std::uint64_t raw,int depth,bool white){
    switch(depth){case 1:case 8:return !raw;case 2:return white?raw!=3:!raw;case 16:return !(raw&255);case 24:return white&&raw==0xffffff;
    case 32:return !(raw>>24)||(white&&(raw&0xffffff)==0xffffff);case 64:return !(raw>>48)||(white&&(raw&0xffffffffffffull)==0xffffffffffffull);}
    return true;
}
std::uint64_t key(std::uint64_t raw,int depth){
    // Native RTCOLORREF keys put R in the low lane, unlike stored BGR.
    switch(depth){
    case 1:return raw?0xff000000:0;
    case 2:return raw==3?0xff000000:raw==2?0xffffffff:0;
    case 8:return raw<<24;
    case 16:return ((raw&255)<<24)|((raw>>8)*0x10101);
    case 24:case 32:return (raw&0xff00)|((raw&255)<<16)|((raw>>16)&255)|(depth==24?0xff000000:raw&0xff000000);
    case 64:return (raw&0xffff0000ffff0000ull)|((raw&65535)<<32)|((raw>>32)&65535);
    }
    return 0;
}
std::uint64_t stored(std::uint64_t value,int depth){
    if(depth==64)return key(value,64);
    const auto r=unsigned(value&255),g=unsigned((value>>8)&255),b=unsigned((value>>16)&255),a=unsigned(value>>24);
    const auto gray=(r+4*g+2*b)/7;
    switch(depth){case 2:return a<128?0:gray>=127?2:3;case 8:return ((255-gray)*(a+1))>>8;case 16:return a|(gray<<8);
    case 24:return (value&0xff00)|((value&255)<<16)|((value>>16)&255);case 32:return key(value,32);}
    return value;
}
struct Span {int y,left,right,dy;};
struct Kernel {
    Raster& output;const Raster& input;Rect area;int type,size;bool white,cached;Budget& budget;
    std::span<const DustMask> masks;
    Bits visited,component,predicate,opaque,allowed;std::vector<bool> cache_present,loaded,component_present;
    std::vector<Span> stack;
    int columns;Rect component_bounds,damage{};bool modified{},allocated{},performed{},has_companion;int companion_x{},companion_y{};
    Kernel(Raster& dst,const Raster& src,Rect r,int t,int s,bool w,Budget& b,std::span<const DustMask> mask,const Raster* companion)
        :output(dst),input(src),area(r),type(t),size(s),white(w),cached(src.depth>1&&t!=2),budget(b),masks(mask),
        visited(src.width,src.height,b),component(src.width,src.height,b),predicate(src.width,src.height,b),opaque(src.width,src.height,b),allowed(src.width,src.height,b),columns((src.width+255)/256),has_companion(companion!=nullptr){
        const auto count=std::size_t(columns)*((src.height+255)/256);budget.storage(3*count+sizeof(Span)*64);cache_present.resize(count);loaded.resize(count);component_present.resize(count);stack.reserve(64);
        if(cached)for(const auto& [tile,bytes]:src.tiles){auto piece=clip({tile.x*256,tile.y*256,256,256},src);
            const auto l=std::max(piece.x,area.x),t0=std::max(piece.y,area.y),right=std::min(piece.x+piece.width,area.x+area.width),bottom=std::min(piece.y+piece.height,area.y+area.height);
            if(l>=right||t0>=bottom)continue;cache_present[std::size_t(tile.y)*columns+tile.x]=true;
            for(int y=t0;y<bottom;++y)for(int x=l;x<right;++x){budget.work();opaque.set(x,y,!transparent(selection_detail::raw_pixel(src,x,y),src.depth,white));}
        }
        // The special12100001 companion builds a separate, same-coordinate
        // opaque cache over the work rectangle; missing pieces allow passage.
        if(companion)for(const auto& [tile,bytes]:companion->tiles){const auto l=std::max(area.x,tile.x*256),t0=std::max(area.y,tile.y*256);
            const auto right=std::min({area.x+area.width,companion->width,(tile.x+1)*256}),bottom=std::min({area.y+area.height,companion->height,(tile.y+1)*256});
            for(int y=t0;y<bottom;++y)for(int x=l;x<right;++x){budget.work();allowed.set(x,y,!transparent(selection_detail::raw_pixel(*companion,x,y),companion->depth,true));}
        }
    }
    void materialize(int x,int y){
        if(!output.contains(x,y)||output.tiles.contains({x/256,y/256}))return;
        budget.storage(output.tile_bytes()+256);budget.work(65536);
        selection_detail::store_raw_pixel(output,x,y,selection_detail::clear_word(output.depth));modified=true;allocated=true;
    }
    void write(int x,int y,std::uint64_t value){
        if(selection_detail::raw_pixel(output,x,y)==value)return;
        auto& tile=output.tiles.at({x/256,y/256});if(tile.use_count()!=1){budget.storage(output.tile_bytes());budget.work(65536);}
        selection_detail::store_raw_pixel(output,x,y,value);modified=true;
    }
    bool equality(std::uint64_t value,std::uint64_t seed) const {
        const int depth=cached?1:input.depth;
        if(depth==1||depth==8)return value==seed;
        if(depth==32){if(!((value|seed)>>24))return true;if(white&&transparent(value,32,true)&&transparent(seed,32,true))return true;return value==seed;}
        if(depth==64)return !((value|seed)>>48)||value==seed;
        return false; // 5F4970 has no2/16/24 equality leaf.
    }
    void probe(int x,int y,std::uint64_t seed){
        budget.work();if(!output.contains(x,y))return;
        const auto index=std::size_t(y/256)*columns+x/256;if(loaded[index])return;loaded[index]=true;
        // 5F7300 write-locks its input before freezing the whole block's
        // predicate. Only the 1-bit and ExactColor paths bind the real image.
        if(!cached)materialize(x,y);else cache_present[index]=true;
        const int l=x/256*256,t=y/256*256,right=std::min(l+256,input.width),bottom=std::min(t+256,input.height);
        budget.work(std::uint64_t(right-l)*(bottom-t));
        for(int yy=t;yy<bottom;++yy)for(int xx=l;xx<right;++xx){const auto value=cached?std::uint64_t(opaque.get(xx,yy)):selection_detail::raw_pixel(output,xx,yy);
            predicate.set(xx,yy,!allowed.get(xx,yy)&&equality(value,seed));}
    }
    void push(Span s){
        budget.work();if(s.y+s.dy<0||s.y+s.dy>=input.height)return;
        if(stack.size()==stack.capacity()){budget.storage(stack.capacity()*sizeof(Span));stack.reserve(stack.capacity()*2);}
        stack.push_back(s);
    }
    Rect flood(int sx,int sy){
        component.clear(budget);budget.work(loaded.size());std::fill(loaded.begin(),loaded.end(),false);stack.clear();
        const auto seed=cached?std::uint64_t(opaque.get(sx,sy)):selection_detail::raw_pixel(output,sx,sy);
        probe(sx,sy,seed);const bool reference=predicate.get(sx,sy);
        int left=sx,right=sx,top=sy,bottom=sy;
        push({sy,sx,sx,1});push({sy+1,sx,sx,-1});
        while(!stack.empty()){
            budget.work();const auto s=stack.back();stack.pop_back();const int y=s.y+s.dy;
            // Native bounds include popped span endpoints before testing bits.
            left=std::min(left,s.left);right=std::max(right,s.right);
            int x=s.left;bool any{};probe(x,y,seed);
            const auto matches=[&]{return !component.get(x,y)&&predicate.get(x,y)==reference;};
            while(x>=0&&matches()){component.set(x,y);any=true;--x;if(x>=0)probe(x,y,seed);}
            int start{};
            bool run=x<s.left;
            if(run){start=++x;if(x<s.left)push({y,x,s.left-1,-s.dy});x=s.left+1;probe(x,y,seed);}
            for(;;){
                if(run){
                    while(x<input.width&&matches()){component.set(x,y);any=true;++x;if(x<input.width)probe(x,y,seed);}
                    push({y,start,x-1,s.dy});if(x>s.right+1)push({y,s.right+1,x-1,-s.dy});
                }
                // Preserve both lookahead probes, including the diagonal tile
                // reached after a failed left scan at a block's lower corner.
                ++x;probe(x,y,seed);
                while(x<=s.right&&predicate.get(x,y)!=reference){++x;probe(x,y,seed);}
                start=x;if(any){top=std::min(top,y);bottom=std::max(bottom,y);}
                if(x>s.right)break;run=true;
            }
        }
        component_bounds={left,top,std::min(input.width,right+1)-left,std::min(input.height,bottom+1)-top};
        // 5F57D0 exports only the reported rectangle into the physical mask;
        // 65FF20 then ORs that mask into visited. A one-row flood can reach
        // beyond its reported x bounds when no adjacent span can be pushed.
        budget.work(component_present.size());std::fill(component_present.begin(),component_present.end(),false);
        for(int y=top;y<top+component_bounds.height;++y)for(int x=left;x<left+component_bounds.width;++x){budget.work();if(component.get(x,y))visited.set(x,y);
            component_present[std::size_t(y/256)*columns+x/256]=true;}
        return component_bounds;
    }
    bool member(int x,int y) const {const auto& b=component_bounds;return x>=b.x&&y>=b.y&&x<b.x+b.width&&y<b.y+b.height&&component.get(x,y);}
    void cleanup(Rect bounds,const DustMask& mask){
        if(mask.cell_scale<1)return;
        const auto integer=[](double value){if(!std::isfinite(value)||value<INT32_MIN||value>INT32_MAX)throw std::invalid_argument("Dust mask geometry exceeds int32");return std::int64_t(value);};
        const auto k=integer(mask.cell_scale);
        const auto delta=[&](std::int64_t offset){
            // Native RCPoint multiplication wraps; reject that geometry rather
            // than deriving an invalid address from its wrapped result.
            if(offset<INT32_MIN||offset>INT32_MAX)throw std::invalid_argument("Dust mask offset exceeds int32");
            const auto value=-offset*k;if(value<INT32_MIN||value>INT32_MAX)throw std::invalid_argument("Dust mask translation exceeds int32");return value;
        };
        const auto dx=delta(mask.offset[0]),dy=delta(mask.offset[1]);
        const auto left=std::max(integer(bounds.x*mask.request_scale)+mask.offset[0],-dx/k);
        const auto top=std::max(integer(bounds.y*mask.request_scale)+mask.offset[1],-dy/k);
        const auto right=std::min(integer((double(bounds.x)+bounds.width)*mask.request_scale)+mask.offset[0]+1,(input.width-dx)/k);
        const auto bottom=std::min(integer((double(bounds.y)+bounds.height)*mask.request_scale)+mask.offset[1]+1,(input.height-dy)/k);
        if(left>=right||top>=bottom)return;
        const auto sl=std::max<std::int64_t>(left,0),st=std::max<std::int64_t>(top,0),sr=std::min<std::int64_t>(right,mask.width),sb=std::min<std::int64_t>(bottom,mask.height);
        if(sl<sr&&st<sb){
            budget.work(std::uint64_t(sr-sl)*(sb-st));
            for(auto v=st;v<sb;++v)for(auto u=sl;u<sr;++u){
                budget.poll();if(mask.test(int(u),int(v)))continue;
                const auto l=std::max<std::int64_t>({dx+k*u,bounds.x,0}),t=std::max<std::int64_t>({dy+k*v,bounds.y,0});
                const auto r=std::min<std::int64_t>({dx+k*(u+1),std::int64_t(bounds.x)+bounds.width,input.width}),b=std::min<std::int64_t>({dy+k*(v+1),std::int64_t(bounds.y)+bounds.height,input.height});
                if(l>=r||t>=b)continue;budget.work(std::uint64_t(r-l)*(b-t));
                // Forward cells also handle negative source coordinates
                // without treating truncation toward zero as pixel membership.
                for(int y=int(t);y<b;++y)for(int x=int(l);x<r;++x)component.set(x,y,false);
            }
        }
        // Optimize uses the mapped job, before clipping to carrier extent.
        // Each touched physical block tests its whole logical exported mask.
        const auto l=std::max<std::int64_t>(0,dx+k*left),t=std::max<std::int64_t>(0,dy+k*top);
        const auto r=std::min<std::int64_t>(input.width,dx+k*right),b=std::min<std::int64_t>(input.height,dy+k*bottom);
        if(l>=r||t>=b)return;
        for(int by=int(t)/256;by<=(b-1)/256;++by)for(int bx=int(l)/256;bx<=(r-1)/256;++bx){
            budget.work();
            const auto index=std::size_t(by)*columns+bx;if(!component_present[index])continue;bool any{};
            const auto x0=std::max(bounds.x,bx*256),y0=std::max(bounds.y,by*256),x1=std::min(bounds.x+bounds.width,(bx+1)*256),y1=std::min(bounds.y+bounds.height,(by+1)*256);
            for(int y=y0;y<y1&&!any;++y)for(int x=x0;x<x1;++x){budget.work();if(component.get(x,y)){any=true;break;}}
            component_present[index]=any;
        }
    }
    struct Paint {std::uint64_t raw;bool opaque;};
    std::optional<Paint> replacement(Rect bounds,int sx,int sy){
        if(input.depth==1){const auto raw=std::uint64_t(type==0?0:type==1?1:!selection_detail::raw_pixel(output,sx,sy));return Paint{raw,raw!=0};}
        const auto fallback=input.depth==16?0ull:selection_detail::clear_word(input.depth==64?64:32);
        if(type==0)return Paint{stored(fallback,input.depth),false};
        std::map<std::uint64_t,std::size_t> votes;
        constexpr std::array<std::array<int,2>,4> directions{{{-1,0},{1,0},{0,-1},{0,1}}};
        for(int y=bounds.y;y<bounds.y+bounds.height;++y)for(int x=bounds.x;x<bounds.x+bounds.width;++x){budget.work();if(!component.get(x,y))continue;
            for(auto d:directions){budget.work();const int xx=x+d[0],yy=y+d[1];if(!output.contains(xx,yy)||member(xx,yy))continue;
                if(has_companion){companion_x=xx;companion_y=yy;if(allowed.get(xx,yy))continue;}
                // The selector only read-locks the component mask. Its color
                // reads retain660F00's write lock on the original surface.
                materialize(xx,yy);
                auto value=key(selection_detail::raw_pixel(output,xx,yy),input.depth);
                if(input.depth==32&&!cached&&white&&(value&0xffffff)==0xffffff)value=0xffffff;
                if(!votes.contains(value))budget.storage(64);++votes[value];
            }
        }
        if(votes.empty())return {};
        std::uint64_t winner=fallback;std::size_t count{};
        // 64-bit selection tests key byte3 (high G), rather than alpha.
        for(const auto& [value,n]:votes)if(((value>>24)&255)&&n>count){winner=value;count=n;}
        return Paint{stored(winner,input.depth),input.depth==64?bool(winner>>48):bool(winner>>24)};
    }
    void fill(Rect bounds,Paint paint){
        bool processed{};
        for(int by=bounds.y/256;by<=(bounds.y+bounds.height-1)/256;++by)for(int bx=bounds.x/256;bx<=(bounds.x+bounds.width-1)/256;++bx){
            budget.work();if(!component_present[std::size_t(by)*columns+bx]||(!paint.opaque&&!output.tiles.contains({bx,by})))continue;
            processed=true;
            materialize(bx*256,by*256);
            const auto l=std::max(bounds.x,bx*256),t=std::max(bounds.y,by*256),right=std::min(bounds.x+bounds.width,(bx+1)*256),bottom=std::min(bounds.y+bounds.height,(by+1)*256);
            for(int y=t;y<bottom;++y)for(int x=l;x<right;++x){budget.work();if(component.get(x,y))write(x,y,paint.raw);}
        }
        if(processed){
            if(!performed)damage=bounds;
            else{const auto r=std::max(damage.x+damage.width,bounds.x+bounds.width),b=std::max(damage.y+damage.height,bounds.y+bounds.height);damage.x=std::min(damage.x,bounds.x);damage.y=std::min(damage.y,bounds.y);damage.width=r-damage.x;damage.height=b-damage.y;}
            performed=true;
        }
    }
    void run(){
        for(int by=area.y/256;by<=(area.y+area.height-1)/256;++by)for(int bx=area.x/256;bx<=(area.x+area.width-1)/256;++bx){
            budget.work();if(cached?!cache_present[std::size_t(by)*columns+bx]:!output.tiles.contains({bx,by}))continue;
            const int l=std::max(area.x,bx*256),t=std::max(area.y,by*256),right=std::min(area.x+area.width,(bx+1)*256),bottom=std::min(area.y+area.height,(by+1)*256);
            for(int y=t;y<bottom;++y){companion_x=l;companion_y=y;for(int x=l;x<right;++x,++companion_x){
                // 660F00 resets the companion at each row, but its post-flood
                // restore accidentally moves visited instead of companion.
                // Neighbor voting can therefore change the next seed's gate.
                budget.work();if(visited.get(x,y)||(has_companion&&allowed.get(companion_x,companion_y)))continue;
                const bool trans=cached?!opaque.get(x,y):transparent(selection_detail::raw_pixel(output,x,y),input.depth,white);
                if((type==0&&trans)||(type==1&&!trans))continue;
                // 5FC170 rechecks the actual seed through a temporary lock,
                // preserving the outer companion cursor even on false.
                if(has_companion&&allowed.get(x,y)){materialize(x,y);continue;}
                const auto bounds=flood(x,y);
                if(bounds.width<=size&&bounds.height<=size&&bounds.x>area.x&&bounds.y>area.y&&bounds.x+bounds.width<area.x+area.width&&bounds.y+bounds.height<area.y+area.height){
                    for(const auto& mask:masks)cleanup(bounds,mask);
                    if(auto value=replacement(bounds,x,y))fill(bounds,*value);
                }
                // 660F00 restores the write-locked original cursor after every
                // eligible seed, including rejected or empty components.
                materialize(x,y);
            }}
        }
    }
};
}
DustOutcome dust_region(Raster& image,int type,int size,bool white,Rect bounds,
    std::span<const DustMask> masks,std::stop_token stop,const Raster* companion,FilterCosts* shared){
    try{
        FilterCosts local;Budget budget{shared?*shared:local,stop};budget.poll();
        if(type<0||type>2||size<0)throw std::invalid_argument("Invalid converted Dust settings");
        validate(image,budget);if(companion)validate(*companion,budget);
        for(const auto& mask:masks)if(mask.width<0||mask.height<0||!mask.test||!std::isfinite(mask.request_scale)||mask.request_scale<=0||!std::isfinite(mask.cell_scale)||mask.cell_scale<=0)throw std::invalid_argument("Invalid Dust mask transform");
        const auto region=clip(bounds,image);if(!region.width||!region.height)return {};
        Raster staged=image;Kernel kernel(staged,image,region,type,size,white,budget,masks,companion);kernel.run();budget.poll();
        FillResult result;result.performed=kernel.performed;result.dirty=kernel.damage;result.storage_changed=kernel.allocated;
        if(kernel.modified){
            for(const auto& [tile,bytes]:staged.tiles){const bool before=image.tiles.contains(tile);const auto l=tile.x*256,t=tile.y*256,right=std::min(l+256,image.width),bottom=std::min(t+256,image.height);
                if(before&&image.tiles.at(tile)==bytes)continue;budget.work(std::uint64_t(right-l)*(bottom-t));
                for(int y=t;y<bottom;++y)for(int x=l;x<right;++x)result.changed_pixels+=(!before&&image.depth==24)||selection_detail::raw_pixel(image,x,y)!=selection_detail::raw_pixel(staged,x,y);
            }
            result.changed=result.changed_pixels!=0;budget.poll();image=std::move(staged);
        }
        return {std::move(result),kernel.modified,true};
    }catch(const Cancelled&){DustOutcome out;out.result.cancelled=true;return out;}
    catch(const std::exception& error){DustOutcome out;out.result.handled=false;out.result.error=error.what();return out;}
}
int dust_size(const DustSettings& value,double owner_dpi,double plane_dpi){
    if(int(value.mode)<0||int(value.mode)>2||!std::isfinite(value.size_mm)||value.size_mm<0||value.size_mm>1000||value.size_px<0||value.size_px>20000)
        throw std::invalid_argument("Invalid Dust settings");
    if(!std::isfinite(owner_dpi)||owner_dpi<=0||!std::isfinite(plane_dpi)||plane_dpi<=0)throw std::invalid_argument("Invalid Dust DPI");
    const double physical=value.use_millimeters?numeric_detail::binary64(numeric_detail::binary64(value.size_mm*plane_dpi)*0x1.42850a142850ap-5):
        numeric_detail::binary64(numeric_detail::binary64(double(value.size_px)/owner_dpi)*plane_dpi);
    const auto bounded=std::isfinite(physical)?std::clamp(physical,0.0,20000.0):20000.0;
    const auto size=value.use_millimeters?legacy_rt_double_to_int(bounded):std::optional<int>(int(bounded));
    if(!size)throw std::invalid_argument("Dust size conversion failed");return *size;
}
FilterResult apply_dust(Raster& image,const FilterSettings& settings,const std::optional<Rect>& bounds,
    const FilterExecutionOptions& options,std::stop_token stop,const std::optional<RasterSelection>& coverage,
    FilterCosts* shared,double owner_dpi,double plane_dpi,const Raster* companion){
    try{
        if(stop.stop_requested())return {FilterStatus::Cancelled,0,{}};
        const auto* value=std::get_if<DustSettings>(&settings);
        if(!value)throw std::invalid_argument("Invalid Dust settings");
        if(!owner_dpi)owner_dpi=image.resolution;if(!plane_dpi)plane_dpi=image.resolution;
        if(plane_dpi!=image.resolution)throw std::invalid_argument("Dust requires matching plane and offscreen DPI");
        if(coverage&&!coverage->has_materialized_mask())throw std::invalid_argument("Dust selection has no mask backing");
        const auto size=dust_size(*value,owner_dpi,plane_dpi);
        std::vector<DustMask> masks;
        if(coverage){selection_detail::MaskView view{*coverage,image.width,image.height};masks.push_back({view.width(),view.height(),view.offset(),1,1,[view](int x,int y){return view.test(x,y);}});}
        const auto outcome=dust_region(image,int(value->mode),size,options.white_is_transparent,bounds.value_or(Rect{0,0,image.width,image.height}),masks,stop,companion,shared);
        if(outcome.result.cancelled)return {FilterStatus::Cancelled,0,{}};
        if(!outcome.result.handled)return {FilterStatus::Rejected,0,outcome.result.error};
        return {outcome.worker_succeeded?FilterStatus::Applied:FilterStatus::Unchanged,outcome.result.changed_pixels,{}};
    }catch(const std::exception& error){return {FilterStatus::Rejected,0,error.what()};}
}
}
