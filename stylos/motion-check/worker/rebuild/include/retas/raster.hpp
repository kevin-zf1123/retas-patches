#pragma once
#include "archive.hpp"
#include <compare>
#include <map>
#include <memory>

namespace retas {
struct Pixel {
    std::uint16_t r{65535}, g{65535}, b{65535}, a{};
    auto operator<=>(const Pixel&) const = default;
    static Pixel rgba8(unsigned r, unsigned g, unsigned b, unsigned a = 255) {
        return {std::uint16_t(r * 257), std::uint16_t(g * 257), std::uint16_t(b * 257), std::uint16_t(a * 257)};
    }
};
struct TileCoord {
    int x{}, y{};
    auto operator<=>(const TileCoord&) const = default;
};
class Raster {
public:
    static constexpr int tile_side = 256;
    int width{}, height{}, depth{32};
    double resolution{144.0};
    std::uint8_t color_order{3};
    std::map<TileCoord, std::shared_ptr<Bytes>> tiles;
    Raster() = default;
    Raster(int width, int height, int depth = 32, double dpi = 144.0);
    std::size_t tile_bytes() const;
    bool can_edit() const { return (depth == 32 || depth == 64) && color_order == 3; }
    bool contains(int x, int y) const { return x >= 0 && y >= 0 && x < width && y < height; }
    Pixel pixel(int x, int y) const;
    void set_pixel(int x, int y, Pixel color);
    void blend_pixel(int x, int y, Pixel color, std::uint16_t coverage = 65535);
    void erase_pixel(int x, int y, std::uint16_t coverage = 65535);
    bool same_pixels(const Raster& other) const;
    void prune_empty();
    // Native canonical-save Optimize. Tests logical samples only, preserves
    // survivor padding, and deletes whole tiles without rewriting backing.
    // High-depth unmodeled color orders are preserved without reinterpretation.
    // Returns whether tile topology changed. Callers serialize backing mutation.
    bool optimize_storage();
};
Pixel over(Pixel destination, Pixel source, std::uint16_t opacity = 65535);
Raster decode_offscreen(Reader& parent);
// Pure canonical host-save encoding, including native logical Optimize.
// This is not the DLL's raw Write-without-Optimize export.
Bytes encode_offscreen(const Raster& image,int compression_level=6);
}
