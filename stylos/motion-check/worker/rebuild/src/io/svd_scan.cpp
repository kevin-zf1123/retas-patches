#include "svd_path.hpp"
#include <algorithm>
#include <cmath>
#include <bit>
#include <map>
#include <optional>

namespace retas::svd_detail {
namespace {
// A sparse set of small tiles keeps a diagonal stroke proportional to its
// covered area even on a very wide canvas.
struct Coverage {
    using Tile = std::array<std::uint16_t, 32 * 32>;
    std::map<std::pair<int, int>, Tile> tiles;
    void add(int x, int y, unsigned bit) {
        auto key = std::pair{y / 32, x / 32};
        if (tiles.size() >= 8192 && !tiles.contains(key))
            throw std::length_error("WCEL coverage exceeds memory limit");
        auto [tile, added] = tiles.try_emplace(key);
        tile->second[(y % 32) * 32 + x % 32] |= std::uint16_t(1u << bit);
    }
};

void fill(const std::vector<Polygon> &polygons, const Projection &p, int samples, Coverage &mask,
          std::uint64_t &budget) {
    struct Crossing {
        int top, bottom;
        double x, slope;
    };
    const int right = p.width;
    std::vector<Crossing> edges;
    int first = p.height * samples, last = 0;
    for (const auto &polygon : polygons)
        for (std::size_t i = 0; i < polygon.size(); ++i) {
            auto a = polygon[i], b = polygon[(i + 1) % polygon.size()];
            if (a.y == b.y)
                continue;
            if (a.y > b.y)
                std::swap(a, b);
            const int top = std::max(a.y, 0), bottom = std::min(b.y, p.height * samples);
            if (top >= bottom)
                continue;
            const double slope = double(b.x - a.x) / (b.y - a.y);
            double crossing = a.x;
            spend(budget, top - a.y);
            for (int i = a.y; i < top; ++i)
                crossing += slope;
            edges.push_back({top, bottom, crossing, slope});
            first = std::min(first, top);
            last = std::max(last, bottom);
        }
    std::vector<double> intersections;
    for (int y = first; y < last; ++y) {
        spend(budget, edges.size());
        intersections.clear();
        for (auto &e : edges)
            if (y >= e.top && y < e.bottom) {
                intersections.push_back(e.x);
                e.x += e.slope;
            }
        std::sort(intersections.begin(), intersections.end());
        for (std::size_t i = 1; i < intersections.size(); i += 2) {
            int x = int(intersections[i - 1]), end = int(intersections[i]);
            if (end <= 0 || x >= right * samples)
                continue;
            x = std::max(x, 0);
            end = std::min(end, right * samples - 1);
            spend(budget, std::uint64_t(end - x + 1));
            for (; x <= end; ++x)
                mask.add(x / samples, y / samples, (y % samples) * samples + x % samples);
        }
    }
}
Pixel combine(Pixel dest, Pixel ink, unsigned as, unsigned limit) {
    const auto ad = unsigned(dest.a >> 8);
    const auto channel = [&](unsigned d, unsigned s) {
        if (!ad || as == 255)
            return s;
        const auto inverse = 255 - as;
        return (((ad * inverse * d) >> 8) + ((((as << 8) + inverse * (255 - ad)) * s) >> 8)) >> 8;
    };
    return Pixel::rgba8(channel(dest.r >> 8, ink.r >> 8), channel(dest.g >> 8, ink.g >> 8),
                        channel(dest.b >> 8, ink.b >> 8), std::max(ad, std::min(limit, ad + as)));
}
void publish(Raster &out, const Coverage &mask, Pixel ink, int samples, int density, std::uint64_t &budget) {
    const unsigned opacity = unsigned(std::clamp(density, 0, 100)) * 65535u / 100;
    const unsigned alpha = ((65535u * ((opacity * ink.a) >> 16)) >> 16) >> 8;
    spend(budget, mask.tiles.size() * 1024);
    for (const auto &[key, tile] : mask.tiles)
        for (int i = 0; i < 32 * 32; ++i) {
            const auto bits = tile[i];
            if (!bits)
                continue;
            const int x = key.second * 32 + i % 32, y = key.first * 32 + i / 32;
            const unsigned coverage = std::popcount(bits) * 255 / (samples * samples);
            const unsigned as = coverage == 255 ? alpha : (alpha * coverage) >> 8;
            if (as)
                out.set_pixel(x, y, combine(out.pixel(x, y), ink, as, alpha));
        }
}
void composite_plane(Raster &out, const Raster &plane, std::uint64_t &budget) {
    for (const auto &[coord, bytes] : plane.tiles) {
        const auto x0 = coord.x * Raster::tile_side, y0 = coord.y * Raster::tile_side;
        const auto xmax = std::min(out.width, x0 + Raster::tile_side),
                   ymax = std::min(out.height, y0 + Raster::tile_side);
        spend(budget, std::uint64_t(xmax - x0) * (ymax - y0));
        for (int y = y0; y < ymax; ++y)
            for (int x = x0; x < xmax; ++x) {
                const auto source = plane.pixel(x, y);
                if (source.a)
                    out.set_pixel(x, y, combine(out.pixel(x, y), source, source.a >> 8, 255));
            }
    }
}
} // namespace
void render_vectors(Raster &out, const VectorDrawing &drawing, const Projection &projection, int samples,
                    int density, bool direction, std::uint64_t &budget) {
    if (!direction && drawing.effects)
        throw std::invalid_argument("WCEL face effects require the extended vector renderer");
    auto shapes = direction ? std::vector<Shape>{} : face_shapes(drawing, projection, samples, budget);
    std::vector<const Edge *> records;
    for (const auto &edge : drawing.edges)
        if (!(edge.flags & 0x413u) && edge.ink.a)
            records.push_back(&edge);
    const auto black = Pixel::rgba8(0, 0, 0);
    std::stable_sort(records.begin(), records.end(), [&](auto a, auto b) {
        if ((a->ink == black) != (b->ink == black))
            return b->ink == black;
        return a->id < b->id;
    });
    for (auto &shape : stroke_shapes(records, projection, samples))
        shapes.push_back(std::move(shape));
    int right = 0, bottom = 0;
    for (const auto &shape : shapes)
        for (const auto &contour : shape.contours)
            for (auto pt : contour) {
                right = std::max(right, pt.x / samples);
                bottom = std::max(bottom, pt.y / samples);
            }
    auto clip = projection;
    clip.width = std::min(right, clip.width);
    clip.height = std::min(bottom, clip.height);
    std::optional<Raster> plane;
    if (!out.tiles.empty())
        plane.emplace(out.width, out.height, 32, out.resolution);
    auto &destination = plane ? *plane : out;
    for (std::size_t i = 0; i < shapes.size();) {
        const auto color = shapes[i].color;
        Coverage mask;
        do {
            fill(shapes[i++].contours, clip, samples, mask, budget);
        } while (i < shapes.size() && shapes[i].color == color);
        publish(destination, mask, color, samples, density, budget);
    }
    if (plane)
        composite_plane(out, *plane, budget);
}
} // namespace retas::svd_detail
