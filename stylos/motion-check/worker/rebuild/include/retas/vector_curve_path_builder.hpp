#pragma once
#include "vector_render.hpp"

namespace retas {
struct VectorCurvePathContour {
    VectorLineGeometry geometry;
    std::vector<VectorSpanProfiles> profiles;
};
// PLLN Curve builds a fixed-width raw-1/16 quadratic chain with native
// signed-shift midpoints, recursive half-splits, short-span pruning/control
// reattachment, then ordered Model2 partitions and fresh kind-0/kind-2 endpoint
// profiles.
// Models retain shared vertex identities and forward selector1 members; only
// the global first/last spans have outer caps. Profiles accompany the geometry
// in saved span order and are transient, not persistent archive decoding.
// Producer support does not imply unrestricted renderer admission: the
// ordinary AA renderer accepts only its strict fresh Model2/profile subset.
// Closed two-point and fully pruned paths return an empty contour.
// Integer sums/differences, active profile points and bounds must fit the
// bounded signed32/raw domain. Invalid inputs throw invalid_argument, resource
// limits length_error, and cancellation runtime_error before publication.
// Arithmetic follows default PC53/binary64 with F32 stores; rendering-time
// native FPU state and original runtime pixels remain unverified.
VectorCurvePathContour build_vector_curve_path_contour(
    std::span<const VectorPoint>,float raw_half_width,std::uint64_t runtime_color,
    bool close_shape,std::stop_token stop={});
// Fresh fill-only centerline metadata uses native width0/color0 and the same
// ordered normalization/partition. It has no endpoint profiles and is not a
// zero-width outline drawing API. The outline entry still requires H>0.
VectorLineGeometry build_vector_curve_path_centerline(
    std::span<const VectorPoint>,bool close_shape,std::stop_token stop={});
}
