#pragma once
#include "svd_render.hpp"

namespace retas::svd_detail {
struct Vertex {
    int x, y;
    bool corner;
    bool guide{};
};
using Polygon = std::vector<Vertex>;
struct Shape {
    Pixel color;
    std::vector<Polygon> contours;
};
Vertex project_point(Point, const Projection &, int samples, bool corner);
Polygon smooth(const Polygon &);
std::vector<Shape> stroke_shapes(const std::vector<const Edge *> &, const Projection &, int samples);
std::vector<Shape> face_shapes(const VectorDrawing &, const Projection &, int samples, std::uint64_t &budget);
} // namespace retas::svd_detail
