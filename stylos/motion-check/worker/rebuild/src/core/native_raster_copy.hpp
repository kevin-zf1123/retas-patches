#pragma once
#include "retas/raster.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>
namespace retas::raster_copy_detail {
inline std::uint64_t low_copy_pixel(std::uint64_t payload,std::uint64_t old,int depth){
    if(depth==1)return payload|old;
    if(depth==2)return old==3?(payload|3):old==2?2:old==1?0:payload;
    if(depth==8)return old?255-((255-payload)*(255-old)>>8):payload;
    if(depth==24)return payload; // backed temporary24 samples have alpha255.
    if(depth==16){
        const unsigned a=payload&255,c=(payload>>8)&255,oa=old&255,oc=(old>>8)&255;
        if(a==255)return payload;
        const unsigned e=oa*(255-a)/255,channel=unsigned(std::max(0,int(c)-int((255-oc)*e/255)));
        return (255-(255-oa)*(255-a)/255)|(std::uint64_t(channel)<<8);
    }
    throw std::invalid_argument("Unsupported low-depth floating copy");
}
inline Pixel writeback_pixel(Pixel destination,Pixel raw,int depth,bool alpha_copy){
    const auto sample=[](std::uint16_t word){return unsigned(word>>8);};
    const unsigned unit=depth==64?257:1,maximum=depth==64?65535:255;
    const std::uint64_t sa=sample(raw.a)*unit;
    const auto unpack=[&](std::uint16_t word){return depth==64?unsigned(word):unsigned(word/257);};
    if(alpha_copy){
        destination.a=std::uint16_t((sample(raw.r)*unit*sa/maximum)*(depth==32?257:1));
        return destination;
    }
    if(!sa)return destination;
    const auto da=unpack(destination.a);
    Pixel result;
    if(!da||sa==maximum){
        return {std::uint16_t(sample(raw.r)*257),std::uint16_t(sample(raw.g)*257),std::uint16_t(sample(raw.b)*257),std::uint16_t(sample(raw.a)*257)};
    }
    if(depth==64){
        const std::uint64_t inverse=maximum-sa,weight=da*inverse/maximum;
        const auto alpha=maximum-(maximum-da)*inverse/maximum;
        const auto channel=[&](std::uint16_t d,std::uint16_t s){
            return std::uint16_t(maximum-((maximum-d)*weight+(maximum-sample(s)*unit)*sa)/alpha);
        };
        result={channel(destination.r,raw.r),channel(destination.g,raw.g),channel(destination.b,raw.b),std::uint16_t(alpha)};
    }else{
        using numeric_detail::binary64;
        if(da==255){
            const auto channel=[&](std::uint16_t d,std::uint16_t s){
                return std::uint16_t((255-((255-unpack(d))*(255-sa)+(255-sample(s))*sa)/255)*257);
            };
            result={channel(destination.r,raw.r),channel(destination.g,raw.g),channel(destination.b,raw.b),65535};
        }else{
            // RCPixelMixerBlend::Do32Bit partial-destination branch: use
            // its stored binary64 reciprocal and inverse-alpha multiply,
            // preserving source operation order at observable rounding ties.
            constexpr double inverse_byte=std::bit_cast<double>(std::uint64_t{0x3f70101010101010});
            const double weight=binary64(double(da*(255-sa))*inverse_byte),alpha=binary64(sa+weight);
            const double inverse_alpha=binary64(1.0/alpha);
            const auto channel=[&](std::uint16_t d,std::uint16_t s){
                const double numerator=binary64(binary64((255-unpack(d))*weight)+double((255-sample(s))*sa));
                return std::uint16_t((255-unsigned(binary64(binary64(numerator*inverse_alpha)+.5)))*257);
            };
            result={channel(destination.r,raw.r),channel(destination.g,raw.g),channel(destination.b,raw.b),std::uint16_t(unsigned(binary64(alpha+.5))*257)};
        }
    }
    return result;
}
}
