#include "retas/resolution_raster.hpp"
#include "retas/numeric.hpp"
#include "resolution_raster_internal.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <vector>

namespace retas {
namespace resolution_detail {
constexpr std::uint64_t storage_limit = 512ull * 1024 * 1024;
constexpr std::uint64_t work_limit = 128ull * 1024 * 1024;
    void Budget::poll() const {
        if (stop.stop_requested()) throw std::runtime_error("Resolution raster cancelled");
    }
    void Budget::allocate(std::uint64_t n) {
        if (n > storage_limit - bytes)
            throw std::length_error("Resolution raster storage exceeds 512 MiB");
        bytes += n;
        poll();
    }
    void Budget::visit(std::uint64_t n) {
        if (n > work_limit - work)
            throw std::length_error("Resolution raster work exceeds 128 Mi");
        work += n;
        poll();
    }
}
namespace {
using numeric_detail::binary64;
using resolution_detail::Budget;
void dimensions(int w, int h) {
    if (w <= 0 || h <= 0 || w > 100000 || h > 100000 ||
        std::int64_t(w) * h > 268435456)
        throw std::invalid_argument("Resolution raster dimensions unsupported");
}
void validate(const Raster& r, Budget& budget) {
    dimensions(r.width, r.height);
    const bool color = r.depth == 32 || r.depth == 64;
    const bool rgb = r.depth == 24;
    if ((r.depth != 1 && r.depth != 2 && r.depth != 8 && r.depth != 16 && !rgb && !color) ||
        r.color_order != (color ? 3 : rgb ? 4 : 0) || !std::isfinite(r.resolution) || r.resolution <= 0)
        throw std::invalid_argument("Resolution raster requires native normal offscreen storage");
    budget.allocate(sizeof(Raster));
    for (const auto& [p, backing] : r.tiles) {
        budget.poll();
        if (p.x < 0 || p.y < 0 || std::int64_t(p.x) * 256 >= r.width ||
            std::int64_t(p.y) * 256 >= r.height || !backing ||
            backing->size() != r.tile_bytes())
            throw std::invalid_argument("Invalid resolution source tile");
        budget.allocate(backing->size() + 128);
    }
}
Rect clipped(const Raster& r, Rect rect) {
    if (rect.width < 0 || rect.height < 0 ||
        std::int64_t(rect.x) + rect.width > std::numeric_limits<int>::max() ||
        std::int64_t(rect.y) + rect.height > std::numeric_limits<int>::max())
        throw std::invalid_argument("Invalid resolution source rectangle");
    const int left = std::max(0, rect.x), top = std::max(0, rect.y);
    const int right = std::min(r.width, rect.x + rect.width);
    const int bottom = std::min(r.height, rect.y + rect.height);
    if (left >= right || top >= bottom) return {};
    return {left, top, right - left, bottom - top};
}
unsigned packed(const Bytes& b,std::size_t pixel,int depth) {
    return (b[pixel*depth/8] >> (8-depth-(pixel*depth)%8)) & ((1u<<depth)-1);
}
void packed(Bytes& b,std::size_t pixel,int depth,unsigned value) {
    const unsigned shift=8-depth-(pixel*depth)%8, mask=((1u<<depth)-1)<<shift;
    auto& v=b[pixel*depth/8];v=std::uint8_t((v&~mask)|(value<<shift));
}
// General24 uses Get24Bit (missing=white); shortcut24 uses Get32Bit
// (missing=zero). General2 accumulates a black indicator, while shortcut2
// accumulates converted white RGB. Neither follows Raster::pixel coverage.
std::array<std::uint32_t, 4> raw(const Raster& r, int x, int y,bool shortcut=false) {
    if(!r.contains(x,y))return {};
    const auto it=r.tiles.find({x/256,y/256});
    if(it==r.tiles.end())return r.depth==24&&!shortcut?std::array<std::uint32_t,4>{255,255,255,255}:std::array<std::uint32_t,4>{};
    const auto p=std::size_t(y%256)*256+x%256;const auto& b=*it->second;
    if(r.depth==1)return shortcut?std::array<std::uint32_t,4>{0,0,0,packed(b,p,1)*255}:std::array<std::uint32_t,4>{packed(b,p,1),0,0,0};
    if(r.depth==2){const auto v=packed(b,p,2);const unsigned gray=shortcut?(v==2?255:0):(v==3?1:0),alpha=v>=2?255:0;return {gray,gray,gray,alpha};}
    if(r.depth==8)return {b[p],b[p],b[p],0};
    if(r.depth==16)return {b[p*2+1],b[p*2+1],b[p*2+1],b[p*2]};
    if(r.depth==24)return {b[p*3+2],b[p*3+1],b[p*3],255};
    if(r.depth==32)return {b[p*4+2],b[p*4+1],b[p*4],b[p*4+3]};
    const auto word=[&](std::size_t n){return unsigned(b[n])|(unsigned(b[n+1])<<8);};
    return {word(p*8+4),word(p*8+2),word(p*8),word(p*8+6)};
}
void ensure_tile(Raster& r, TileCoord p) {
    if (r.tiles.contains(p)) return;
    auto backing = std::make_shared<Bytes>(r.tile_bytes(), 0);
    if(r.depth==24)std::fill(backing->begin(),backing->end(),255);
    if(r.depth==32||r.depth==64){const unsigned stride=unsigned(r.depth/8);
        for(std::size_t i=0;i<backing->size();i+=stride)std::fill_n(backing->begin()+std::ptrdiff_t(i),stride*3/4,255);}
    r.tiles.emplace(p, std::move(backing));
}
void put(Raster& r, int x, int y, const std::array<std::uint32_t, 4>& c,bool general=false) {
    ensure_tile(r,{x/256,y/256});auto& b=*r.tiles.at({x/256,y/256});const auto p=std::size_t(y%256)*256+x%256;
    if(r.depth==1){packed(b,p,1,c[3]>=128);return;}
    if(r.depth==2){packed(b,p,2,general?(c[0]>=127?3:0):(c[3]<=127?0:c[0]<127?3:2));return;}
    if(r.depth==8){b[p]=std::uint8_t(c[0]);return;}
    if(r.depth==16){b[p*2]=std::uint8_t(c[3]);b[p*2+1]=std::uint8_t((c[2]+4*c[1]+2*c[0])/7);return;}
    const int stride=r.depth/8;
    for(int lane=0;lane<(r.depth==24?3:4);++lane){const auto v=c[lane==0?2:lane==2?0:lane];const auto offset=p*stride+lane*(r.depth==64?2:1);b[offset]=std::uint8_t(v);if(r.depth==64)b[offset+1]=std::uint8_t(v>>8);}
}
// ChangeResolution2 chooses the destination's SDK class before sampling the
// original source. Cross-depth conversion therefore belongs to getters/setters,
// not to a preconverted source image.
std::array<std::uint32_t,4> native_shortcut_sample(const Raster& source,int x,int y,int depth){
    auto c=raw(source,x,y,true);
    if(source.depth==8){c={0,0,0,c[0]};}
    if(source.depth==64&&depth!=64)for(auto& v:c)v>>=8;
    else if(source.depth!=64&&depth==64)for(auto& v:c)v*=257;
    return c;
}
void native_put(Raster& out,int x,int y,std::array<std::uint32_t,4> c,bool general){
    if(out.depth<=8){
        unsigned value{};
        if(general)value=out.depth==1?unsigned(c[0]>127):out.depth==2?unsigned(c[0]>=127?3:0):c[0];
        else{const auto gray=(c[0]+4*c[1]+2*c[2])/7,ink=((255-gray)*(c[3]+1))>>8;
            value=out.depth==1?unsigned(ink>=128):out.depth==2?unsigned(c[3]<=127?0:gray<127?3:2):ink;}
        ensure_tile(out,{x/256,y/256});auto& b=*out.tiles.at({x/256,y/256});const auto p=std::size_t(y%256)*256+x%256;
        if(out.depth==8)b[p]=std::uint8_t(value);else packed(b,p,out.depth,value);return;
    }
    if(out.depth==16){const auto gray=(c[0]+4*c[1]+2*c[2])/7;c[0]=c[1]=c[2]=gray;}
    put(out,x,y,c);
}
struct Area {
    int begin{}, end{};
    double first{}, last{};
    double weight(int index) const {
        if (index == begin) return first;
        return index == end ? last : 1.0;
    }
};
Area area(double left, double right) {
    Area result{int(left), int(right), 0, 0};
    if (result.begin == result.end) {
        result.first = binary64(right - left);
        return result;
    }
    result.first = binary64(1.0 - binary64(left - double(result.begin)));
    result.last = binary64(right - double(result.end));
    if (result.last == 0) {
        --result.end;
        result.last = result.end == result.begin ? 0.0 : 1.0;
    }
    return result;
}
std::vector<Area> areas(int count, double step, Budget& budget, std::uint64_t& visits) {
    budget.allocate(std::uint64_t(count) * sizeof(Area));
    std::vector<Area> result;
    result.reserve(std::size_t(count));
    double current = 0;
    for (int i = 0; i < count; ++i) {
        budget.poll();
        const double next = binary64(current + step);
        auto value = area(current, next);
        visits += std::uint64_t(value.end - value.begin + 1);
        result.push_back(value);
        current = next;
    }
    return result;
}
void general(const Raster& source, Rect rect, Raster& out, Budget& budget,bool native=false) {
    //The native 64-bit destination general worker admits only 64-bit sources.
    if(native&&out.depth==64&&source.depth!=64)return;
    const double step_x = binary64(double(rect.width) / out.width);
    const double step_y = binary64(double(rect.height) / out.height);
    const double normalization = binary64(1.0 / binary64(step_y * step_x));
    std::uint64_t nx{}, ny{};
    const auto xs = areas(out.width, step_x, budget, nx);
    const auto ys = areas(out.height, step_y, budget, ny);
    budget.visit(nx * ny + std::uint64_t(out.width) * out.height);
    // Mode 1's destination write-lock allocates its initial cursor block.
    ensure_tile(out, {0, 0});
    const int maximum = out.depth == 64 ? 65535 : 255;
    for (int y = 0; y < out.height; ++y) {
        budget.poll();
        const auto& ay = ys[std::size_t(y)];
        for (int x = 0; x < out.width; ++x) {
            const auto& ax = xs[std::size_t(x)];
            std::array<double, 4> total{};
            for (int sy = ay.begin; sy <= ay.end; ++sy) {
                budget.poll();
                std::array<double, 4> row{};
                for (int sx = ax.begin; sx <= ax.end; ++sx) {
                    if ((sx & 4095) == 0) budget.poll();
                    auto c = raw(source, rect.x + sx, rect.y + sy);
                    if(native&&source.depth==1)c={c[0]*255,c[0]*255,c[0]*255,0};
                    if(native&&source.depth==2)for(int i=0;i<3;++i)c[i]*=255;
                    if(native&&source.depth==24)c[3]=0;
                    const double weight = ax.weight(sx);
                    for (std::size_t channel = 0; channel < row.size(); ++channel)
                        row[channel] = binary64(binary64(double(c[channel]) * weight) + row[channel]);
                }
                const double weight = ay.weight(sy);
                for (std::size_t channel = 0; channel < total.size(); ++channel)
                    total[channel] = binary64(binary64(row[channel] * weight) + total[channel]);
            }
            std::array<std::uint32_t, 4> pixel{};
            for (std::size_t channel = 0; channel < pixel.size(); ++channel) {
                double value_scaled=binary64(total[channel]*normalization);
                if(!native&&out.depth==2&&channel<3)value_scaled=binary64(value_scaled*255.0);
                const auto value = legacy_rt_double_to_int(value_scaled);
                if (!value || *value < 0)
                    throw std::invalid_argument("Resolution channel outside numeric domain");
                pixel[channel] = std::uint32_t(std::min(*value, maximum));
            }
            //64-bit source's inherited +174 slot is an empty stub; the caller's
            //packed output local retains the clipped source-height initializer.
            if(native&&source.depth==64&&out.depth!=64){const auto v=unsigned(rect.height);pixel={v&255,(v>>8)&255,(v>>16)&255,(v>>24)&255};}
            if(native)native_put(out,x,y,pixel,true);else put(out, x, y, pixel,true);
        }
    }
}
void shortcut(const Raster& source, Rect rect, Raster& out, int factor, Budget& budget,bool native=false) {
    ensure_tile(out, {0, 0});
    for (const auto& [tile, backing] : source.tiles) {
        (void)backing;
        budget.poll();
        const int left = std::max(rect.x, tile.x * 256);
        const int top = std::max(rect.y, tile.y * 256);
        const int right = std::min(rect.x + rect.width, (tile.x + 1) * 256);
        const int bottom = std::min(rect.y + rect.height, (tile.y + 1) * 256);
        if (left >= right || top >= bottom) continue;
        // Each backed block is independently truncated onto the destination
        // grid. A sample at a block edge can include an adjacent sparse hole.
        const int dx = (left - rect.x) / factor, dy = (top - rect.y) / factor;
        const int dr = (right - rect.x) / factor, db = (bottom - rect.y) / factor;
        budget.visit(std::uint64_t(dr - dx) * std::uint64_t(db - dy) *
                     std::uint64_t(factor * factor + 1));
        for (int y = dy; y < db; ++y) {
            budget.poll();
            for (int x = dx; x < dr; ++x) {
                std::array<std::uint32_t, 4> sum{};
                for (int sy = 0; sy < factor; ++sy)
                    for (int sx = 0; sx < factor; ++sx) {
                        const auto c = native&&!(source.depth==8&&out.depth==8)?native_shortcut_sample(source,rect.x+x*factor+sx,rect.y+y*factor+sy,out.depth):raw(source, rect.x + x * factor + sx,rect.y + y * factor + sy,true);
                        for (std::size_t channel = 0; channel < sum.size(); ++channel)
                            sum[channel] += c[channel];
                    }
                for (auto& channel : sum) channel >>= factor == 2 ? 2 : 4;
                if(native&&!(source.depth==8&&out.depth==8))native_put(out,x,y,sum,false);else put(out, x, y, sum);
            }
        }
    }
}
void enlarge(const Raster& source,Rect rect,Raster& out,int factor,Budget& budget){
    for(int y=0;y<out.height;++y){budget.poll();for(int x=0;x<out.width;++x){
        budget.visit(1);const int sx=rect.x+x/factor,sy=rect.y+y/factor;
        if(!source.contains(sx,sy)||!source.tiles.contains({sx/256,sy/256}))continue;
        if(source.depth==8&&out.depth==8)put(out,x,y,raw(source,sx,sy,true));else native_put(out,x,y,native_shortcut_sample(source,sx,sy,out.depth),false);
    }}
}
void identity(const Raster& source, Rect rect, Raster& out, Budget& budget) {
    if (rect.x == 0 && rect.y == 0 && rect.width == source.width &&
        rect.height == source.height) {
        for (const auto& [tile, backing] : source.tiles) {
            budget.visit(256 * 256);
            out.tiles.emplace(tile, std::make_shared<Bytes>(*backing));
        }
        return;
    }
    budget.visit(std::uint64_t(rect.width) * rect.height);
    std::vector<TileCoord> empty_blocks;
    const bool aligned = rect.x % 256 == 0 && rect.y % 256 == 0;
    for (int ty = rect.y / 256; ty <= (rect.y + rect.height - 1) / 256; ++ty) {
        for (int tx = rect.x / 256; tx <= (rect.x + rect.width - 1) / 256; ++tx) {
            budget.poll();
            const int left = std::max(rect.x, tx * 256);
            const int top = std::max(rect.y, ty * 256);
            const int right = std::min(rect.x + rect.width, (tx + 1) * 256);
            const int bottom = std::min(rect.y + rect.height, (ty + 1) * 256);
            const bool present = source.tiles.contains({tx, ty});
            if (!present && (source.depth==1||source.depth==2||source.depth==8||source.depth==32) && aligned &&
                right - left == 256 && bottom - top == 256) {
                budget.allocate(sizeof(TileCoord));
                empty_blocks.push_back({(left - rect.x) / 256, (top - rect.y) / 256});
                continue;
            }
            for (int y = top; y < bottom; ++y) {
                budget.poll();
                for (int x = left; x < right; ++x) {
                    const int dx=x-rect.x,dy=y-rect.y;
                    ensure_tile(out,{dx/256,dy/256});
                    if(present){
                        auto& dest=*out.tiles.at({dx/256,dy/256});const auto& src=*source.tiles.at({tx,ty});
                        const auto sp=std::size_t(y%256)*256+x%256,dp=std::size_t(dy%256)*256+dx%256;
                        if(out.depth<8){auto value=packed(src,sp,out.depth);if(out.depth==2&&rect.x%256!=0&&value==1)value=0;packed(dest,dp,out.depth,value);}
                        else std::copy_n(src.begin()+std::ptrdiff_t(sp*(out.depth/8)),out.depth/8,dest.begin()+std::ptrdiff_t(dp*(out.depth/8)));
                    }
                }
            }
        }
    }
    // Copy32 destroys complete aligned missing blocks. Its cursor can allocate
    // a following row's block before reaching that block's copy job; erasing
    // after all jobs preserves the final native topology under that ordering.
    for (auto tile : empty_blocks) out.tiles.erase(tile);
}
int mapped(int relative,int numerator,int denominator) {
    const auto product=std::int64_t(relative)*numerator;
    // Native mode0 uses imul/idiv in signed32, not floating-point ratios.
    // Overflowing that native arithmetic domain is rejected before division.
    if(product<INT32_MIN||product>INT32_MAX)throw std::overflow_error("Resolution integer mapping exceeds int32");
    return int(product/denominator);
}
void low(const Raster& source,Rect rect,Raster& out,Budget& budget,bool native_color=false) {
    ensure_tile(out,{0,0});
    for(int ty=0;ty<out.height;ty+=256)for(int tx=0;tx<out.width;tx+=256){
        budget.visit(1);const int tr=std::min(tx+256,out.width),tb=std::min(ty+256,out.height);
        const int rl=rect.x+mapped(tx,rect.width,out.width),rt=rect.y+mapped(ty,rect.height,out.height);
        const int rr=rect.x+mapped(tr,rect.width,out.width),rb=rect.y+mapped(tb,rect.height,out.height);
        if(rl>=rr||rt>=rb)continue;
        for(int sy=rt/256;sy<=(rb-1)/256;++sy)for(int sx=rl/256;sx<=(rr-1)/256;++sx){
            budget.visit(1);if(!source.tiles.contains({sx,sy}))continue;
            const int ul=std::max(rl,sx*256),ut=std::max(rt,sy*256),ur=std::min(rr,sx*256+256),ub=std::min(rb,sy*256+256);
            const int vl=std::max(tx,mapped(ul-rect.x,out.width,rect.width)),vt=std::max(ty,mapped(ut-rect.y,out.height,rect.height));
            const int vr=ur==rr?tr:mapped(ur-rect.x,out.width,rect.width),vb=ub==rb?tb:mapped(ub-rect.y,out.height,rect.height);
            if(vl>=vr||vt>=vb)continue;
            ensure_tile(out,{vl/256,vt/256});
            int row_start=ut;
            for(int y=vt;y<vb;++y){budget.poll();const int row_end=ut+mapped(y-vt+1,ub-ut,vb-vt),sample_end_y=std::max(row_end,row_start+1);int col_start=ul;
                for(int x=vl;x<vr;++x){
                    const int col_end=ul+mapped(x-vl+1,ur-ul,vr-vl),sample_end_x=std::max(col_end,col_start+1);
                    const auto samples=std::uint64_t(sample_end_x-col_start)*(sample_end_y-row_start);budget.visit(samples+1);
                    std::uint64_t black{};std::array<std::uint64_t,4> sum{};
                    for(int yy=row_start;yy<sample_end_y;++yy){budget.poll();for(int xx=col_start;xx<sample_end_x;++xx){
                        if(native_color){const auto c=native_shortcut_sample(source,xx,yy,out.depth);for(int i=0;i<4;++i)sum[i]+=c[i];}
                        else black+=raw(source,xx,yy)[0];}}
                    // V fits in the current destination tile; FastestNextX
                    // neither allocates nor prefetches its neighbor.
                    if(native_color){std::array<std::uint32_t,4> c{};for(int i=0;i<4;++i)c[i]=std::uint32_t(sum[i]/samples);native_put(out,x,y,c,false);}
                    else{auto& backing=*out.tiles.at({x/256,y/256});packed(backing,std::size_t(y%256)*256+x%256,1,black>samples-black?1:0);}
                    col_start=col_end;
                }row_start=row_end;
            }
        }
    }
}
}
Raster resolution_detail::resample(const Raster& source,Rect source_rectangle,int width,int height,double dpi,Budget& budget,bool change_resolution2,int target_depth,bool mode_zero) {
    budget.poll();
    validate(source, budget);
    dimensions(width, height);
    if (!std::isfinite(dpi) || dpi <= 0)
        throw std::invalid_argument("Invalid target resolution");
    const auto rect = clipped(source, source_rectangle);
    // Reserve the complete output surface, including map-node overhead, before
    // allocating any backing; failure and cancellation expose no partial result.
    const auto blocks = std::uint64_t((width + 255) / 256) * ((height + 255) / 256);
    Raster result(width, height, target_depth?target_depth:source.depth, dpi);
    budget.allocate(sizeof(Raster) + blocks * (result.tile_bytes() + 128));
    if (!rect.width || !rect.height) return result;
    if(change_resolution2){
        //105474D0 tests approximate integer shortcuts before any identity or
        //mode dispatch, and each shortcut receives origins plus output size.
        if(std::abs(4*rect.width-width)<=4&&std::abs(4*rect.height-height)<=4)enlarge(source,rect,result,4,budget);
        else if(std::abs(2*rect.width-width)<=2&&std::abs(2*rect.height-height)<=2)enlarge(source,rect,result,2,budget);
        else if(std::abs(rect.width-2*width)<=2&&std::abs(rect.height-2*height)<=2)shortcut(source,clipped(source,{rect.x,rect.y,2*width,2*height}),result,2,budget,true);
        else if(std::abs(rect.width-4*width)<=4&&std::abs(rect.height-4*height)<=4)shortcut(source,clipped(source,{rect.x,rect.y,4*width,4*height}),result,4,budget,true);
        else if(mode_zero)low(source,rect,result,budget,source.depth!=1||result.depth!=1);
        else general(source,rect,result,budget,true);
        return result;
    }
    if (rect.width == width && rect.height == height)
        identity(source, rect, result, budget);
    else if(source.depth==1)
        low(source,rect,result,budget);
    else if (rect.width==2*width && rect.height==2*height)
        shortcut(source, rect, result, 2, budget);
    else if (rect.width==4*width && rect.height==4*height)
        shortcut(source, rect, result, 4, budget);
    else
        general(source, rect, result, budget);
    budget.poll();
    return result;
}
Raster resolution_raster(const Raster& source,Rect source_rectangle,int width,int height,double dpi,std::stop_token stop) {
    if(source_rectangle.width<=0||source_rectangle.height<=0)throw std::invalid_argument("Invalid resolution source rectangle");
    Budget budget{0,0,stop};return resolution_detail::resample(source,source_rectangle,width,height,dpi,budget);
}
}
