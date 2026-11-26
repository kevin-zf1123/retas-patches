#pragma once
#include "retas/score_sound.hpp"

namespace retas::score_sound_detail {
inline std::uint64_t frame_bytes(const ScoreSoundMetadata& m){
    if((m.format!=ScoreSoundFormat::Wave&&m.format!=ScoreSoundFormat::Aiff)||
       (m.channels!=1&&m.channels!=2)||(m.bits_per_sample!=8&&m.bits_per_sample!=16))
        throw std::invalid_argument("Unsupported cached Score sound metadata");
    return std::uint64_t(m.channels)*(m.bits_per_sample/8);
}
inline std::uint64_t byte_offset(std::uint32_t ticks,std::uint32_t rate,std::uint64_t alignment){
    const auto bytes_per_second=std::uint64_t(rate)*alignment/65536;
    auto offset=std::uint64_t(ticks/600)*bytes_per_second+std::uint64_t(ticks%600)*bytes_per_second/600;
    return offset-offset%alignment;
}
}
