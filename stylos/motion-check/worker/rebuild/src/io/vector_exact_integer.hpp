#pragma once
#include <cstdint>

namespace retas::vector_archive_detail {
// Two's-complement signed 128-bit arithmetic for bounded exact predicates.
// The geometry domain proves all results fit; unsigned limbs avoid signed UB.
struct PortableExactInteger {
    std::uint64_t low{},high{};
    constexpr PortableExactInteger()=default;
    constexpr PortableExactInteger(std::int64_t value):low(std::uint64_t(value)),high(value<0?UINT64_MAX:0){}
    constexpr PortableExactInteger(std::uint64_t low_word,std::uint64_t high_word):low(low_word),high(high_word){}
    constexpr bool operator==(const PortableExactInteger&) const=default;
    constexpr bool operator<(const PortableExactInteger& other) const{
        const auto negative=high>>63,other_negative=other.high>>63;
        if(negative!=other_negative)return negative!=0;
        return high!=other.high?high<other.high:low<other.low;
    }
    constexpr bool operator>(const PortableExactInteger& other) const{return other<*this;}
    constexpr bool operator<=(const PortableExactInteger& other) const{return !(other<*this);}
    constexpr bool operator>=(const PortableExactInteger& other) const{return !(*this<other);}
    constexpr explicit operator bool() const{return low||high;}
    constexpr PortableExactInteger operator-() const{return {0-low,~high+(low==0)};}
    constexpr PortableExactInteger operator+(const PortableExactInteger& other) const{
        const auto sum=low+other.low;return {sum,high+other.high+(sum<low)};
    }
    constexpr PortableExactInteger operator-(const PortableExactInteger& other) const{return *this+-other;}
    constexpr PortableExactInteger& operator+=(const PortableExactInteger& other){return *this=*this+other;}
    static constexpr PortableExactInteger product(std::int64_t a,std::int64_t b){
        const auto x=a<0?0-std::uint64_t(a):std::uint64_t(a),y=b<0?0-std::uint64_t(b):std::uint64_t(b);
        constexpr auto mask=std::uint64_t{0xffffffff};
        const auto x0=x&mask,x1=x>>32,y0=y&mask,y1=y>>32;
        const auto first=x0*y0,second=x1*y0+(first>>32);
        const auto third=(second&mask)+x0*y1;
        const PortableExactInteger result{(third<<32)+(first&mask),x1*y1+(second>>32)+(third>>32)};
        return (a<0)!=(b<0)?-result:result;
    }
};
#ifdef _MSC_VER
using ExactInteger=PortableExactInteger;
constexpr ExactInteger exact_product(std::int64_t a,std::int64_t b){return ExactInteger::product(a,b);}
#else
using ExactInteger=__int128;
constexpr ExactInteger exact_product(std::int64_t a,std::int64_t b){return ExactInteger(a)*b;}
#endif
}
