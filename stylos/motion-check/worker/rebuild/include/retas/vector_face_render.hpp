#pragma once
#include "vector_render.hpp"
#include "vector_gradation.hpp"
#include "vector_texture.hpp"

namespace retas {
// Transient native face graph. Indices are dense runtime identities;
// these records are never serialized as a vector-paint archive.
struct VectorFaceVertex {
    std::int32_t x{},y{};
    bool operator==(const VectorFaceVertex&) const = default;
};
struct VectorFaceSpan {
    std::uint32_t start{},end{};
    std::int32_t control_x{},control_y{};
    bool operator==(const VectorFaceSpan&) const = default;
};
struct VectorFaceModel {
    // 3: fixed linear; 2: fixed quadratic. The linear bit is authoritative,
    // even when a quadratic control coincides with an endpoint.
    std::uint32_t flags{3};
    std::vector<VectorFaceSpan> spans;
    // -1 is a null adjacent loop. Equal owners suppress this internal edge.
    std::int32_t outside_loop{-1},inside_loop{-1};
    bool operator==(const VectorFaceModel&) const = default;
};
struct VectorFaceMember {
    std::uint32_t model{};
    std::int32_t selector{1}; // nonzero: forward/inside; zero: reverse/outside
    bool operator==(const VectorFaceMember&) const = default;
};
struct VectorFaceLoop {
    std::vector<VectorFaceMember> members;
    bool operator==(const VectorFaceLoop&) const = default;
};
struct VectorFaceGeometry {
    // Caller supplies identity-closed native member walks and side ownership.
    // Geometric crossings, internal branches and nested holes retain native
    // emission and original-span query semantics.
    // Admission validates references, identity closure, numeric and resource
    // bounds; it does not prove geometric simplicity/containment/planarity.
    std::vector<VectorFaceVertex> vertices;
    std::vector<VectorFaceModel> models;
    std::vector<VectorFaceLoop> loops;
    std::uint32_t outer_loop{};
    std::vector<std::uint32_t> holes;
    std::uint32_t material_flags{1};
    std::uint64_t color{}; // native AAAABBBBGGGGRRRR
    std::shared_ptr<const VectorGradationMaterial> gradation; // required for low nibble2
    std::shared_ptr<const VectorTextureMaterial> texture; // required for low nibble4
    bool operator==(const VectorFaceGeometry& b) const {
        return vertices==b.vertices&&models==b.models&&loops==b.loops&&outer_loop==b.outer_loop&&holes==b.holes&&
            material_flags==b.material_flags&&color==b.color&&bool(gradation)==bool(b.gradation)&&(!gradation||*gradation==*b.gradation)&&
            bool(texture)==bool(b.texture)&&(!texture||*texture==*b.texture);
    }
};
struct VectorFaceRaster {
    Raster pixels;
    RasterSelection coverage{Rect{}};
    std::size_t covered_pixels{};
    Rect dirty{};
};
// Solid-only same-DPI base_scale16, oversample1, normal32/64 overwrite. Coverage
// includes alpha-zero writes. Invalid graphs throw invalid_argument, bounded
// work/storage violations throw length_error, cancellation throws runtime_error.
// Complete routing/span preparation precedes output raster/mask allocation.
// Limits: 100000 vertices/models/spans/members, 1 Mi routed points,
// 128 Mi work units, 512 MiB combined logical tiles/mask/preparation storage.
bool can_render_vector_face_geometry(const VectorFaceGeometry&,const VectorRenderContext&);
VectorFaceRaster render_vector_face_geometry(const VectorFaceGeometry&,
    const VectorRenderContext&,std::stop_token stop={});
// Original-span native region predicate, including the +0.125 axis probe and
// orthogonal-ray edge handling used by no-routed-point block fallback.
bool vector_face_region_contains(const VectorFaceGeometry&,VectorFaceVertex raw_point);
// Pure AA2 routing certificate for native material queries. Includes zero
// coverage tails and same-floor floor(right) queries, checks actual row
// interval endpoints, and never reads/decompresses/copies the embedded image.
void admit_vector_face_texture_queries(const VectorFaceGeometry&,
    const VectorRenderContext&,const VectorTextureMaterial&);
struct VectorFacePlaneView {
    // Saved plane order; faces retain the native forward list order.
    std::span<const VectorFaceGeometry* const> faces;
};
// Same-DPI base_scale16/oversample2 normal32/64 solid/gradient/Texture owner.
// Full raw flags and WORD colors survive; gradient32 fractional writes use
// native648280, distinct from solid648180. Raw queries use global logical
// AA samples*8 plus owner origin*16; final same-floor partial queries use
// floor(right) although the physical write pointer advances. At most100000
// stop occurrences over all faces (shared materials billed per face), with
// prepared samplers and work/storage admission before raster allocation.
// Texture snapshots are deep and shared once per distinct material pointer;
// source+snapshot tiles and all graph counts are billed before copying.
// Texture32 uses648180 single-pass BYTE edges; Texture64 uses6483C0 DWORDs.
// Actual routed query intervals must fit signed cursor conversion, including
// zero-coverage tail queries; unpainted remote context coordinates are ignored.
// Planes are
// painted in reverse order into shared companion samples. Each plane paints
// its ordered faces into fresh scratch, then composes once; the whole owner
// resolves once using the native AA byte/WORD arithmetic. This API receives
// already admitted owner planes: visibility, archive/companion families,
// opacity and transforms are the caller's responsibility.
// Alpha-zero full samples overwrite scratch; zero-alpha fractional samples
// leave it unchanged. The derived raster carries no overwrite coverage.
// Structural/numeric/planar prerequisites and error types match the direct
// API above. Resource limits apply to the whole batch, not independently to
// each face. A null face pointer is invalid. Empty planes/owners are allowed.
bool can_render_vector_face_planes(std::span<const VectorFacePlaneView>,const VectorRenderContext&);
Raster render_vector_face_planes(std::span<const VectorFacePlaneView>,
    const VectorRenderContext&,std::stop_token stop={});
// Uses the same complete physical-block routing and owner resolve as render.
Pixel vector_face_planes_pixel(std::span<const VectorFacePlaneView>,
    const VectorRenderContext&,int x,int y,std::stop_token stop={});
}
