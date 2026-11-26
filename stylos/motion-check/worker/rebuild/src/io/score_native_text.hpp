#pragma once
#include "retas/archive.hpp"

namespace retas::score_native_detail {
// Project stored RCString bytes to the pinned Chinese runtime's CP936 string.
// Preserve valid lead/trail pairs; expand archive CR before C-string consumers.
inline Bytes runtime_string(std::span<const std::uint8_t> raw){
    Bytes text;
    for(std::size_t i=0;i<raw.size()&&raw[i];++i){
        const auto c=raw[i];text.push_back(c);
        if(c>=0x81&&c<=0xfe&&i+1<raw.size()&&raw[i+1]>=0x40&&raw[i+1]<=0xfe&&raw[i+1]!=0x7f)text.push_back(raw[++i]);
        else if(c==13)text.push_back(10);
    }
    return text;
}
}
