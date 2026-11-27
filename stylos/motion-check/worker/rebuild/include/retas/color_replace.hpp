#pragma once
#include "document.hpp"
#include <stop_token>

namespace retas {
struct ColorReplaceRule {
    Pixel source{65535,65535,65535,65535};
    Pixel destination{65535,65535,65535,65535};
    std::int32_t done{};
    auto operator<=>(const ColorReplaceRule&) const = default;
};
using ColorReplaceRules = std::vector<ColorReplaceRule>;
ColorReplaceRules default_color_replace_rules();
// Auto-detects the sectioned PaintMan .src family and the compact Ver.5 .crf
// family. Rebuild-era .ColorReplace files use the compact .crf bytes.
ColorReplaceRules decode_color_replace(std::span<const std::uint8_t> bytes);
ColorReplaceRules decode_color_replace_src(std::span<const std::uint8_t> bytes);
ColorReplaceRules decode_color_replace_crf(std::span<const std::uint8_t> bytes);

// The native .src format retains 16-bit RGBA and the full Done word.
Bytes encode_color_replace_src(std::span<const ColorReplaceRule> rules);
// Ver.5 .crf intentionally stores only high-byte RGB and low-word Done.
Bytes encode_color_replace_crf(std::span<const ColorReplaceRule> rules);
// Kept as the compact encoder for source compatibility with earlier rebuilds.
Bytes encode_color_replace(std::span<const ColorReplaceRule> rules);

inline constexpr std::size_t max_generated_color_replace_rules = 255;
// PaintMan compares flattened byte-color images by coordinate. The first
// destination observed for each differing source color wins; emitted rules
// are ordered by the packed source color and enabled.
ColorReplaceRules generate_color_replace_rules(const Raster& source,
                                                const Raster& destination);

enum class ColorReplaceMode { Normal, WhiteIsEmpty, Nonwhite, RgbKeepAlpha };
struct ColorReplaceResult {
    std::uint64_t changed_pixels{};
    bool cancelled{};
    std::uint64_t materialized_tiles{};
    std::uint64_t metadata_records_changed{};
    bool modified() const {
        return changed_pixels || materialized_tiles || metadata_records_changed;
    }
};
// These workers leave the input unchanged on cancellation or exception.
ColorReplaceResult replace_colors(Raster& image, std::span<const ColorReplaceRule> rules,
                                 ColorReplaceMode mode=ColorReplaceMode::Normal,
                                 std::stop_token stop={},std::array<std::uint64_t,2>* cumulative=nullptr);
ColorReplaceResult replace_colors(DocumentData& document, std::span<const ColorReplaceRule> rules,
                                 std::stop_token stop={},std::optional<std::size_t> owner={},std::array<std::uint64_t,2>* cumulative=nullptr);
}
