#pragma once
#include "retas/raster.hpp"
#include "resolution_raster_internal.hpp"
#include <array>
#include <optional>

namespace retas {
class TextFontBackend;
struct Layer;
struct Plane;
struct DocumentData;
namespace text_font_detail {
// Binary coverage uses canonical Raster depth1: set bit means ink, opposite
// the original white-background GDI bitmap bit. Export/image origins are
// document pixels; ordinary display origins are plane-local.
struct Mask {Raster raster;std::array<int,2> origin;};
struct ExportMasks {
    Mask glyph;
    std::optional<Mask> leader;
    std::uint32_t text_color{},line_color{};
};
ExportMasks export_masks(const TextFontBackend&,const Layer&,const Plane&,
    double dpi,resolution_detail::Budget&,std::uint64_t& padded_pixels);
// Native642010's mode1/32 terminal. Preserves allocated white blocks and
// visits glyph then leader on the same complement-on-white destination.
void paint_export_masks(Raster&,const ExportMasks&,int density,resolution_detail::Budget&);
// Native63C210 ->642010(destination,100,0): opaque replacement at mask ink,
// directly on canonical32/64 backing; leader replaces glyph at intersections.
void paint_conversion_masks(Raster&,const ExportMasks&,resolution_detail::Budget&);
// Ordinary641CE0 profile, independent of the opaque merged raster getter.
// These masks use plane-local origins; owner translation is applied by consumers.
const ExportMasks* display_masks(const Layer&,const Plane&,double dpi) noexcept;
const ExportMasks* image_masks(const Layer&,const Plane&,double dpi) noexcept;
// Reposition the target-DPI bitmaps for6415F0(0), without resampling. Source
// pixels clipped by a negative destination keep their original bitmap offset.
ExportMasks ordinary_view_masks(const Layer&,const Plane&,double dpi,resolution_detail::Budget&);
// Same zero-placement blit geometry, for freshly generated positive-DPI masks
// in the independent print intermediate (without a display-cache fingerprint).
ExportMasks zero_placement_masks(const Layer&,const Plane&,const ExportMasks&,resolution_detail::Budget&);
bool requires_native_image(const DocumentData&) noexcept;
void admit_display_masks(const ExportMasks&,resolution_detail::Budget&);
Pixel display_pixel(const ExportMasks&,Pixel,int x,int y,int depth,int density);
// The document root precharges its entire destination backing. This terminal
// charges/validates masks separately, and never applies the root copy flag.
void paint_display_masks(Raster&,const ExportMasks&,int density,std::array<int,2> owner,std::stop_token={});
}
}
