#pragma once
#include "document.hpp"
#include "geometry.hpp"
#include <stop_token>
namespace retas {
// At 1:1 document/raster scale, only the owning layer sets the raster origin.
// Clip before narrowing so off-page selections and stored int32 origins are safe.
Rect raster_local_rect(const Layer& owner,const Raster& raster,Rect document_rect) noexcept;
// Runtime selection adapter for the current same-DPI owner-local edit space.
// Plane common origins remain metadata. Mixed-resolution selection transfer
// needs the native per-tool mapper and is rejected before editing.
enum class RasterSelectionSource { BoundsOrMask,BackingSurface };
std::optional<RasterSelection> raster_selection(const Document& document,const Layer& owner,const Raster& raster,
    RasterSelectionSource source=RasterSelectionSource::BoundsOrMask);
struct PaintOptions {
    double radius{8};
    double hardness{1};
    Pixel color{0,0,0,65535};
    std::uint16_t opacity{65535};
    bool eraser{};
};
bool paint_segment(Raster& image,Point from,Point to,const PaintOptions& options,const std::optional<RasterSelection>& selection={});
void fill_region(Raster& image,int x,int y,Pixel color,int tolerance=0,const std::optional<RasterSelection>& selection={},std::stop_token stop={});
void fill_rectangle(Raster& image,Rect rect,Pixel color,const std::optional<RasterSelection>& selection={});
Raster transform_raster(const Raster& source,int quarter_turns,bool mirror_x,bool mirror_y);
void map_pixels(Raster& image,const std::function<Pixel(Pixel)>& mapper,const std::optional<RasterSelection>& selection={});
}
