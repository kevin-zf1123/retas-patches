#pragma once
#include "retas/raster.hpp"
#include "retas/numeric.hpp"
#include <bit>
#include <cmath>

namespace retas::selection_color_detail {
inline bool outside_space(Pixel p,int depth){
    if(!p.a)return false;
    using numeric_detail::binary64;
    const auto factor=depth==32?std::bit_cast<float>(std::uint32_t{0x3b808081}):std::bit_cast<float>(std::uint32_t{0x37800080});
    const auto normalize=[&](unsigned value){return double(float(binary64(double(value)*double(factor))));};
    const double r=normalize(depth==32?p.r>>8:p.r),g=normalize(depth==32?p.g>>8:p.g),b=normalize(depth==32?p.b>>8:p.b);
    const auto multiply=[&](double a,float coefficient){return binary64(a*double(coefficient));};
    const auto y=binary64(binary64(multiply(r,.299f)+multiply(g,.587f))+multiply(b,.114f));
    const auto i=binary64(binary64(multiply(r,.596f)-multiply(g,.275f))-multiply(b,.321f));
    const auto q=binary64(binary64(multiply(r,.212f)-multiply(g,.523f))+multiply(b,.311f));
    const auto chroma=binary64(std::sqrt(binary64(binary64(i*i)+binary64(q*q))));
    return binary64(y+chroma)>1.05||binary64(y-chroma)<-.2;
}
inline Pixel unpack(std::uint64_t c){return {std::uint16_t(c),std::uint16_t(c>>16),std::uint16_t(c>>32),std::uint16_t(c>>48)};}
inline std::uint64_t packed(Pixel p){return std::uint64_t(p.r)|(std::uint64_t(p.g)<<16)|(std::uint64_t(p.b)<<32)|(std::uint64_t(p.a)<<48);}
}
