#pragma once
#include "raster.hpp"
#include <memory>

namespace retas {
namespace vector_render_detail {class GradationAdmission;}
struct VectorGradationStop {
    std::uint64_t color{}; // AAAABBBBGGGGRRRR, all WORDs retained
    std::uint32_t position_bits{};
    bool operator==(const VectorGradationStop&) const = default;
};
struct VectorGradationMaterial {
    std::uint32_t flags{}; // low nibble1 linear or2 elliptic; F0 selects dither
    std::int32_t start_x{},start_y{},end_x{},end_y{};
    std::uint32_t minor_ratio_bits{}; // inactive for linear
    std::vector<VectorGradationStop> stops; // native file order
    bool operator==(const VectorGradationMaterial&) const = default;
};
// Immutable native59A310/59A640/59A530 sampler, prepared once. An admitted
// private snapshot prevents later mutable aliases from changing its state.
// Nonempty
// finite nondecreasing stops (including ties/outside0..1) and Type1/2 only.
// At most100000 stops; DWORD differences must not wrap or equalINT_MIN.
// sample returns full WORD channels; face writers perform depth adaptation.
// PC53/binary64 with native F32 stores and stdlib trig: original Windows
// FPU/trigonometric runtime pixel agreement remains unverified.
class VectorGradationSampler {
    std::shared_ptr<const VectorGradationMaterial> material_;
    std::int32_t dx_{},dy_{};
    std::int64_t squared_{};
    double u_{},v_{},cosine_{},negative_sine_{};
    float parameter(std::int32_t,std::int32_t) const;
    VectorGradationSampler(std::shared_ptr<const VectorGradationMaterial>,bool freeze);
    friend class vector_render_detail::GradationAdmission;
public:
    explicit VectorGradationSampler(std::shared_ptr<const VectorGradationMaterial>);
    Pixel sample(std::int32_t raw_x,std::int32_t raw_y) const;
    // Certify all subsequent queries before raster allocation.
    void admit_rectangle(std::int32_t left,std::int32_t top,
        std::int32_t right,std::int32_t bottom) const;
};
}
