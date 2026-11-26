#pragma once
#include "retas/raster.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <cmath>

namespace retas::vector_render_detail::byte_sink {
inline std::uint8_t full(unsigned destination,unsigned source,bool compose){
    return std::uint8_t(compose?destination+(255-destination)*source/255:source);
}
inline std::uint8_t partial(unsigned destination,unsigned source,double coverage,bool compose){
    const auto k=std::min(256u,unsigned(std::trunc(coverage*256+1.0)));
    const auto s=source*k>>8;
    if(!s)return std::uint8_t(destination);
    return std::uint8_t(std::min(255u,s+(compose?destination*(255-s)>>8:destination)));
}
template<class Write> void fractional_span(double left,double right,Write write){
    if(left>=right)return;
    const auto first=int(std::floor(left)),last=int(std::floor(right));
    write(first,true,first+1.0-left);
    int x=first+1;for(;x<last;++x)write(x,false,1.0);
    // 649BD0 advances its physical pointer after the first partial even
    // when both endpoints floor to the same coordinate.
    write(x,true,right-last);
}
// 64C020 consumes a real Raster8 byte and an opaque Common.color material.
// Its destination is the complement-on-white cache, not straight RGBA.
inline Pixel behind_white(Pixel destination,unsigned coverage,std::uint64_t color){
    const auto alpha=unsigned(destination.a/257),remaining=255-alpha;
    if(!coverage||!remaining)return destination;
    const auto weight=remaining*coverage/255;
    const auto channel=[&](unsigned d,unsigned c){return unsigned(std::max(0,int(d)-int(weight*(255-c)/255)));};
    return Pixel::rgba8(channel(destination.r/257,unsigned(color>>8)&255),
                       channel(destination.g/257,unsigned(color>>24)&255),
                       channel(destination.b/257,unsigned(color>>40)&255),
                       255-remaining*(255-coverage)/255);
}
// 650B10 uses PC53 reciprocal/multiply/subtract, then __ftol2. Exact
// rational division differs at integral boundaries (alpha75/raw185 ->16).
inline Pixel straight(Pixel cache){
    const auto alpha=unsigned(cache.a/257);if(!alpha||alpha==255)return cache;
    const auto inverse=numeric_detail::binary64(1.0/alpha);
    const auto channel=[&](unsigned value){const auto product=255*(255-value);
        const auto result=numeric_detail::binary64(255.0-numeric_detail::binary64(double(product)*inverse));
        return unsigned(std::max(0.0,result));};
    return Pixel::rgba8(channel(cache.r/257),channel(cache.g/257),channel(cache.b/257),alpha);
}
}
