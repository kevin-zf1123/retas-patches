#pragma once
#include "retas/vector_render.hpp"
#include <map>

namespace retas {struct VectorFaceGeometry;struct VectorPaintArchive;struct VectorPaintPlaneGeometry;struct VectorShapeTextureRecipe;}

namespace retas::vector_render_detail {
// Owning ordinary Line/Vert-Line cache source, separate from the public
// transient/Fresh geometry API. All source siblings are validated before
// visibility filtering. Stored bounds and descriptors are never rebuilt.
struct StoredLinePlane {
    VectorLineGeometry lines;
    std::map<std::int32_t,VectorBounds> model_bounds;
    std::vector<VectorBounds> line_bounds; // Exact saved strokes order/count.
    std::vector<VectorSpanProfiles> line_profiles; // Empty means all NULL.
    std::uint64_t reserved_storage{};
    std::uint64_t reserved_work{};
    std::uint64_t color{}; // Formal Vert-Line Common.color, independent of LC.
};
std::vector<StoredLinePlane> decode_stored_line_planes(const Layer&);
// Paint callers retain the already decoded complete base graph. Ownership
// validation includes all Line/Vert siblings; Face slots remain empty.
std::vector<StoredLinePlane> project_stored_paint_lines(const Layer&,const VectorLineArchive&);
std::vector<VectorPaintPlaneGeometry> project_stored_paint_faces(VectorPaintArchive&,const Layer&,
    std::map<int,std::shared_ptr<const VectorShapeTextureRecipe>>&,std::uint64_t reserved);
struct StoredPaintPlane {
    StoredLinePlane lines;
    std::vector<const VectorFaceGeometry*> faces;
    std::vector<std::shared_ptr<const VectorShapeTextureRecipe>> texture_recipes;
    bool visible{true};
};
// Paint normal1 (target32) / AA2: reverse planes, reverse LC at1x/forward at2x, forward
// Face occurrences. Normal1 composes directly; AA2 flushes each plane and
// resolves once per owner. All hidden
// siblings participate in admission before publication is filtered.
bool can_render_stored_paint_planes(std::span<const StoredPaintPlane>,const VectorRenderContext&,std::uint64_t previous_work=0);
Raster render_stored_paint_planes(std::span<const StoredPaintPlane>,const VectorRenderContext&,std::stop_token={},std::uint64_t previous_work=0);
Pixel stored_paint_planes_pixel(std::span<const StoredPaintPlane>,const VectorRenderContext&,int,int,std::uint64_t previous_work=0);
// Normal no-minimum-width/style-scaling1x/2x32/64 cache: reverse planes,
// reverse LC at1x, forward LC at2x, one companion flush per plane. Both
// LC and MC use original raw four-comparison gates, even collapsed bounds.
bool can_render_stored_line_planes(std::span<const StoredLinePlane>,const VectorRenderContext&);
Raster render_stored_line_planes(std::span<const StoredLinePlane>,const VectorRenderContext&,std::stop_token={});
Pixel stored_line_planes_pixel(std::span<const StoredLinePlane>,const VectorRenderContext&,int,int,std::stop_token={});
// Formal normal display cache: real per-Vert Raster8 masks, Common.color,
// one complement-white normalization. Context target must be32; output
// rerendering instead uses the shared32/64 functions above.
bool can_render_stored_line_cache_planes(std::span<const StoredLinePlane>,const VectorRenderContext&);
struct StoredLineCacheCosts {std::uint64_t storage{},work{};};
// Reference-only joint Line/Face plan: target32,1x/2x, finite positive
// DPI scale, native NULL/affine context; generic Paint keeps its certificate.
StoredLineCacheCosts stored_paint_reference_costs(std::span<const StoredPaintPlane>,const VectorRenderContext&,StoredLineCacheCosts,std::stop_token);
Raster render_stored_paint_reference(std::span<const StoredPaintPlane>,const VectorRenderContext&,std::stop_token,std::uint64_t);
StoredLineCacheCosts stored_line_cache_costs(std::span<const StoredLinePlane>,const VectorRenderContext&,StoredLineCacheCosts previous={},std::stop_token={});
Raster render_stored_line_mask(const StoredLinePlane&,const VectorRenderContext&,std::stop_token={});
Raster render_stored_line_cache_planes(std::span<const StoredLinePlane>,const VectorRenderContext&,std::stop_token={});
// Private print profile only. Actual32 follows the ordinary Common cache;
// actual64 renders Raster8 masks but native source8-to-target64 transfer skips,
// leaving fresh white/A0. Caller completes every logical block and padding.
// Normal display cache admission above remains32-only.
StoredLineCacheCosts stored_line_print_cache_costs(std::span<const StoredLinePlane>,const VectorRenderContext&,StoredLineCacheCosts previous={},std::stop_token={});
Raster render_stored_line_print_cache(std::span<const StoredLinePlane>,const VectorRenderContext&,std::stop_token={});
// Native owner-output temporary surface, before its separate root terminal.
// Sampling is independent of output precision and of normal display caches.
enum class StoredLineOutputSampling {Plain1x,AA2,Fractional4x};
bool can_render_stored_line_output(std::span<const StoredLinePlane>,const VectorRenderContext&,StoredLineOutputSampling);
StoredLineCacheCosts stored_line_output_costs(std::span<const StoredLinePlane>,const VectorRenderContext&,StoredLineOutputSampling,StoredLineCacheCosts previous={},std::stop_token={});
Raster render_stored_line_output(std::span<const StoredLinePlane>,const VectorRenderContext&,StoredLineOutputSampling,std::stop_token={});
//62F4A0 reference-only context: actual target32,1x/2x, finite positive
//owner/target DPI base scale, no fine step. Nonaffine origin remains zero;
//a translated converter uses the native staged matrix/inverse. Ordinary
//output, display and Temp print keep their separate fixed-base admission.
StoredLineCacheCosts stored_line_reference_costs(std::span<const StoredLinePlane>,const VectorRenderContext&,StoredLineOutputSampling,StoredLineCacheCosts,std::stop_token);
Raster render_stored_line_reference(std::span<const StoredLinePlane>,const VectorRenderContext&,StoredLineOutputSampling,std::stop_token);
// Native62F4A0 image output: target32/64, actual owner/target DPI.
StoredLineCacheCosts stored_line_direct_costs(std::span<const StoredLinePlane>,const VectorRenderContext&,StoredLineOutputSampling,StoredLineCacheCosts={},std::stop_token={});
Raster render_stored_line_direct(std::span<const StoredLinePlane>,const VectorRenderContext&,StoredLineOutputSampling,std::stop_token={});
StoredLineCacheCosts stored_paint_direct_costs(std::span<const StoredPaintPlane>,const VectorRenderContext&,StoredLineCacheCosts,std::stop_token);
Raster render_stored_paint_direct(std::span<const StoredPaintPlane>,const VectorRenderContext&,std::stop_token,std::uint64_t);
// Print's owner-sized Temp normal1 cache. Actual WORD samples retain RGB even
// when648000 narrows their combined alpha to zero. Ordinary output keeps its
// separate publication policy. Uses the Plain1x cost plan above.
Raster render_stored_temp_print_cache(std::span<const StoredLinePlane>,const VectorRenderContext&,std::stop_token={});
}
