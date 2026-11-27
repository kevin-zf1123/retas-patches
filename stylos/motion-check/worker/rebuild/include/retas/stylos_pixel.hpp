#pragma once
#include <algorithm>
#include <cstdint>

namespace retas::stylos260 {
// Stylos 2.6.0 composes into a complement-on-white cache with 8-bit shifts.
// Both source and destination expose byte channels r/g/b/a; source is straight.
template<class Color> constexpr Color behind_nontransparent(Color d, Color s) {
    if (d.a == 255) return d;
    if (s.a == 255 && !d.a) return s;
    const unsigned remaining = 255u - d.a;
    const unsigned weight = ((s.a + 1u) * remaining) >> 8;
    auto channel = [weight](int old, int ink) {
        return static_cast<std::uint8_t>(std::max(0, old - int(((255u - ink) * weight) >> 8)));
    };
    return {channel(d.r,s.r), channel(d.g,s.g), channel(d.b,s.b),
            static_cast<std::uint8_t>(255u - (((255u - s.a) * remaining) >> 8))};
}
template<class Color> constexpr Color behind(Color d, Color s) {
    return s.a ? behind_nontransparent(d,s) : d;
}
}
