#pragma once
#include "raster.hpp"
#include <array>
#include <stop_token>

namespace retas {
struct VectorTextureMaterial {
    std::uint32_t flags{}; // low nibble1 repeat,2 clamp; other bits retained
    std::int32_t origin_x{},origin_y{};
    std::uint32_t scale_x_bits{},scale_y_bits{},rotation_bits{},shear_x_bits{};
    Raster image; // embedded image DPI is metadata, independent of owner DPI
    bool operator==(const VectorTextureMaterial&) const;
};
// Validate dense canonical1/32/64 storage and finite nonzero scales, finite
// shear, rotation radians strictly inside(-2^63,2^63). Returns source tile/map
// bytes before any snapshot allocation. Native C2 out-of-range angles remain
// raw but are explicitly unsupported by this portable sampler.
// Missing tiles, empty images and unknown cursor modes reject explicitly.
std::uint64_t vector_texture_storage(const VectorTextureMaterial&);
std::array<double,6> vector_texture_inverse_matrix(const VectorTextureMaterial&);
void admit_vector_texture_rectangle(const VectorTextureMaterial&,std::int32_t left,
    std::int32_t top,std::int32_t right,std::int32_t bottom);
void admit_vector_texture_rectangle(const std::array<double,6>& inverse,
    std::int32_t left,std::int32_t top,std::int32_t right,std::int32_t bottom);
// Native5AACF0 inverse matrix and5AAEA0 integer cursor. The constructor makes
// a private deep snapshot of every tile, so writable Raster/shared_ptr aliases
// cannot change admitted sampling. Binary64 coefficients preserve native store
// order; stdlib trig agreement with original x87 remains unverified.
class VectorTextureSampler {
    VectorTextureMaterial material_;
    std::array<double,6> matrix_{};
    std::array<double,2> transform(std::int32_t,std::int32_t) const;
public:
    explicit VectorTextureSampler(const VectorTextureMaterial&,std::stop_token stop={});
    // Clipboard5AACF0 composes a caller inverse matrix before cursor sampling.
    VectorTextureSampler(const VectorTextureMaterial&,const std::array<double,6>& effective_inverse,std::stop_token stop={});
    void admit_rectangle(std::int32_t left,std::int32_t top,std::int32_t right,std::int32_t bottom) const;
    Pixel sample(std::int32_t raw_x,std::int32_t raw_y) const; // full WORD channels
};
}
