#include "svd_render.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace retas::svd_detail {
namespace {
struct Position {
    int x, y;
};
void line(Raster &image, const Projection &projection, Position a, Position b, int pen, Pixel color,
          std::uint64_t &budget) {
    const int dx = std::abs(b.x - a.x), dy = std::abs(b.y - a.y);
    const int sx = b.x >= a.x ? 1 : -1, sy = b.y >= a.y ? 1 : -1;
    const bool steep = dy > dx;
    const int major = std::max(dx, dy), minor = std::min(dx, dy);
    // Skip offscreen steps without restarting the Bresenham error at the border.
    const int origin = steep ? a.y : a.x, sign = steep ? sy : sx,
              extent = steep ? projection.height : projection.width;
    const int first = std::max(0, sign > 0 ? -origin : origin - extent);
    const int last = std::min(major, sign > 0 ? extent - origin : origin);
    if (first > last)
        return;
    spend(budget, std::uint64_t(last - first + 1) * pen * pen);
    for (int i = first; i <= last; ++i) {
        const auto advance = major ? int((std::int64_t(2) * i * minor + major) / (2 * major)) : 0;
        const int x = a.x + sx * (steep ? advance : i) - pen / 2;
        const int y = a.y + sy * (steep ? i : advance) - pen / 2;
        if (x < 0)
            continue;
        for (int j = std::max(0, y); j < std::min(projection.height, y + pen); ++j)
            for (int k = x; k < std::min(projection.width, x + pen); ++k)
                image.set_pixel(k, j, color);
    }
}

void frame(Raster &image, const Projection &p, Box b, double angle, bool safe, std::uint64_t &budget) {
    constexpr double tick = 0.5 / 2.54;
    const double cx = b.left + (b.right - b.left) * 0.5, cy = b.top + (b.bottom - b.top) * 0.5;
    const Position center{sample_coordinate(cx * p.dpi), sample_coordinate(cy * p.dpi)};
    const double radians = (std::abs(angle) == 180 ? 0 : -angle) * std::numbers::pi / 180;
    const double cosine = std::cos(radians), sine = std::sin(radians);
    const auto transform = [&](Point pt) {
        int x = sample_coordinate(pt.x * p.dpi), y = sample_coordinate(pt.y * p.dpi);
        if (radians != 0) {
            const auto dx = x - center.x, dy = y - center.y;
            x = sample_coordinate(dx * cosine + dy * sine) + center.x;
            y = sample_coordinate(dy * cosine - dx * sine) + center.y;
        }
        return Position{x - p.x, y - p.y};
    };
    const Pixel color = safe ? Pixel::rgba8(179, 179, 179) : Pixel::rgba8(0, 0, 0);
    const auto segment = [&](Point a, Point end) {
        line(image, p, transform(a), transform(end), safe ? 1 : 2, color, budget);
    };
    if (safe) {
        const Point points[] = {{b.left + tick, b.top},     {b.right - tick, b.top},
                                {b.right, b.top + tick},    {b.right, b.bottom - tick},
                                {b.right - tick, b.bottom}, {b.left + tick, b.bottom},
                                {b.left, b.bottom - tick},  {b.left, b.top + tick}};
        for (int i = 0; i < 8; ++i)
            segment(points[i], points[(i + 1) % 8]);
        segment({cx - tick, cy}, {cx + tick, cy});
        segment({cx, cy - tick}, {cx, cy + tick});
    } else {
        const Point points[] = {{b.left, b.top}, {b.right, b.top}, {b.right, b.bottom}, {b.left, b.bottom}};
        for (int i = 0; i < 4; ++i)
            segment(points[i], points[(i + 1) % 4]);
        segment({cx, b.top}, {cx, b.top + tick});
        segment({b.right, cy}, {b.right - tick, cy});
        segment({cx, b.bottom}, {cx, b.bottom - tick});
        segment({b.left, cy}, {b.left + tick, cy});
    }
}
} // namespace

void render_frames(Raster &image, const Document &doc, const Projection &projection, std::uint64_t &budget) {
    frame(image, projection, projection.outer, 0, false, budget);
    frame(image, projection, projection.safe, 0, true, budget);
    for (const auto &f : doc.frames) {
        frame(image, projection,
              {f.origin.x, f.origin.y, f.origin.x + f.size.x * f.percent / 100.,
               f.origin.y + f.size.y * f.percent / 100.},
              f.angle, false, budget);
    }
}
} // namespace retas::svd_detail
