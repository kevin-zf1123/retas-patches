#pragma once
#include "vector_stored_line.hpp"
#include "vector_byte_sink.hpp"
#include "retas/numeric.hpp"
#include "adjust_render.hpp"
#include <algorithm>
#include <cmath>

namespace retas::vector_render_detail {
enum class LineDocumentOutput {StandardImage,PlainPreview,QualityPreview,StandardImageWhite};
namespace output_terminal {
inline unsigned density(int opacity,int target_depth){return unsigned(*legacy_rt_mul_div(target_depth==32?255:65535,opacity,100));}
// Native1/8 sources are coverage masks. These leaves ignore the root copy
// flag and child mode; only the binary full-coverage/zero-alpha case copies.
inline Pixel mask_behind_white(Pixel destination,unsigned sample,int source_depth,unsigned density,std::uint64_t color){
    const auto coverage=source_depth==1?(sample?density:0u):sample*(density+1)>>8;
    if(source_depth==1&&coverage==255&&!destination.a)
        return Pixel::rgba8(unsigned(color>>8)&255,unsigned(color>>24)&255,unsigned(color>>40)&255);
    return byte_sink::behind_white(destination,coverage,color);
}
// Scaled bitmap leaves quantize cell coverage before multiplying density.
// Only8x full coverage/full density copies dirty zero-alpha material RGB.
inline Pixel scaled_mask_behind_white(Pixel destination,unsigned count,int scale,unsigned density,std::uint64_t color){
    const auto coverage=(255*count/unsigned(scale*scale))*(density+1)>>8;
    if(scale==8&&count==64&&density==255&&!destination.a)
        return Pixel::rgba8(unsigned(color>>8)&255,unsigned(color>>24)&255,unsigned(color>>40)&255);
    return byte_sink::behind_white(destination,coverage,color);
}
// 6505C0 receives constant black/full-alpha material. Partial coverage stays
// numeric BYTE-sized in WORD lanes. Full coverage preserves signed32 product
// wrap before division, followed by the native final WORD narrowing.
inline Pixel scaled_bitmap_word_behind_white(Pixel d,unsigned count){
    if(!count||d.a==65535)return d;
    const auto remaining=65535u-d.a;
    if(count==64){
        if(!d.a)return {0,0,0,65535};
        const int weight=remaining<=32768?int(remaining):int(remaining)-65537;
        const auto channel=[&](unsigned value){return std::uint16_t(std::max(0,int(value)-weight));};
        return {channel(d.r),channel(d.g),channel(d.b),65535};
    }
    const auto alpha=255*count/64,weight=remaining*alpha/65535;
    const auto channel=[&](unsigned value){return std::uint16_t(std::max(0,int(value)-int(weight)));};
    return {channel(d.r),channel(d.g),channel(d.b),std::uint16_t(65535-std::uint64_t(remaining)*(65535-alpha)/65535)};
}
// Native typed transfer preserves source alpha until its density multiply.
// Normal32->64 expands alpha by256 at100%; WhiteKey uses257 instead.
// No-LUT WhiteKey tests the transferred RGB, including64->32 near-white.
inline Pixel raster_source(Pixel source,int source_depth,int target_depth,unsigned density,bool white_key=false,const adjust_render_detail::Tables* tables=nullptr,bool copy=false){
    const bool original_white=source.r==65535&&source.g==65535&&source.b==65535;
    source=adjust_render_detail::general_rgb(source,tables);
    Pixel result=source;
    if(target_depth==32){
        const auto alpha=source_depth==32?std::uint64_t(source.a/257)*(density+1)>>8:std::uint64_t(source.a)*(257*density+1)>>24;
        result=source_depth==32?Pixel::rgba8(source.r/257,source.g/257,source.b/257,unsigned(alpha)):Pixel::rgba8(source.r>>8,source.g>>8,source.b>>8,unsigned(alpha));
    }else result.a=std::uint16_t(source_depth==32&&!white_key?std::uint64_t(source.a/257)*(density+1)>>8:std::uint64_t(source.a)*(density+1)>>16);
    if(white_key&&(tables?(!copy&&original_white):(result.r==65535&&result.g==65535&&result.b==65535)))result.a=0;
    return result;
}
inline Pixel behind_white(Pixel d,Pixel s,int depth,bool copy=false){
    if(copy)return s;
    const unsigned maximum=depth==32?255:65535,expand=depth==32?257:1;
    const auto da=unsigned(d.a/expand),sa=unsigned(s.a/expand),remaining=maximum-da;
    if(!sa||!remaining)return d;
    if(!da&&sa==maximum)return s;
    const auto weight=std::uint64_t(remaining)*sa/maximum;
    const auto channel=[&](unsigned dc,unsigned sc){return std::uint16_t(std::max<std::int64_t>(0,std::int64_t(dc/expand)-std::int64_t(std::uint64_t(maximum-sc/expand)*weight/maximum))*expand);};
    return {channel(d.r,s.r),channel(d.g,s.g),channel(d.b,s.b),
            std::uint16_t((maximum-std::uint64_t(maximum-sa)*remaining/maximum)*expand)};
}
// 64C310 uses the current packed BGR and the next physical byte as coverage.
// Empty/full destination alpha and zero density have invariant pixel output.
inline Pixel rgb24_behind_white(Pixel destination,Pixel source,unsigned next_byte,unsigned density){
    if(!destination.a||destination.a==65535||!density)return destination;
    source.a=std::uint16_t((next_byte*(density+1)>>8)*257);
    return behind_white(destination,source,32);
}
inline Pixel opaque_present(Pixel d,Pixel source,int source_depth,int target_depth,const adjust_render_detail::Tables* tables=nullptr){
    const unsigned maximum=target_depth==32?255:65535,expand=target_depth==32?257:1;
    const auto alpha=unsigned(d.a/expand);if(alpha==maximum)return d;
    source=adjust_render_detail::general_rgb(source,tables);
    if(target_depth==32&&source_depth==64)source=Pixel::rgba8(source.r>>8,source.g>>8,source.b>>8,0);
    const auto channel=[&](unsigned dc,unsigned sc){return std::uint16_t(std::max<std::int64_t>(0,std::int64_t(dc/expand)-std::int64_t(std::uint64_t(maximum-sc/expand)*(maximum-alpha)/maximum))*expand);};
    // Both native WORD selector1 leaves overwrite alpha with numeric255.
    return {channel(d.r,source.r),channel(d.g,source.g),channel(d.b,source.b),std::uint16_t(target_depth==32?65535:255)};
}
inline Pixel opaque_missing(Pixel d,const adjust_render_detail::Tables* tables=nullptr,int depth=32){
    if(!tables){if(!d.a)return Pixel::rgba8(255,255,255);d.a=65535;return d;}
    const unsigned maximum=depth==32?255:65535,expand=depth==32?257:1,bits=depth==32?8:16;
    const auto alpha=unsigned(d.a/expand);const auto white=adjust_render_detail::general_rgb(Pixel::rgba8(255,255,255),tables);
    const auto channel=[&](unsigned dc,unsigned sc){const auto base=alpha?dc/expand:maximum;return std::uint16_t(std::max<std::int64_t>(0,std::int64_t(base)-std::int64_t(std::uint64_t(maximum-sc/expand)*(maximum-alpha)>>bits))*expand);};
    return {channel(d.r,white.r),channel(d.g,white.g),channel(d.b,white.b),65535};
}
inline Pixel finish(Pixel cache,int depth,std::optional<Pixel> background,bool copy){
    const unsigned maximum=depth==32?255:65535,expand=depth==32?257:1;
    const auto alpha=unsigned(cache.a/expand);
    if(background){
        if(!alpha){auto result=*background;result.a=65535;return result;}
        const auto bits=depth==32?8:16;
        const auto channel=[&](unsigned value,unsigned bg){const auto amount=std::uint64_t(maximum+1-alpha)*(maximum-bg/expand)>>bits;
            return std::uint16_t(std::max<std::int64_t>(0,std::int64_t(value/expand)-std::int64_t(amount))*expand);};
        return {channel(cache.r,background->r),channel(cache.g,background->g),channel(cache.b,background->b),65535};
    }
    if(copy||!alpha||alpha==maximum)return cache;
    const auto inverse=numeric_detail::binary64(1.0/alpha);
    const auto channel=[&](unsigned value){const auto product=std::uint32_t(std::uint64_t(maximum-value/expand)*maximum);
        const auto result=numeric_detail::binary64(double(maximum)-numeric_detail::binary64(double(product)*inverse));
        return std::uint16_t(unsigned(std::max(0.0,result))*expand);};
    return {channel(cache.r),channel(cache.g),channel(cache.b),cache.a};
}
}
// Ordinary Line, Paint/Select, General raster and prepared Text image/thumbnail path. Nullopt means a
// different family/metadata/placement domain; limits and malformed graphs
// throw before publication. Explicit64 is a target-format kernel extension,
// not a claim that native61/361/322 choose64 automatically.
std::optional<Raster> render_native_line_document(const DocumentData&,int,LineDocumentOutput,std::stop_token={},StoredLineCacheCosts* ledger=nullptr);
// Paint owner density/modes outside the legacy normal2 composite require
// the whole-document native terminal. Other producers retain their own gates.
bool requires_native_paint_image(const DocumentData&);
// Domain check only; admitted metadata/storage is validated during rendering.
bool supports_native_line_document(const DocumentData&,LineDocumentOutput);
}
