#pragma once
#include "retas/vector_shape_render.hpp"
namespace retas::vector_render_detail {
struct FaceOccurrence {const VectorFaceGeometry* geometry{};VectorBounds gate,region_gate;std::span<const VectorBounds> model_gates,loop_gates;std::shared_ptr<const VectorShapeTextureRecipe> texture_recipe;};
struct FaceProgramBudget {std::uint64_t storage{},work{},points{},sampled{};bool texture_sources_precharged{};};
struct FacePrograms {
    struct Storage;
    std::shared_ptr<Storage> storage;
    FaceProgramBudget costs;
};
// Original-span point query with the native saved Loop/MC gates, no material
// sampling or rasterization. Caller owns certified geometry and shared budgets.
bool cached_region_contains(const VectorShapeFaceGeometry&,VectorFaceVertex,std::stop_token);
// Private solid/gradation/texture writer programs share the production Face router/fractional
// arithmetic. Gates select physical blocks, never clip the Face within one.
FacePrograms prepare_shape_face_programs(std::span<const FaceOccurrence>,const VectorRenderContext&,FaceProgramBudget,std::stop_token);
// Paint's existing derived Face router is distinct from Shape's saved gates.
FacePrograms prepare_paint_face_programs(std::span<const FaceOccurrence>,const VectorRenderContext&,FaceProgramBudget,std::stop_token);
// Private direct reference context; no widening of stored Shape/Paint paths.
FacePrograms prepare_paint_reference_face_programs(std::span<const FaceOccurrence>,const VectorRenderContext&,FaceProgramBudget,std::stop_token);
FacePrograms prepare_paint_direct_face_programs(std::span<const FaceOccurrence>,const VectorRenderContext&,FaceProgramBudget,std::stop_token);
std::vector<VectorBounds> face_program_blocks(const FacePrograms&);
bool face_program_has_block(const FacePrograms&,std::size_t,VectorBounds);
// Called after the joint executor budget closes; only then expand immutable
// gradation/image sampler snapshots. The private plan is not yet published.
void complete_face_programs(FacePrograms&,std::stop_token);
// Canonical Pixel WORDs (byte*257 for depth32) are converted only at each
// written sample. The Shape owner controls flush and final AA resolve.
bool paint_shape_face_program(const FacePrograms&,std::size_t,VectorBounds,std::span<Pixel>,const VectorRenderContext&,std::stop_token,bool overwrite1x=false,bool fractional4=false);
}
