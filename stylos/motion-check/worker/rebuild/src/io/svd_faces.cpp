#include "svd_path.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace retas::svd_detail {
// Enumerate directed sides in angular order at each node. The unbounded
// face winds in the opposite direction from the bounded cells.
std::vector<Shape> face_shapes(const VectorDrawing &drawing, const Projection &p, int samples,
                               std::uint64_t &budget) {
    const auto &edges = drawing.edges;
    if (std::ranges::none_of(edges, [](const Edge &e) { return e.face.a != 0; }))
        return {};
    using Key = std::pair<double, double>;
    std::map<Key, std::vector<std::size_t>> outgoing;
    for (std::size_t i = 0; i < edges.size(); ++i)
        outgoing[{edges[i].start.x, edges[i].start.y}].push_back(i);
    const auto angle = [&](std::size_t i) {
        return std::atan2(edges[i].end.y - edges[i].start.y, edges[i].end.x - edges[i].start.x);
    };
    std::vector<std::size_t> next(edges.size());
    for (auto &[key, list] : outgoing) {
        std::stable_sort(list.begin(), list.end(), [&](auto a, auto b) { return angle(a) < angle(b); });
        for (std::size_t i = 0; i < list.size(); ++i)
            next[list[i] ^ 1] = list[(i + 1) % list.size()];
    }
    struct Ring {
        Pixel color;
        Polygon contour;
        double area;
        std::vector<Point> raw;
    };
    std::vector<Ring> rings;
    std::vector<bool> seen(edges.size());
    for (std::size_t first = 0; first < edges.size(); ++first) {
        if (seen[first])
            continue;
        std::vector<std::size_t> cycle;
        double area = 0;
        for (auto i = first; !seen[i]; i = next[i]) {
            seen[i] = true;
            cycle.push_back(i);
            const auto &e = edges[i];
            area += e.start.x * e.end.y - e.start.y * e.end.x;
        }
        if (area >= 0 || cycle.size() < 3)
            continue;
        Ring ring{{}, {}, -area, {}};
        Polygon controls;
        const Edge *owner = &edges[cycle.front()];
        for (std::size_t k = 0; k < cycle.size(); ++k) {
            const auto &e = edges[cycle[k]], &before = edges[cycle[(k + cycle.size() - 1) % cycle.size()]];
            if (e.face_order > owner->face_order)
                owner = &e;
            const double ax = e.start.x - before.start.x, ay = e.start.y - before.start.y;
            const double bx = e.end.x - e.start.x, by = e.end.y - e.start.y;
            const bool sharp = (e.flags & 0x20) || (before.flags & 0x80) || ((e.flags ^ before.flags) & 4) ||
                               ((e.flags & 4) && (e.radius != before.radius || e.effect != before.effect)) ||
                               (ax * bx + ay * by < .5 * std::hypot(ax, ay) * std::hypot(bx, by)) ||
                               outgoing.at({e.start.x, e.start.y}).size() != 2;
            controls.push_back(project_point(e.start, p, samples, sharp));
            ring.raw.push_back(e.start);
        }
        ring.color = owner->face;
        ring.contour = smooth(controls);
        rings.push_back(std::move(ring));
    }
    std::vector<Shape> shapes;
    for (auto &ring : rings)
        shapes.push_back({ring.color, {std::move(ring.contour)}});
    // Determine the nearest enclosing boundary independently of draw order.
    for (std::size_t i = 0; i < rings.size(); ++i) {
        const Point sample = rings[i].raw.front();
        auto parent = rings.size();
        spend(budget, rings.size());
        for (std::size_t j = 0; j < rings.size(); ++j) {
            if (rings[j].area <= rings[i].area ||
                (parent != rings.size() && rings[j].area >= rings[parent].area))
                continue;
            const auto &border = rings[j].raw;
            spend(budget, border.size());
            bool inside = false, on_edge = false;
            for (std::size_t k = 0; k < border.size(); ++k) {
                const Point a = border[k], b = border[(k + 1) % border.size()];
                if (sample.x == a.x && sample.y == a.y) {
                    on_edge = true;
                    break;
                }
                if ((a.y > sample.y) != (b.y > sample.y) &&
                    sample.x < (b.x - a.x) * (sample.y - a.y) / (b.y - a.y) + a.x)
                    inside = !inside;
            }
            if (inside && !on_edge)
                parent = j;
        }
        if (parent != rings.size())
            shapes[parent].contours.push_back(shapes[i].contours.front());
    }
    std::erase_if(shapes, [](const Shape &s) { return !s.color.a; });
    return shapes;
}
} // namespace retas::svd_detail
