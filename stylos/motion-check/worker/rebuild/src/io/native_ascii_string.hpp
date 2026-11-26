#pragma once
#include "retas/archive.hpp"
#include <algorithm>
#include <cstring>

namespace retas::native_ascii_detail {
inline std::span<const std::uint8_t> c_prefix(std::span<const std::uint8_t> text){return text.first(std::find(text.begin(),text.end(),0)-text.begin());}
inline void ascii_only(std::span<const std::uint8_t> text){if(std::ranges::any_of(text,[](auto c){return c>=128;}))throw std::invalid_argument("Native archive ASCII profile requires ASCII strings");}
template<class Ledger>Bytes decode_string(std::span<const std::uint8_t> wire,Ledger& ledger,bool ascii_profile=true){
    ledger.visit(wire.size()*2);const auto cr=std::ranges::count(wire,'\r');
    Bytes result(wire.begin(),wire.end());result.resize(wire.size()+std::size_t(cr)+1,0);std::size_t i{};
    for(;result[i];++i){ledger.visit(1);if(ascii_profile&&result[i]>=128)throw std::invalid_argument("Native archive ASCII profile requires ASCII strings");
        if(result[i]!='\r')continue;
        // ReadString tests i+2 and moves strlen(i+1) bytes, excluding NUL.
        // This move and the LF store can expose an original NUL suffix.
        if(result[i+2]){const auto end=std::find(result.begin()+i+1,result.end(),0);const auto n=std::size_t(end-(result.begin()+i+1));ledger.visit(n*2);std::memmove(result.data()+i+2,result.data()+i+1,n);}
        result[i+1]='\n';
    }
    result.resize(i);return result;
}
inline Bytes encode_string(std::span<const std::uint8_t> input,bool ascii_profile=true){
    const auto text=c_prefix(input);if(ascii_profile)ascii_only(text);Bytes result;result.reserve(text.size());
    for(std::size_t i=0;i<text.size();++i){result.push_back(text[i]);if(text[i]=='\r'){
        if(i+1==text.size())throw std::invalid_argument("Native archive string has unsafe trailing CR");++i;}}
    return result;
}
}
