#pragma once
#include "retas/raster_stroke_pixels.hpp"
#include "retas/fill.hpp"
#include <cstring>
namespace retas::selection_detail {
// Immutable access to the physical native mask carrier. Its cleanup domain
// can differ from logical selection bounds, notably for mixed-DPI AreaMask.
struct MaskView {
    const RasterSelection& value;int owner_width{},owner_height{};
    std::array<std::int64_t,2> offset() const {return value.mask_offset_;}
    const BinaryMask* carrier() const {return value.mask_.get();}
    int width() const {return value.masked_?(value.mask_?value.mask_->width():0):owner_width;}
    int height() const {return value.masked_?(value.mask_?value.mask_->height():0):owner_height;}
    bool test(int x,int y) const {
        if(value.masked_)return value.mask_&&value.mask_->test(x,y);
        const auto tx=std::int64_t(x)-value.mask_offset_[0],ty=std::int64_t(y)-value.mask_offset_[1];const auto& b=value.bounds_;
        return tx>=b.x&&ty>=b.y&&tx<std::int64_t(b.x)+b.width&&ty<std::int64_t(b.y)+b.height;
    }
};
inline std::uint64_t clear_word(int depth){return depth==24?0xffffffull:depth==32?0xffffffull:depth==64?0x0000ffffffffffffull:0;}
inline std::uint64_t raw_pixel(const Raster& raster,int x,int y){
    if(!raster.contains(x,y))return clear_word(raster.depth);
    const auto tile=raster.tiles.find({x/256,y/256});if(tile==raster.tiles.end())return clear_word(raster.depth);
    if(!tile->second||tile->second->size()!=raster.tile_bytes())throw std::invalid_argument("Invalid selection raster tile");
    const auto p=std::size_t(y%256)*256+x%256;const auto& bytes=*tile->second;
    if(raster.depth<8)return (bytes[p*raster.depth/8]>>(8-raster.depth-(p*raster.depth)%8))&((1u<<raster.depth)-1);
    std::uint64_t value{};for(int i=0;i<raster.depth/8;++i)value|=std::uint64_t(bytes[p*(raster.depth/8)+i])<<(8*i);return value;
}
inline void store_raw_pixel(Raster& raster,int x,int y,std::uint64_t value){
    if(!raster.contains(x,y))throw std::invalid_argument("Selection raster write outside its surface");
    auto& tile=raster.tiles[{x/256,y/256}];
    if(!tile){tile=std::make_shared<Bytes>(raster.tile_bytes(),0);const auto clear=clear_word(raster.depth);
        if(clear)for(std::size_t p=0;p<65536;++p)for(int i=0;i<raster.depth/8;++i)(*tile)[p*(raster.depth/8)+i]=std::uint8_t(clear>>(8*i));}
    else if(tile.use_count()!=1)tile=std::make_shared<Bytes>(*tile);
    if(tile->size()!=raster.tile_bytes())throw std::invalid_argument("Invalid selection raster tile");
    const auto p=std::size_t(y%256)*256+x%256;
    if(raster.depth<8){const unsigned shift=8-raster.depth-(p*raster.depth)%8,mask=((1u<<raster.depth)-1)<<shift;
        auto& b=(*tile)[p*raster.depth/8];b=std::uint8_t((b&~mask)|((unsigned(value)<<shift)&mask));}
    else for(int i=0;i<raster.depth/8;++i)(*tile)[p*(raster.depth/8)+i]=std::uint8_t(value>>(8*i));
}
inline bool transparent(const Raster& raster,int x,int y,bool white_key){
    // The guarded IsTransparent wrapper treats missing storage as transparent,
    // including24-bit whose backed Get24Bit predicate has no alpha channel.
    if(!raster.contains(x,y)||!raster.tiles.contains({x/256,y/256}))return true;
    const auto raw=raw_pixel(raster,x,y);
    switch(raster.depth){
    case 1:return !raw;case 2:return white_key?raw!=3:raw==0;case 8:return !raw;case 16:return !(raw&255);case 24:return white_key&&raw==0xffffff;
    case 32:return !(raw>>24)||(white_key&&(raw&0xffffff)==0xffffff);
    case 64:return !(raw>>48)||(white_key&&(raw&0xffffffffffffull)==0xffffffffffffull);
    default:throw std::invalid_argument("Unsupported selection transparency depth");
    }
}
}
