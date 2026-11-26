#pragma once
#include "retas/vector_texture.hpp"
#include "retas/archive.hpp"
#include <cmath>
namespace retas::vector_archive_detail {
// Validate framing/dense extents without decompressing or allocating tiles.
inline std::uint64_t texture_image_storage(std::span<const std::uint8_t> bytes){
    Reader r(bytes);auto outer=r.section(),header=outer.section();const auto version=header.u16();
    const auto width=header.u32(),height=header.u32(),depth=header.u32();const auto dpi=header.f64();const auto count=header.u32();const auto order=header.u8();
    if((version>>8)>1||!width||!height||width>100000||height>100000||std::uint64_t(width)*height>268435456||
       (depth!=1&&depth!=32&&depth!=64)||order!=(depth==1?0:3)||!std::isfinite(dpi)||dpi<=0)
        throw std::invalid_argument("Unsupported embedded vector texture image header");
    const auto columns=(width+255)/256,rows=(height+255)/256;
    if(count>100000||count!=std::uint64_t(columns)*rows)throw std::invalid_argument("Vector texture requires dense image tiles");
    auto extent=outer.section();const auto left=extent.i32(),top=extent.i32(),right=extent.i32(),bottom=extent.i32();const bool compressed=extent.u8()!=0;
    if(left||top||right<int(width)||bottom<int(height)||right>int(width+255)||bottom>int(height+255)||
       (std::uint64_t(right)+255)/256!=columns||(std::uint64_t(bottom)+255)/256!=rows)
        throw std::invalid_argument("Vector texture requires complete image extent");
    const std::uint64_t tile_size=256ull*256*depth/8;auto blocks=outer.section();
    for(std::uint32_t i=0;i<count;++i){if(blocks.u8()!=1)throw std::invalid_argument("Vector texture cannot sample a missing tile");
        if(blocks.u32()!=tile_size)throw FormatError(0,"Invalid embedded texture tile size");blocks.take(compressed?blocks.u32():std::size_t(tile_size));}
    if(r.remaining())throw std::invalid_argument("Embedded texture image prefix includes trailing bytes");
    return sizeof(VectorTextureMaterial)+std::uint64_t(count)*(tile_size+128);
}
}
