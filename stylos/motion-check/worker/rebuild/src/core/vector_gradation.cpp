#include "retas/vector_gradation.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>

namespace retas {
namespace {
std::int32_t difference(std::int32_t a,std::int32_t b){
    const auto d=std::int64_t(a)-b;
    if(d<=std::numeric_limits<std::int32_t>::min()||d>std::numeric_limits<std::int32_t>::max())
        throw std::invalid_argument("Vector gradation DWORD difference overflow");
    return std::int32_t(d);
}
Pixel packed(std::uint64_t c){return {std::uint16_t(c),std::uint16_t(c>>16),std::uint16_t(c>>32),std::uint16_t(c>>48)};}
// Runtime rcwCmn cbDitherTable256 literal, ASM263698..263714, in byte order.
constexpr std::array<std::uint8_t,256> dither{{
0x00,0x80,0x20,0xa0,0x08,0x88,0x28,0xa8,0x02,0x82,0x22,0xa2,0x0a,0x8a,0x2a,0xaa,
0xc0,0x40,0xe0,0x60,0xc8,0x48,0xe8,0x68,0xc2,0x42,0xe2,0x62,0xca,0x4a,0xea,0x6a,
0x30,0xb0,0x10,0x90,0x38,0xb8,0x18,0x98,0x32,0xb2,0x12,0x92,0x3a,0xba,0x1a,0x9a,
0xf0,0x70,0xd0,0x50,0xf8,0x78,0xd8,0x58,0xf2,0x72,0xd2,0x52,0xfa,0x7a,0xda,0x5a,
0x0c,0x8c,0x2c,0xac,0x04,0x84,0x24,0xa4,0x0e,0x8e,0x2e,0xae,0x06,0x86,0x26,0xa6,
0xcc,0x4c,0xec,0x6c,0xc4,0x44,0xe4,0x64,0xce,0x4e,0xee,0x6e,0xc6,0x46,0xe6,0x66,
0x3c,0xbc,0x1c,0x9c,0x34,0xb4,0x14,0x94,0x3e,0xbe,0x1e,0x9e,0x36,0xb6,0x16,0x96,
0xfc,0x7c,0xdc,0x5c,0xf4,0x74,0xd4,0x54,0xfe,0x7e,0xde,0x5e,0xf6,0x76,0xd6,0x56,
0x03,0x83,0x23,0xa3,0x0b,0x8b,0x2b,0xab,0x01,0x81,0x21,0xa1,0x09,0x89,0x29,0xa9,
0xc3,0x43,0xe3,0x63,0xcb,0x4b,0xeb,0x6b,0xc1,0x41,0xe1,0x61,0xc9,0x49,0xe9,0x69,
0x33,0xb3,0x13,0x93,0x3b,0xbb,0x1b,0x9b,0x31,0xb1,0x11,0x91,0x39,0xb9,0x19,0x99,
0xf3,0x73,0xd3,0x53,0xfb,0x7b,0xdb,0x5b,0xf1,0x71,0xd1,0x51,0xf9,0x79,0xd9,0x59,
0x0f,0x8f,0x2f,0xaf,0x07,0x87,0x27,0xa7,0x0d,0x8d,0x2d,0xad,0x05,0x85,0x25,0xa5,
0xcf,0x4f,0xef,0x6f,0xc7,0x47,0xe7,0x67,0xcd,0x4d,0xed,0x6d,0xc5,0x45,0xe5,0x65,
0x3f,0xbf,0x1f,0x9f,0x37,0xb7,0x17,0x97,0x3d,0xbd,0x1d,0x9d,0x35,0xb5,0x15,0x95,
0xff,0x7f,0xdf,0x5f,0xf7,0x77,0xd7,0x57,0xfd,0x7d,0xdd,0x5d,0xf5,0x75,0xd5,0x55}};
}
VectorGradationSampler::VectorGradationSampler(std::shared_ptr<const VectorGradationMaterial> material):VectorGradationSampler(std::move(material),true){}
VectorGradationSampler::VectorGradationSampler(std::shared_ptr<const VectorGradationMaterial> material,bool freeze):material_(std::move(material)){
    if(!material_)throw std::invalid_argument("Null vector gradation material");
    const auto& m=*material_;const auto type=m.flags&15;
    if(type!=1&&type!=2)throw std::invalid_argument("Unsupported vector gradation type");
    if(m.stops.empty())throw std::invalid_argument("Empty vector gradation stop chain");
    if(m.stops.size()>100000)throw std::length_error("Vector gradation stop limit exceeded");
    std::optional<float> previous;
    for(const auto& s:m.stops){const float p=std::bit_cast<float>(s.position_bits);
        if(!std::isfinite(p)||(previous&&p<*previous))throw std::invalid_argument("Vector gradation stops must be finite and nondecreasing");previous=p;}
    const auto r=numeric_detail::binary64;
    dx_=difference(m.end_x,m.start_x);dy_=difference(m.end_y,m.start_y);
    squared_=std::int64_t(dx_)*dx_+std::int64_t(dy_)*dy_;
    if(type==2){
        const float ratio=std::bit_cast<float>(m.minor_ratio_bits);
        if(!std::isfinite(ratio)||ratio<=0||!squared_)throw std::invalid_argument("Elliptic vector gradation needs positive finite ratio and nonzero axis");
        const double length=r(std::sqrt(r(r(double(dx_)*dx_)+r(double(dy_)*dy_))));
        u_=r(1.0/length);v_=r(1.0/r(double(ratio)*length));
        double angle;constexpr double degrees=double(std::bit_cast<float>(std::uint32_t(0x42652ee0)));
        if(dx_>0){angle=r(std::atan2(r(double(dy_)/dx_),1.0)*degrees);if(angle<0)angle=r(angle+360.0);}
        else if(dx_<0)angle=r(r(std::atan2(r(double(dy_)/dx_),1.0)*degrees)+180.0);
        else angle=dy_<0?270.0:90.0;
        const double theta=r(double(float(angle))*std::bit_cast<double>(std::uint64_t(0x3f91df46a2529d39)));
        cosine_=r(std::cos(theta));negative_sine_=r(-std::sin(theta));
        if(!std::isfinite(u_)||!std::isfinite(v_))throw std::invalid_argument("Vector gradation inverse radius exceeds numeric domain");
    }
    // A const shared view can still have mutable aliases. Freeze the admitted
    // fields and stop chain together so cached numeric state and later reads
    // cannot disagree or observe a cleared chain after construction.
    if(freeze)material_=std::make_shared<const VectorGradationMaterial>(*material_);
}
float VectorGradationSampler::parameter(std::int32_t x,std::int32_t y) const {
    const auto& m=*material_;const auto r=numeric_detail::binary64;
    const auto qx=difference(x,m.start_x),qy=difference(y,m.start_y);float t=0;
    if((m.flags&15)==1){
        if(squared_>=std::numeric_limits<std::int32_t>::max()){
            const double numerator=r(r(double(qy)*dy_)+r(double(qx)*dx_));
            const double denominator=r(r(double(dy_)*dy_)+r(double(dx_)*dx_));t=float(r(numerator/denominator));
        }else if(squared_){
            const auto dot=std::int64_t(dx_)*qx+std::int64_t(dy_)*qy;
            const auto numerator=std::clamp<std::int64_t>(dot,-2147483647,2147483647);t=float(r(double(numerator)/double(squared_)));
        }
    }else{
        const float xx=float(r(r(r(cosine_*qx)-r(negative_sine_*qy))*u_));
        const double yy=r(r(r(cosine_*qy)+r(negative_sine_*qx))*v_);
        t=float(r(std::sqrt(r(r(double(xx)*xx)+r(yy*yy)))));
    }
    const auto choice=m.flags&0xf0;
    const std::uint32_t coefficient=choice==0x10?0x37a7c5ac:choice==0x20?0x38a7c5ac:choice==0x40?0x399d4952:0;
    if(coefficient){const auto px=std::uint32_t(x>>4)&15u,py=std::uint32_t(y>>4)&15u;
        t=float(r(r(double(int(dither[py*16+px])-127)*std::bit_cast<float>(coefficient))+double(t)));}
    if(!std::isfinite(t))throw std::invalid_argument("Vector gradation sample exceeds finite F32 domain");return t;
}
void VectorGradationSampler::admit_rectangle(std::int32_t left,std::int32_t top,std::int32_t right,std::int32_t bottom) const {
    if(right<left||bottom<top)throw std::invalid_argument("Invalid vector gradation query rectangle");
    for(auto x:{left,right})for(auto y:{top,bottom})(void)parameter(x,y);
}
Pixel VectorGradationSampler::sample(std::int32_t x,std::int32_t y) const {
    const float t=parameter(x,y);const auto& stops=material_->stops;
    if(t<=std::bit_cast<float>(stops.front().position_bits))return packed(stops.front().color);
    if(t>=std::bit_cast<float>(stops.back().position_bits))return packed(stops.back().color);
    const auto upper=std::lower_bound(stops.begin()+1,stops.end(),t,[](const VectorGradationStop& s,float p){return std::bit_cast<float>(s.position_bits)<p;});
    const auto& lower=*(upper-1);const auto r=numeric_detail::binary64;
    const auto weight=std::uint64_t(std::trunc(r(r(r(double(std::bit_cast<float>(upper->position_bits))-t)/
        r(double(std::bit_cast<float>(upper->position_bits))-std::bit_cast<float>(lower.position_bits)))*65536.0)));
    const auto a=packed(lower.color),b=packed(upper->color);
    const auto lane=[&](unsigned lo,unsigned hi){return std::uint16_t((std::uint64_t(lo)*weight+std::uint64_t(hi)*(65536-weight))>>16);};
    return {lane(a.r,b.r),lane(a.g,b.g),lane(a.b,b.b),lane(a.a,b.a)};
}
}
