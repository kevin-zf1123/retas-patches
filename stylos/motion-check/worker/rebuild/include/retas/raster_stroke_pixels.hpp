#pragma once
#include "raster.hpp"
#include "raster_stroke.hpp"
#include "selection.hpp"
#include <optional>
#include <stop_token>
#include <string>

namespace retas {
// Native offscreen pixel adapters; unlike Raster::pixel, packed depths expose
// their color/coverage meaning (not a normalized integer storage word).
bool raster_stroke_surface_supported(const Raster&) noexcept;
Pixel raster_stroke_surface_pixel(const Raster&,int x,int y);
struct RasterStrokeReferenceMask {
    const Raster& surface;
    Pixel color;
    bool alpha_only{},white_key{};
    double scale{1};
    bool allows(int x,int y) const;
};
struct RasterStrokePixelResult {
    bool handled{true},changed{},cancelled{};
    Rect dirty{};
    std::size_t changed_pixels{};
    std::string error;
};
// Terminal-only arithmetic. Source is already normalized; 32-bit channels use
// their high byte. Independent weight is in destination units (255 or 65535).
// Invalid depth, terminal or weight throws before any state can be changed.
Pixel raster_stroke_mix_pixel(Pixel destination,Pixel normalized_source,int depth,
    RasterStrokeTerminal terminal,std::uint16_t weight);
// radius_pixels has ALREADY received pressure and hardness expansion. Strength
// scales source alpha in packed32 before normalization, even on a 64-bit target.
// Selection gates final writes only. A dab commits atomically using raster COW;
// cancellation/failure leaves target unchanged. Canonical 1/2/8/16/24/32/64
// offscreens use their native depth-specific terminal/setter.
RasterStrokePixelResult raster_stroke_pixels(Raster& target,Point center,
    const RasterStrokeDab& dab,Pixel color,
    const std::optional<RasterSelection>& selection={},std::stop_token stop={},const RasterStrokeReferenceMask* reference=nullptr);
}
