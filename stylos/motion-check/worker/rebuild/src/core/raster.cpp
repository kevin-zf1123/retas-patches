#include "retas/raster.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <zlib.h>

namespace retas {
namespace {
std::uint16_t word(const Bytes& b, std::size_t n) {
    return std::uint16_t(unsigned(b.at(n)) | (unsigned(b.at(n + 1)) << 8));
}
void put_word(Bytes& b, std::size_t n, std::uint16_t v) {
    b[n] = std::uint8_t(v); b[n + 1] = std::uint8_t(v >> 8);
}
Bytes empty_tile(int depth, std::size_t size) {
    Bytes bytes(size, 0);
    if (depth == 32) for (std::size_t i = 0; i < size; i += 4) bytes[i] = bytes[i+1] = bytes[i+2] = 255;
    if (depth == 64) for (std::size_t i = 0; i < size; i += 8) std::fill_n(bytes.begin()+i, 6, 255);
    return bytes;
}
}
Raster::Raster(int w, int h, int d, double dpi) : width(w), height(h), depth(d), resolution(dpi) {
    if (w <= 0 || h <= 0 || w > 100000 || h > 100000 || std::int64_t(w) * h > 268435456)
        throw std::invalid_argument("Image dimensions exceed supported limits");
    if (d != 1 && d != 2 && d != 8 && d != 16 && d != 24 && d != 32 && d != 64)
        throw std::invalid_argument("Unsupported image depth");
    if (!std::isfinite(dpi) || dpi <= 0) throw std::invalid_argument("Invalid image resolution");
    color_order = d == 24 ? 4 : (d == 32 || d == 64 ? 3 : 0);
}
std::size_t Raster::tile_bytes() const { return std::size_t(tile_side) * tile_side * depth / 8; }
Pixel Raster::pixel(int x, int y) const {
    if (!contains(x, y)) return {};
    auto t = tiles.find({x / tile_side, y / tile_side});
    if (t == tiles.end()) return {};
    const auto& b = *t->second;
    const std::size_t p = std::size_t(y % tile_side) * tile_side + x % tile_side;
    if (depth == 32 && color_order == 3) return Pixel::rgba8(b.at(p*4+2), b.at(p*4+1), b.at(p*4), b.at(p*4+3));
    if (depth == 64 && color_order == 3) return {word(b,p*8+4),word(b,p*8+2),word(b,p*8),word(b,p*8+6)};
    if (depth == 24 && color_order == 4) return Pixel::rgba8(b.at(p*3+2),b.at(p*3+1),b.at(p*3));
    // Low-depth material semantics depend on the owning plane; this accessor
    // exposes coverage only. Its editing adapters are implemented separately.
    unsigned value = 0, maximum = 255;
    if (depth == 8) value = b.at(p);
    else if (depth == 16) { value = word(b,p*2); maximum = 65535; }
    else { maximum = (1u << depth) - 1; value = (b.at(p*depth/8) >> (8-depth-(p*depth)%8)) & maximum; }
    return {0,0,0,static_cast<std::uint16_t>(std::uint64_t(value)*65535/maximum)};
}
void Raster::set_pixel(int x, int y, Pixel c) {
    if (!contains(x,y)) return;
    if (!can_edit()) throw std::logic_error("This raster requires a plane-specific editing adapter");
    auto& tile = tiles[{x/tile_side,y/tile_side}];
    if (!tile) tile = std::make_shared<Bytes>(empty_tile(depth,tile_bytes()));
    else if (tile.use_count() != 1) tile = std::make_shared<Bytes>(*tile);
    const auto p = std::size_t(y%tile_side)*tile_side+x%tile_side;
    auto& b = *tile;
    if (depth == 32) {
        b[p*4] = c.b/257; b[p*4+1] = c.g/257; b[p*4+2] = c.r/257; b[p*4+3] = c.a/257;
    } else {
        put_word(b,p*8,c.b); put_word(b,p*8+2,c.g); put_word(b,p*8+4,c.r); put_word(b,p*8+6,c.a);
    }
}
Pixel over(Pixel d, Pixel s, std::uint16_t opacity) {
    const std::uint64_t sa = std::uint64_t(s.a)*opacity/65535;
    const std::uint64_t da = std::uint64_t(d.a)*(65535-sa)/65535;
    const auto a = sa+da;
    if (!a) return {};
    auto channel = [&](std::uint16_t dc, std::uint16_t sc) { return std::uint16_t((std::uint64_t(sc)*sa+std::uint64_t(dc)*da)/a); };
    return {channel(d.r,s.r),channel(d.g,s.g),channel(d.b,s.b),std::uint16_t(a)};
}
void Raster::blend_pixel(int x,int y,Pixel c,std::uint16_t coverage) { set_pixel(x,y,over(pixel(x,y),c,coverage)); }
void Raster::erase_pixel(int x,int y,std::uint16_t coverage) {
    auto c=pixel(x,y); c.a=std::uint16_t(std::uint64_t(c.a)*(65535-coverage)/65535);
    if (!c.a) c={}; set_pixel(x,y,c);
}
bool Raster::same_pixels(const Raster& other) const {
    if (width!=other.width || height!=other.height || depth!=other.depth) return false;
    // Only the union of allocated tiles can differ. In particular, a new
    // tile at the far edge must not make a one-pixel edit scan the canvas.
    // Compare logical samples for differing backing: edge padding is ignored,
    // and an allocated transparent tile need not equal a missing tile in all
    // formats (pixel() also exposes hidden RGB).
    auto i=tiles.begin(),j=other.tiles.begin();
    while(i!=tiles.end()||j!=other.tiles.end()){
        const auto key=j==other.tiles.end()||(i!=tiles.end()&&i->first<j->first)?i->first:j->first;
        const bool left=i!=tiles.end()&&i->first==key,right=j!=other.tiles.end()&&j->first==key;
        const bool equal=left&&right&&color_order==other.color_order
            &&(i->second==j->second||*i->second==*j->second);
        if(!equal){
            const auto x0=std::int64_t(key.x)*tile_side,y0=std::int64_t(key.y)*tile_side;
            const auto x1=std::min<std::int64_t>(width,x0+tile_side),y1=std::min<std::int64_t>(height,y0+tile_side);
            for(auto y=std::max<std::int64_t>(0,y0);y<y1;++y)
                for(auto x=std::max<std::int64_t>(0,x0);x<x1;++x)
                    if(pixel(int(x),int(y))!=other.pixel(int(x),int(y)))return false;
        }
        if(left)++i;if(right)++j;
    }
    return true;
}
void Raster::prune_empty() {
    if(depth==1){
        // RCVOffscreen1Bit::Optimize tests logical bits only at raster edges.
        // A surviving block keeps its complete backing, including tail bits.
        std::erase_if(tiles,[&](const auto& item){
            const auto left=std::int64_t(item.first.x)*tile_side;
            const auto top=std::int64_t(item.first.y)*tile_side;
            if(left<0||top<0||left>=width||top>=height||!item.second||item.second->size()!=tile_bytes())
                throw std::logic_error("Invalid one-bit raster tile");
            const int columns=int(std::min<std::int64_t>(tile_side,std::int64_t(width)-left));
            const int rows=int(std::min<std::int64_t>(tile_side,std::int64_t(height)-top));
            const int whole_bytes=columns/8,tail_bits=columns%8;
            for(int y=0;y<rows;++y){
                const auto row=std::size_t(y)*32;
                for(int x=0;x<whole_bytes;++x)if((*item.second)[row+x])return false;
                if(tail_bits&&((*item.second)[row+whole_bytes]&(0xffu<<(8-tail_bits))))return false;
            }
            return true;
        });
        return;
    }
    auto empty=empty_tile(depth,tile_bytes());
    std::erase_if(tiles,[&](const auto& item){return *item.second==empty;});
}
bool Raster::optimize_storage(){
    if(width<=0||height<=0||width>100000||height>100000||std::int64_t(width)*height>268435456
       ||(depth!=1&&depth!=2&&depth!=8&&depth!=16&&depth!=24&&depth!=32&&depth!=64))
        throw std::invalid_argument("Invalid native Optimize raster");
    if((depth==24&&color_order!=4)||((depth==32||depth==64)&&color_order!=3))return false;
    // Validate the complete source first, so malformed later nodes cannot
    // cause a partially optimized live raster. No backing is ever mutated.
    for(const auto& [key,backing]:tiles)
        if(key.x<0||key.y<0||std::int64_t(key.x)*256>=width||std::int64_t(key.y)*256>=height||!backing||backing->size()!=tile_bytes())
            throw std::invalid_argument("Invalid native Optimize tile");
    const auto count=tiles.size();
    std::erase_if(tiles,[&](const auto& item){
        const int columns=std::min(256,width-item.first.x*256),rows=std::min(256,height-item.first.y*256);
        const auto& b=*item.second;
        // Optimize2's full-tile path ignores code1, but its logical-edge path
        // tests the entire two-bit code. This is an intentional native branch.
        if(depth==2&&columns==256&&rows==256){for(auto value:b)if(value&0xaa)return false;return true;}
        for(int y=0;y<rows;++y)for(int x=0;x<columns;++x){
            const auto p=std::size_t(y)*256+x;
            if(depth<8){if((b[p*depth/8]>>(8-depth-(p*depth)%8))&((1u<<depth)-1))return false;}
            else if(depth==8){if(b[p])return false;}
            else if(depth==16){if(b[p*2])return false;}
            else if(depth==24){if(b[p*3]||b[p*3+1]||b[p*3+2])return false;}
            else if(depth==32){if(b[p*4+3])return false;}
            else if(b[p*8+6]||b[p*8+7])return false;
        }
        return true;
    });
    return count!=tiles.size();
}
Raster decode_offscreen(Reader& parent) {
    auto outer=parent.section(); auto header=outer.section();
    const auto version=header.u16();
    if((version>>8)>1)throw FormatError(header.offset(),"Unsupported offscreen version");
    const auto w=header.u32(),h=header.u32(),d=header.u32();
    const auto dpi=header.f64(); const auto count=header.u32(); const auto order=header.u8();
    Raster image(static_cast<int>(w),static_cast<int>(h),static_cast<int>(d),dpi);
    image.color_order=order;
    if(count>100000)throw FormatError(header.offset(),"Too many image tiles");
    if(!count)return image;
    auto extent=outer.section();
    const auto left=extent.i32(),top=extent.i32(),right=extent.i32(),bottom=extent.i32();
    const bool compressed=extent.u8()!=0;
    if(left<0||top<0||right<=left||bottom<=top||left%256||top%256||
       right>image.width+255||bottom>image.height+255)
        throw FormatError(extent.offset(),"Invalid image tile extent");
    const std::int64_t cols=(std::int64_t(right)-left+255)/256,rows=(std::int64_t(bottom)-top+255)/256;
    if(cols*rows!=count)throw FormatError(extent.offset(),"Tile count/extent mismatch");
    auto blocks=outer.section(); std::size_t total=0;
    for(int y=top;y<bottom;y+=256)for(int x=left;x<right;x+=256){
        const auto present=blocks.u8(); if(!present)continue;
        if(present!=1)throw FormatError(blocks.offset(),"Invalid tile presence marker");
        const auto rawSize=blocks.u32();
        if(rawSize!=image.tile_bytes() || (total+=rawSize)>512ull*1024*1024)
            throw FormatError(blocks.offset(),"Invalid raster allocation size");
        Bytes bytes;
        if(compressed){
            auto encoded=blocks.take(blocks.u32()); bytes.resize(rawSize);
            uLongf size=rawSize; uLong used=static_cast<uLong>(encoded.size());
            const int result=uncompress2(bytes.data(),&size,encoded.data(),&used);
            if(result!=Z_OK || size!=rawSize || used!=encoded.size())
                throw FormatError(blocks.offset(),"Invalid compressed tile");
        }else bytes=blocks.bytes(rawSize);
        image.tiles.emplace(TileCoord{x/256,y/256},std::make_shared<Bytes>(std::move(bytes)));
    }
    return image;
}
Bytes encode_offscreen(const Raster& source,int compression_level) {
    if(compression_level < -1 || compression_level > 9)throw std::invalid_argument("Invalid native compression level");
    // Canonical encoding includes the native raster-plane save wrapper's
    // Optimize step; the lower-level native shared Write does not prune.
    Raster image=source; image.optimize_storage();
    Writer outer,header;
    int left=0,top=0,right=0,bottom=0;
    if(!image.tiles.empty()){
        left=image.width;top=image.height;
        for(const auto& [pos,tile]:image.tiles){
            left=std::min(left,pos.x*256);top=std::min(top,pos.y*256);
            right=std::max(right,std::min(image.width,(pos.x+1)*256));
            bottom=std::max(bottom,std::min(image.height,(pos.y+1)*256));
        }
    }
    const auto count=std::uint32_t(((right-left+255)/256)*((bottom-top+255)/256));
    header.u16(0x100);header.u32(image.width);header.u32(image.height);header.u32(image.depth);
    header.f64(image.resolution);header.u32(count);header.u8(image.color_order);outer.section(header.data);
    if(count){
        Writer extent,blocks; extent.i32(left);extent.i32(top);extent.i32(right);extent.i32(bottom);extent.u8(compression_level!=0);
        outer.section(extent.data);
        for(int y=top;y<bottom;y+=256)for(int x=left;x<right;x+=256){
            const auto found=image.tiles.find({x/256,y/256});blocks.u8(found!=image.tiles.end());
            if(found==image.tiles.end())continue;
            const auto& bytes=*found->second;
            if(bytes.size()!=image.tile_bytes())throw std::logic_error("Invalid in-memory tile");
            if(compression_level==0){blocks.u32(static_cast<std::uint32_t>(bytes.size()));blocks.raw(bytes);continue;}
            uLongf size=compressBound(static_cast<uLong>(bytes.size()));Bytes output(size);
            if(compress2(output.data(),&size,bytes.data(),static_cast<uLong>(bytes.size()),compression_level)!=Z_OK)
                throw std::runtime_error("Tile compression failed");
            output.resize(size);blocks.u32(static_cast<std::uint32_t>(bytes.size()));
            blocks.u32(static_cast<std::uint32_t>(output.size()));blocks.raw(output);
        }
        outer.section(blocks.data);
    }
    Writer result;result.section(outer.data);return result.data;
}
}
