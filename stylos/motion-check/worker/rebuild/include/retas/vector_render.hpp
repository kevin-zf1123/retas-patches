#pragma once

#include "vector_geometry.hpp"
#include <optional>
#include <span>
#include <stop_token>

namespace retas {

struct VectorRenderContext {
    int width{},height{},depth{32};
    double resolution{144.0};
    // RVVectorDraw constructor scale: raw model units per destination pixel.
    // Same-DPI normal context uses 16; other scales require another profile.
    // Archive coordinates are not pixels.
    double base_scale{16.0};
    VectorPoint origin{}; // integer destination-pixel origin subtracted after quantization
    std::optional<VectorBounds> clip; // half-open destination-local rectangle
    unsigned oversample{1}; // 1x direct, 2x fractional spans, 4x modal resolve
    // Native affine converter for clipboard and direct reference sources:
    // complete geometry quantization and distinct gradient query coordinates.
    // Persistent normal-display callers keep false.
    bool clipboard_affine{};
    std::array<double,6> clipboard_matrix{0.0625,0,0,0,0.0625,0};
    std::array<double,6> clipboard_inverse{16,0,0,0,16,0};
    //5E0340's converter argument refines curve sampling even with a1x bit sink.
    bool converter_fine_step{};
    bool minimum_line_width{};
};

// Transient native endpoint decoration. These named fields correspond to the
// fourteen payload DWORDs of the runtime descriptor; no archive is changed.
// Kinds 1/4 emit middle; kinds 0/3 omit it. Kind 2 samples a quadratic half:
// point0/control0/middle or middle/control1/point2. Only kind-2 controls may
// be nonzero. Sampling uses deterministic binary64/default CRT PC53 arithmetic
// with F32 parameters; rendering-time native FPU state remains unverified.
// Kind1 trims original body samples strictly: at the head, second_trim is
// the forward-side lower bound and first_trim the reverse-side lower bound;
// at the tail, first_trim is forward upper and second_trim reverse upper.
// Equal/reversed bounds keep descriptor points and insert no new samples.
struct VectorEndpointProfile {
    std::uint32_t first_kind{},second_kind{};
    VectorPoint point0,control0,middle,control1,point2;
    float first_trim{},second_trim{};
};
struct VectorSpanProfiles {
    std::optional<VectorEndpointProfile> start,end;
};

// Admission performs all geometry/resource checks without allocating a raster.
// Unsupported profiles must be rejected by decode_vector_line_geometry first.
bool can_render_vector_line_geometry(const VectorLineGeometry&,const VectorRenderContext&);
// Returns a new transient surface. Never mutates archive bytes or a plane raster.
// Invalid/unsupported inputs throw invalid_argument; resource caps throw
// length_error; cancellation throws runtime_error before a result is published.
Raster render_vector_line_geometry(const VectorLineGeometry&,const VectorRenderContext&,
                                  std::stop_token stop={});
// Uses the same polygon, span and resolve path with a one-pixel clip.
Pixel vector_line_geometry_pixel(const VectorLineGeometry&,const VectorRenderContext&,
                                 int x,int y,std::stop_token stop={});

// Profiles are in forward saved geometry.strokes/spans order, independently
// of render traversal order. A nonempty array must have one entry per span.
// Empty arrays use the ordinary renderer exactly. Decorations admit legacy
// fixed-linear spans and a strict fresh quadratic profile: one isolated line
// owner/color/float width; unique identity-open source vertices; complete raw
// Model, member, subcurve and reciprocal-vertex ownership; Model2 flags; null
// outer profiles; and zero-trim profile halves of kind 0 or 2. The quadratic
// gate rebuilds line/member stroke bounds from the fresh graph and active
// profiles before private subcurve predicates; cached model scalars are not
// its source. Native no-point block probes use profile-expanded bounds before
// the stored line/control/width analytic predicate. This does not admit
// persistent archived endpoint profiles or custom Model families.
bool can_render_vector_line_geometry(const VectorLineGeometry&,std::span<const VectorSpanProfiles>,
                                    const VectorRenderContext&);
Raster render_vector_line_geometry(const VectorLineGeometry&,std::span<const VectorSpanProfiles>,
                                  const VectorRenderContext&,std::stop_token stop={});
Pixel vector_line_geometry_pixel(const VectorLineGeometry&,std::span<const VectorSpanProfiles>,
                                 const VectorRenderContext&,int x,int y,std::stop_token stop={});

// Owner-local visible planes in archive order. Native normal-cache rendering
// reverses plane order and resolves the shared AA companion once per owner.
bool can_render_vector_line_geometries(std::span<const VectorLineGeometry>,const VectorRenderContext&);
Raster render_vector_line_geometries(std::span<const VectorLineGeometry>,const VectorRenderContext&,
                                    std::stop_token stop={});
Pixel vector_line_geometries_pixel(std::span<const VectorLineGeometry>,const VectorRenderContext&,
                                  int x,int y,std::stop_token stop={});

} // namespace retas
