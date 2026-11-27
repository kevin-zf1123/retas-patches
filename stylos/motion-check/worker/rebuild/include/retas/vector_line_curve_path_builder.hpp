#pragma once
#include "vector_curve_path_builder.hpp"

namespace retas {
enum class VectorLineCurveKind { Line,Quadratic };
struct VectorLineCurveSeed {
    VectorLineCurveKind kind{VectorLineCurveKind::Line};
    VectorPoint start,control,end;
};
struct VectorLineCurveMaterial {
    float raw_half_width{},raw_inlet_length{},raw_outlet_length{};
    std::uint64_t runtime_color{};
    bool pattern_rect{};
    bool minimum_width{};
};
// Native owner-local raw1/16 LINE/CURV seed and material producer. LINE uses
// its signed arithmetic-shift midpoint; Quadratic keeps the supplied control.
// Endpoint taper precedes native quadratic normalization and Model partition.
// The returned transient graph/profiles still require consumer admission.
// Integer coordinates and lengths are bounded by 2^30; finite H is 0..65536.
// Native signed32 midpoint/difference overflow is rejected. Curves/knots are
// bounded by 100000 and work by 128 Mi units. Ordered duplicate seam parameters
// are retained; the strict archived geometry projector is not invoked.
// Invalid inputs, resource limits and stop throw before publication. Native
// runtime/FPU and FSIN hardware precision are not asserted by this API.
VectorCurvePathContour build_vector_line_curve_contour(
    const VectorLineCurveSeed&,const VectorLineCurveMaterial&,
    std::stop_token stop={});
}
