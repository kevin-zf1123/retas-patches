#pragma once
#include "vector_probe.hpp"
namespace retas::vector_query_detail {
struct Hit {VectorFaceVertex point;float target_parameter{},probe_parameter{};};
struct Nearest {VectorFaceVertex point;float parameter{};};
// Compatibility raw query: retains its existing traversal and duplicate hits.
std::vector<Hit> cross(const vector_probe_detail::Curve& target,const vector_probe_detail::Curve& probe,vector_probe_detail::Budget);
//596F40/593A70(mode0): typed dispatch without scaling, preserving t roles.
std::vector<Hit> cross_mode0(const vector_probe_detail::Curve& target,const vector_probe_detail::Curve& probe,vector_probe_detail::Budget);
//593A70(mode0): target is always RVBezier, including its saved control;
// probe.linear alone selects RVLinearLine. No scaling or parameter swapping.
std::vector<Hit> cross_obstacle(const vector_probe_detail::Curve& target_bezier,const vector_probe_detail::Curve& probe,vector_probe_detail::Budget);
//596F40/593A70(mode1): RTTI dispatch, DWORD x4 for Bezier recursion and
// arithmetic-shift output x/y by2, with unchanged F32 parameters. Both linear
// operands bypass scaling. Each consumed (possibly scaled) coordinate must
// have magnitude <=536870908. Order and duplicate CROSS records are retained.
std::vector<Hit> cross_mode1(const vector_probe_detail::Curve& target,const vector_probe_detail::Curve& probe,vector_probe_detail::Budget);
//596730/5927E0 return the best integer polyline point, not curve(t) afterward.
Nearest nearest(const vector_probe_detail::Curve&,VectorFaceVertex,vector_probe_detail::Budget);
}
