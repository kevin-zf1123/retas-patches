#pragma once
#include "retas/vector_shape_render.hpp"
#include "../io/vector_texture_image.hpp"
namespace retas::vector_render_detail {
// Scalar-only certificate input; the empty local Raster is never published
// as a texture material or passed to a sampler.
inline VectorTextureMaterial shape_texture_metadata(const VectorShapeTextureRecipe& r){
    VectorTextureMaterial m;m.flags=r.flags;m.origin_x=r.origin_x;m.origin_y=r.origin_y;
    m.scale_x_bits=r.scale_x_bits;m.scale_y_bits=r.scale_y_bits;m.rotation_bits=r.rotation_bits;m.shear_x_bits=r.shear_x_bits;return m;
}
inline std::span<const std::uint8_t> shape_texture_image(const VectorShapeTextureRecipe& r){
    if(!r.leaf||r.leaf->size()>64U*1024U*1024U||!r.image_prefix_size||r.image_prefix_size>r.leaf->size())throw std::invalid_argument("Invalid Shape texture recipe image prefix");
    return std::span<const std::uint8_t>(*r.leaf).first(r.image_prefix_size);
}
inline std::uint64_t shape_texture_storage(const VectorShapeTextureRecipe& r){
    if((r.flags&15)!=1&&(r.flags&15)!=2)throw std::invalid_argument("Unknown Shape texture cursor mode");
    (void)vector_texture_inverse_matrix(shape_texture_metadata(r));
    try{return vector_archive_detail::texture_image_storage(shape_texture_image(r));}
    catch(const FormatError&){throw std::invalid_argument("Malformed Shape texture image framing");}
}
}
