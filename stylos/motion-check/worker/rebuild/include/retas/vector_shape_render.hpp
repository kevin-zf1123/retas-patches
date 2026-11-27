#pragma once
#include "vector_face_render.hpp"
#include <map>
namespace retas {
struct VectorShapeCommand {
    std::int32_t stroke{-1},face{-1}; // Local projected indices; negative is NULL.
    VectorBounds gate; // Line bounds if present, otherwise Region bounds.
};
// Deferred source: owns the complete raw Texture leaf, without decoded tiles.
// The framed image prefix and scalar metadata are admitted in the actual
// rendering context before decompression and immutable sampler construction.
struct VectorShapeTextureRecipe {
    std::uint32_t flags{};
    std::int32_t origin_x{},origin_y{};
    std::uint32_t scale_x_bits{},scale_y_bits{},rotation_bits{},shear_x_bits{};
    std::shared_ptr<const Bytes> leaf;
    std::size_t image_prefix_size{};
};
struct VectorShapeFaceGeometry {
    VectorFaceGeometry geometry;
    VectorBounds region_bounds; // Must equal loop_bounds[geometry.outer_loop].
    std::vector<VectorBounds> model_bounds,loop_bounds; // Native inner four-comparison gates.
    std::shared_ptr<const VectorShapeTextureRecipe> texture_recipe; // Alternative to geometry.texture.
};
struct VectorShapePlaneGeometry {
    VectorLineGeometry lines;
    std::map<std::int32_t,VectorBounds> model_bounds;
    std::vector<std::shared_ptr<const VectorShapeFaceGeometry>> faces;
    std::vector<VectorShapeCommand> commands; // Saved forward member order.
    std::uint64_t reserved_storage{};
    // Saved forward strokes/spans order; empty means all NULL. Nonempty has
    // one entry per span and preserves non-NULL kind0..4 cap overrides. Kind1
    // trims in[0,1] strictly filter original samples independently per side;
    // equal/reversed windows remain valid. Only
    // active descriptor points/controls are projected; unused raw fields stay
    // in the document archive. Cached LC/MC gates are never rebuilt here.
    std::vector<VectorSpanProfiles> line_profiles;
};
// Complete Shape owner/hidden sibling intrusive lists are admitted jointly.
// Only each command's first Region Face is borrowed. This projection admits
// certified quadratic/linear solid/gradation/texture Faces and isolated
// open/closed Lines with NULL or Model0..3 kind0..4 stored descriptors.
// New variable profile/retained/closed spans require two effective width
// nodes, finite increasing parameters from0 to1 and bounded scales/widths.
// Saved MC retention bits0x10/0x20 may accompany these models. Transient
// Face flags normalize to2/3; raw archives retain all flags. Unknown bits
// and cross-model attachments reject; unretained open-NULL admission stays.
// Other active Face materials/profiles reject; inactive raw records survive.
std::vector<VectorShapePlaneGeometry> decode_vector_shape_geometries(const Layer&);
// Normal same-DPI1x/2x32/64. Planes reverse; commands reverse at1x, forward
// at2x. Each command has a raw outer block gate, Line then first Face. At1x
// both compose under prior alpha; at2x each Line/Face flushes into companion,
// followed by one owner resolve. Line and Face fractional writers differ.
// All graph/numeric/storage/work admission precedes output allocation.
bool can_render_vector_shape_geometries(std::span<const VectorShapePlaneGeometry>,const VectorRenderContext&);
Raster render_vector_shape_geometries(std::span<const VectorShapePlaneGeometry>,const VectorRenderContext&,std::stop_token={});
Pixel vector_shape_geometries_pixel(std::span<const VectorShapePlaneGeometry>,const VectorRenderContext&,int x,int y,std::stop_token={});
}
