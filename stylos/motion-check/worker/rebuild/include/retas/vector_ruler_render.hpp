#pragma once
#include "vector_render.hpp"
#include <map>

namespace retas {
struct VectorRulerCommand {
    // Nonnegative: index in plane.lines.strokes. Negative: sampled circle.
    std::int32_t stroke{-1},center_x{},center_y{},radius{};
    VectorBounds gate; // Native outer Ruler gate, independent of span bounds.
};
struct VectorRulerPlaneGeometry {
    std::uint64_t color{};
    VectorLineGeometry lines;
    std::map<std::int32_t,VectorBounds> model_bounds; // Persistent MC stroke gates.
    std::vector<VectorRulerCommand> commands; // Forward Ruler member order.
    // Decoder bills retained wire/graph storage once on the first plane.
    // Transient callers can reserve other simultaneously live source bytes.
    std::uint64_t reserved_storage{};
    // Owning saved forward strokes/spans order; empty means all NULL.
    // Nonempty has one entry per span, preserving kind0..4 cap overrides
    // and strict independent kind1 body trims under original LC/MC gates.
    std::vector<VectorSpanProfiles> line_profiles;
};
// Raw Ruler owner and all siblings are admitted together. Ordinary Line
// companions participate in intrusive ownership but have no Ruler draw pass.
// Lines admit isolated open/closed stored Model0..3 and retention0x30;
// new variable profile/retained/closed spans need two effective width nodes.
// circle draws ignore their nullable Line and inactive other parameters.
std::vector<VectorRulerPlaneGeometry> decode_vector_ruler_geometries(const Layer&);
// Saved planes, all forward commands; normal1x/2x contexts only. Each physical
// block starts a plane's material from Common.color. An intersecting Line gate
// changes that material, including a Line with no spans; later circles inherit
// it. Planes traverse backwards, AA scratch composes once per plane, then the
// owner resolves once. Circle coordinates use native F32 sampling and the
// existing Line polygon/AA writer, including center-in-clip empty-route fill.
// Modern numeric/resources checks precede output allocation; raw archives are
// never changed. Host libm/default PC53 is not an original x87 pixel golden.
bool can_render_vector_ruler_geometries(std::span<const VectorRulerPlaneGeometry>,const VectorRenderContext&);
Raster render_vector_ruler_geometries(std::span<const VectorRulerPlaneGeometry>,const VectorRenderContext&,std::stop_token={});
Pixel vector_ruler_geometries_pixel(std::span<const VectorRulerPlaneGeometry>,const VectorRenderContext&,int x,int y,std::stop_token={});
}
