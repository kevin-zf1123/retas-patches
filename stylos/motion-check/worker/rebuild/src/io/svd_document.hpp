#pragma once
#include "retas/svd.hpp"
#include "retas/geometry.hpp"

namespace retas::svd_detail {
inline constexpr double inches_per_cm = std::bit_cast<double>(std::uint64_t{0x3fd93264c993264c});
struct Geometry {
    Point scale, size;
    double percent{};
    int frame_position{};
    std::array<double, 4> margins{};
};
struct DrawFrame {
    int percent{};
    double angle{};
    Point origin, size;
};
// SVD export selects the Special view and borrows its main drawing. The
// independent direction DB is overlaid only when the import option is set.
struct Document {
    Geometry geometry;
    Point origin;
    std::array<double, 4> tv_margins{};
    std::vector<DrawFrame> frames;
    std::span<const std::uint8_t> main_database, direction_database;
    int direction_density{};
};
struct Edge {
    std::uint32_t flags{}, id{}, face_order{};
    Pixel ink, face;
    Point start, end;
    float start_scale{}, end_scale{}, radius{};
    int effect{};
};
struct VectorDrawing {
    std::vector<Edge> edges;
    unsigned effects{};
};
VectorDrawing read_drawing(std::span<const std::uint8_t>, std::uint64_t workspace);
std::array<int, 2> image_size(const Geometry &, const SvdImportSettings &);
Document read_document(std::span<const std::uint8_t>);
} // namespace retas::svd_detail
