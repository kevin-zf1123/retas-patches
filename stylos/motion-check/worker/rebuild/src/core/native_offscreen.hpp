#pragma once
#include "selection_raster.hpp"
#include <algorithm>

namespace retas::offscreen_detail {
// Native virtual Set64BitColor (+B0). Low-depth output encodes coverage or
// gray, rather than the normalized storage word exposed by Raster::pixel.
inline std::uint64_t color_word(Pixel p,int depth){
    const unsigned r=p.r>>8,g=p.g>>8,b=p.b>>8,a=p.a>>8;
    const unsigned gray=(2*r+4*g+b)/7,coverage=((255-gray)*(a+1))>>8;
    switch(depth){
    case 1:return coverage>127;
    case 2:return a<128?0:gray>126?2:3;
    case 8:return coverage;
    case 16:return a|(gray<<8);
    case 24:return b|(g<<8)|(r<<16);
    case 32:return b|(g<<8)|(r<<16)|(std::uint64_t(a)<<24);
    case 64:return std::uint64_t(p.b)|(std::uint64_t(p.g)<<16)|(std::uint64_t(p.r)<<32)|(std::uint64_t(p.a)<<48);
    default:throw std::invalid_argument("Unsupported native offscreen depth");
    }
}
inline bool store_color(Raster& raster,int x,int y,Pixel p){
    const auto value=color_word(p,raster.depth);
    const bool changed=selection_detail::raw_pixel(raster,x,y)!=value;
    selection_detail::store_raw_pixel(raster,x,y,value);return changed;
}
inline bool materialize(Raster& raster,Rect area,std::stop_token stop={}){
    if(area.width<=0||area.height<=0)return true;
    const auto left=std::max(0,area.x),top=std::max(0,area.y);
    const auto right=std::min<std::int64_t>(raster.width,std::int64_t(area.x)+area.width);
    const auto bottom=std::min<std::int64_t>(raster.height,std::int64_t(area.y)+area.height);
    if(left>=right||top>=bottom)return true;
    for(int ty=top/256;ty<=(bottom-1)/256;++ty)for(int tx=left/256;tx<=(right-1)/256;++tx){
        if(stop.stop_requested())return false;
        if(!raster.tiles.contains({tx,ty}))selection_detail::store_raw_pixel(raster,tx*256,ty*256,selection_detail::clear_word(raster.depth));
    }
    return true;
}
}
