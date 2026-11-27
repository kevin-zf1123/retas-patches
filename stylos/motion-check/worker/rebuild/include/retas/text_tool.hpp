#pragma once
#include "archive.hpp"
#include <array>
namespace retas {
// Raw legacy strings and COLORREF words are preserved by the native codec.
struct TextParameters {
    Bytes text,font_name{ascii("MS UI Gothic")};
    std::int32_t font_size{10};
    std::uint32_t text_color{},line_color{};
    std::int32_t line_enabled{};
    std::array<std::int32_t,2> text_anchor{},line_anchor{};
    Bytes suffix;
    bool operator==(const TextParameters&) const=default;
};
class Raster;
// Apply a prepared colored glyph image through canonical offscreen adapters.
void bake_text_raster(Raster& target,const Raster& glyphs,bool alpha_only=false);
TextParameters decode_text_parameters(std::span<const std::uint8_t>);
Bytes encode_text_parameters(const TextParameters&);
}
