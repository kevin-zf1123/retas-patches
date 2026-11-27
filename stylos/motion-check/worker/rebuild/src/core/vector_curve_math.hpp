#pragma once
#include "retas/vector_geometry.hpp"
#include "retas/vector_line_curve_path_builder.hpp"
#include <optional>
namespace retas::vector_curve_detail {
//553970 initial1SC material, before5BA580 persistent normalization.
//Reuses the same seed/taper engine as the public transient contour producer.
VectorLineGeometry build_line_curve_initial_seed(const VectorLineCurveSeed&,
    const VectorLineCurveMaterial&,std::stop_token stop={});
// Persistent callers share their parse/normalization work counter with taper.
VectorLineGeometry build_line_curve_initial_seed(const VectorLineCurveSeed&,
    const VectorLineCurveMaterial&,std::uint64_t& work,std::stop_token stop);
//56B600 and5A4AF0, shared by persistent edits and fresh path construction.
float heading(VectorPoint,VectorPoint);
//596CD0(testBounds=0): first microgrid intersection, without finite gates.
// Temporary coordinates are not restricted to the persistent raw point range;
// consumers check signed32 conversion before distance-based candidate rejection.
std::optional<VectorPoint> first_infinite_intersection(VectorPoint,VectorPoint,VectorPoint,VectorPoint);
VectorPoint reattach(VectorPoint other,VectorPoint old,VectorPoint replacement,VectorPoint control);
}
