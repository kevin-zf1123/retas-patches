#pragma once
#include "retas/vector_face_archive.hpp"
#include "retas/vector_shape_render.hpp"
namespace retas::vector_archive_detail {
void publish_paint_storage(Layer&,const VectorPaintArchive&,Bytes);
// Production Face projection retains the native member walk and validates
// structure/numeric domains. Geometry is admitted by the bounded scanline
// consumer rather than the optional simple-contour certificate API.
std::vector<VectorPaintPlaneGeometry> project_native_paint_faces(
    const VectorPaintArchive&,const Layer&,std::uint64_t reserved_storage=0);
// Explicit ordinary Face borrows. The complete archive is validated, then
// each selected solid/gradation/texture Face keeps its native structural closure. No
// Paint family or separate Border membership is imposed on Shape borrows.
struct ShapeFaceProjection {std::int32_t face_index{};VectorShapeFaceGeometry face;};
std::vector<ShapeFaceProjection> project_shape_faces(VectorPaintArchive&,std::span<const std::int32_t>,std::uint64_t reserved_storage);
// BFIL queries the Border's ordered Regions, including unpainted ones. Keep
// saved gates without allocating persistent Faces or interpreting materials.
struct PaintRegionProjection {std::int32_t region_index{};VectorShapeFaceGeometry region;};
std::vector<PaintRegionProjection> project_paint_regions(const VectorPaintArchive&,const Layer&);
}
