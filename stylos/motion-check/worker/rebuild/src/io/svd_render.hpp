#pragma once
#include "svd_document.hpp"

namespace retas::svd_detail {
struct Box {
    double left{}, top{}, right{}, bottom{};
};
struct Projection {
    double dpi{};
    int x{}, y{}, width{}, height{};
    Box outer, safe;
};
// All raster paths use integer sample positions. Bound before narrowing, also
// keeping the products in the integer line and polygon walkers representable.
int sample_coordinate(double);
Projection project(const Document &, const SvdImportSettings &);
void render_frames(Raster &, const Document &, const Projection &, std::uint64_t &budget);
void render_vectors(Raster &, const VectorDrawing &, const Projection &, int samples, int density,
                    bool direction, std::uint64_t &budget);
void spend(std::uint64_t &remaining, std::uint64_t cost);
} // namespace retas::svd_detail
