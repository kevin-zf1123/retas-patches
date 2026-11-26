#include "svd_path.hpp"
#include <algorithm>
#include <cmath>
#include <deque>
#include <numbers>
#include <optional>
#include <tuple>

namespace retas::svd_detail {
Vertex project_point(Point point, const Projection &p, int samples, bool corner) {
    return {sample_coordinate((sample_coordinate(point.x) / 4800. * p.dpi - p.x) * samples),
            sample_coordinate((sample_coordinate(point.y) / 4800. * p.dpi - p.y) * samples), corner};
}
Polygon smooth(const Polygon &input) {
    Polygon output;
    const auto append = [&](Vertex v) {
        if (output.empty() || output.back().x != v.x || output.back().y != v.y)
            output.push_back(v);
    };
    for (std::size_t i = 0; i < input.size(); ++i) {
        const auto b = input[i];
        if (b.guide)
            continue;
        if (b.corner) {
            append(b);
            continue;
        }
        auto a = input[(i + input.size() - 1) % input.size()], c = input[(i + 1) % input.size()];
        const bool reverse = std::tie(a.y, a.x) > std::tie(c.y, c.x);
        if (reverse)
            std::swap(a, c);
        const double length = std::hypot(a.x - b.x, a.y - b.y) + std::hypot(c.x - b.x, c.y - b.y);
        const int steps = std::clamp(int(length / 32), 2, 16) & ~1;
        if (a.corner || a.guide) {
            a.x += a.x - b.x;
            a.y += a.y - b.y;
        }
        if (c.corner || c.guide) {
            c.x += c.x - b.x;
            c.y += c.y - b.y;
        }
        for (int j = 0; j <= steps; ++j) {
            const double t = double(reverse ? steps - j : j) / steps;
            const double wc = t * t * .5, wa = wc - t + .5, wb = t - t * t + .5;
            append({sample_coordinate(wa * a.x + wb * b.x + wc * c.x),
                    sample_coordinate(wa * a.y + wb * b.y + wc * c.y), false});
        }
    }
    return output;
}
namespace {
double rotation(Point a, Point b) {
    double angle = std::atan2(a.y - b.y, b.x - a.x);
    return angle < 0 ? angle + 2 * std::numbers::pi : angle;
}
Point offset(Point p, double angle, double length) {
    return {p.x + std::cos(angle) * length, p.y - std::sin(angle) * length};
}
std::array<Point, 2> cap_controls(Point a, Point e, Point b, Point f) {
    constexpr auto pi = std::numbers::pi;
    const double base = rotation(e, f) - pi / 2;
    double start = std::remainder(rotation(a, e) - base, 2 * pi);
    double end = std::remainder(rotation(b, f) - base, 2 * pi);
    if (start < 0 || end > 0) {
        a = offset(e, base, -16);
        b = offset(f, base, -16);
    } else if (start > pi / 6 || end < -pi / 6) {
        a = offset(e, base + std::min(start, pi / 6), -16);
        b = offset(f, base + std::max(end, -pi / 6), -16);
    }
    const double radius = std::hypot(e.x - f.x, e.y - f.y) * .5;
    const auto outside = offset(e, base, radius);
    const Point axis{f.x - e.x, f.y - e.y};
    const auto cross = [](Point u, Point v) { return u.x * v.y - u.y * v.x; };
    const auto intersect = [&](Point inner, Point tip) -> std::optional<Point> {
        const Point d{tip.x - inner.x, tip.y - inner.y};
        const double determinant = cross(d, axis);
        if (std::abs(determinant) < 1.e-8)
            return {};
        const double t = cross({outside.x - tip.x, outside.y - tip.y}, axis) / determinant;
        return Point{tip.x + t * d.x, tip.y + t * d.y};
    };
    auto c1 = intersect(a, e), c2 = intersect(b, f);
    if (c1 && c2)
        return {*c1, *c2};
    return {e, f};
}
Polygon outline(const Edge &edge, const Projection &p, int samples) {
    const Point a{float(edge.start.x), float(edge.start.y)}, b{float(edge.end.x), float(edge.end.y)};
    const double distance = std::hypot(b.x - a.x, b.y - a.y);
    if (!distance)
        return {};
    const double ra = std::max(2.f, edge.radius * edge.start_scale);
    const double rb = std::max(2.f, edge.radius * edge.end_scale);
    const bool short_segment = std::max(ra, rb) > distance;
    const double forward = rotation(a, b) - std::numbers::pi / 2;
    const double backward =
        short_segment ? forward + std::numbers::pi : rotation(b, a) - std::numbers::pi / 2;
    const auto side = [&](Point center, double direction, double radius) {
        auto pt = offset(center, direction, radius);
        if (!short_segment) {
            pt.x = std::trunc(pt.x);
            pt.y = std::trunc(pt.y);
        }
        return pt;
    };
    const auto ap = side(a, forward, ra), bp = side(b, forward, rb);
    const auto am = side(a, backward, ra), bm = side(b, backward, rb);
    Polygon controls;
    const auto add = [&](Point v, bool corner) { controls.push_back(project_point(v, p, samples, corner)); };
    add(ap, true);
    add(bp, true);
    if (!(edge.flags & 0x100))
        for (auto v : cap_controls(ap, bp, am, bm))
            add(v, false);
    add(bm, true);
    add(am, true);
    if (!(edge.flags & 0x40))
        for (auto v : cap_controls(bm, am, bp, ap))
            add(v, false);
    return smooth(controls);
}
struct Knot {
    Point point;
    double radius;
    bool sharp, flat;
    bool guide{};
};
struct StrokeHead {
    std::vector<Point> left, right;
    Polygon contour;
    bool dot{};
};
StrokeHead make_head(std::span<const Knot> knots, const Projection &p, int samples, bool cap_begin,
                     bool cap_end, bool first_head = false, bool last_head = false) {
    const auto n = knots.size();
    StrokeHead head;
    head.left.resize(n);
    head.right.resize(n);
    const auto side = [&](std::size_t i, bool reverse) {
        const auto prev = reverse ? i + 1 : i - 1, next = reverse ? i - 1 : i + 1;
        const auto pos = knots[i].point;
        const double out = next < n ? rotation(pos, knots[next].point) : rotation(knots[prev].point, pos);
        double angle = out, radius = knots[i].radius;
        if (prev < n && next < n) {
            const double half =
                std::remainder(rotation(knots[prev].point, pos) - out, 2 * std::numbers::pi) * .5;
            angle += half;
            radius = std::min(2 * radius, radius / std::max(.0001, std::abs(std::cos(half))));
        }
        auto v = offset(pos, angle - std::numbers::pi / 2, radius);
        return Point{std::trunc(v.x), std::trunc(v.y)};
    };
    double max_radius = 0, extent = 0;
    for (std::size_t i = 0; i < n; ++i) {
        head.left[i] = side(i, false);
        head.right[i] = side(i, true);
        max_radius = std::max(max_radius, knots[i].radius);
        extent = std::max(extent, std::hypot(knots[i].point.x - knots.front().point.x,
                                             knots[i].point.y - knots.front().point.y));
    }
    if (max_radius > extent) {
        Edge dot;
        dot.start = knots.front().point;
        dot.end = knots.back().point;
        dot.radius = 1;
        dot.start_scale = float(knots.front().radius);
        dot.end_scale = float(knots.back().radius);
        dot.flags = (knots.front().flat ? 0x40u : 0) | (knots.back().flat ? 0x100u : 0);
        head.contour = outline(dot, p, samples);
        head.dot = true;
        return head;
    }
    Polygon controls;
    const auto add = [&](Point v, bool corner, bool guide = false) {
        auto pt = project_point(v, p, samples, corner);
        pt.guide = guide;
        controls.push_back(pt);
    };
    const auto guide = [&](std::size_t i) {
        return knots[i].guide && (i != 0 || first_head) && (i + 1 != n || last_head);
    };
    for (std::size_t i = 0; i < n; ++i)
        add(head.left[i], i == 0 || i + 1 == n || knots[i].sharp, guide(i));
    if (cap_end && !knots.back().flat)
        for (auto pt : cap_controls(head.left[n - 2], head.left.back(), head.right[n - 2], head.right.back()))
            add(pt, false);
    for (std::size_t i = n; i-- > 0;)
        add(head.right[i], i == 0 || i + 1 == n || knots[i].sharp, guide(i));
    if (cap_begin && !knots.front().flat)
        for (auto pt : cap_controls(head.right[1], head.right.front(), head.left[1], head.left.front()))
            add(pt, false);
    head.contour = smooth(controls);
    return head;
}
std::optional<Point> intersect(Point a, Point b, Point c, Point d, bool segment = false) {
    const Point ab{b.x - a.x, b.y - a.y}, cd{d.x - c.x, d.y - c.y}, ac{c.x - a.x, c.y - a.y};
    const auto cross = [](Point u, Point v) { return u.x * v.y - u.y * v.x; };
    const auto det = cross(ab, cd);
    if (std::abs(det) < 1.e-8)
        return {};
    const double t = cross(ac, cd) / det, u = cross(ac, ab) / det;
    if (segment && (t < -1.e-7 || u < -1.e-7 || t > 1 + 1.e-7 || u > 1 + 1.e-7))
        return {};
    return Point{std::trunc(a.x + t * ab.x), std::trunc(a.y + t * ab.y)};
}
Polygon sharp_join(const StrokeHead &prev, const StrokeHead &next, double radius, const Projection &p,
                   int samples) {
    if (prev.dot || next.dot)
        return {};
    const auto l0 = prev.left.back(), r0 = prev.right.back(), l1 = next.left.front(), r1 = next.right.front();
    const auto center = intersect(r0, l0, r1, l1);
    const double turn = std::remainder(rotation(r1, l1) - rotation(r0, l0), 2 * std::numbers::pi);
    if (!center || turn == 0)
        return {};
    const auto a = turn > 0 ? l0 : r1, b = turn > 0 ? l1 : r0;
    const auto ta = turn > 0 ? prev.left[prev.left.size() - 2] : next.right[1];
    const auto tb = turn > 0 ? next.left[1] : prev.right[prev.right.size() - 2];
    Polygon controls;
    const auto add = [&](Point pt, bool corner) {
        controls.push_back(project_point(pt, p, samples, corner));
    };
    for (auto pt : turn > 0 ? std::array{l1, r0, r1, l0} : std::array{r0, l1, l0, r1})
        add(pt, true);
    if (const auto miter = intersect(a, ta, b, tb)) {
        if (std::hypot(miter->x - center->x, miter->y - center->y) <= radius)
            add(*miter, false);
        else if (const auto h = intersect(a, b, *center, *miter, true)) {
            const auto delta = radius - std::hypot(h->x - center->x, h->y - center->y);
            auto shift = offset({}, rotation(*center, *miter), delta);
            shift.x = std::trunc(shift.x);
            shift.y = std::trunc(shift.y);
            const Point ca{a.x + shift.x, a.y + shift.y}, cb{b.x + shift.x, b.y + shift.y};
            const auto q0 = intersect(ca, cb, a, *miter, true), q1 = intersect(ca, cb, b, *miter, true);
            if (q0 && q1) {
                add(*q0, false);
                add(*q1, false);
            }
        }
    }
    return smooth(controls);
}
// A closed smooth centerline has no cap. Divide the quadratic spline at
// its seam and whenever its tangent turns more than ninety degrees from
// the start of the current run. The three cut controls retain float precision.
std::vector<Polygon> smooth_loop(std::span<const Knot> knots, const Projection &p, int samples) {
    const auto n = knots.size();
    std::vector<std::vector<Knot>> sections(1);
    unsigned run = 0;
    double baseline = 0;
    for (std::size_t i = 0; i <= n; ++i) {
        const auto &a = knots[(i + n - 1) % n];
        const auto &b = knots[i % n];
        const auto &c = knots[(i + 1) % n];
        const double angle = rotation(a.point, b.point);
        if (run == 1)
            baseline = angle;
        if (i != 0 && i != n &&
            (run <= 1 ||
             std::abs(std::remainder(angle - baseline, 2 * std::numbers::pi)) <= std::numbers::pi / 2)) {
            sections.back().push_back(b);
            ++run;
            continue;
        }
        const auto part = [](float a, float b, float c) {
            const float mid = (a + c) * .5f, center = mid + (b - mid) * 3.f * .25f;
            return std::array{(a + b) * .5f, a + (b - a) * 3.f * .25f, center, c + (b - c) * 3.f * .25f,
                              (c + b) * .5f};
        };
        const auto x = part(a.point.x, b.point.x, c.point.x), y = part(a.point.y, b.point.y, c.point.y);
        auto radius = part(a.radius, b.radius, c.radius);
        radius[2] = float(b.radius);
        const auto append = [&](int j) {
            sections.back().push_back({{x[j], y[j]}, radius[j], false, false, j % 2 == 0});
        };
        if (i) {
            for (int j = 0; j < 3; ++j)
                append(j);
            if (i != n)
                sections.emplace_back();
        }
        if (i != n)
            for (int j = 2; j < 5; ++j)
                append(j);
        run = 0;
    }
    std::vector<Polygon> contours;
    for (std::size_t i = 0; i < sections.size(); ++i) {
        auto h = make_head(sections[i], p, samples, false, false, i == 0, i + 1 == sections.size());
        contours.push_back(std::move(h.contour));
    }
    return contours;
}
std::vector<Polygon> chain_outline(const std::vector<Edge> &chain, const Projection &p, int samples) {
    if (chain.size() == 1)
        return {outline(chain.front(), p, samples)};
    std::vector<Knot> knots;
    for (const auto &edge : chain) {
        const double r = std::max(2.f, edge.radius * edge.start_scale);
        if (knots.empty())
            knots.push_back({{float(edge.start.x), float(edge.start.y)}, r, true, bool(edge.flags & 0x40)});
        else {
            knots.back().radius = std::max(knots.back().radius, r);
            knots.back().sharp |= bool(edge.flags & 0x20);
        }
        knots.push_back({{float(edge.end.x), float(edge.end.y)},
                         std::max(2.f, edge.radius * edge.end_scale),
                         bool(edge.flags & 0x80),
                         bool(edge.flags & 0x100)});
    }
    const bool closed =
        knots.front().point.x == knots.back().point.x && knots.front().point.y == knots.back().point.y;
    if (closed) {
        knots.pop_back();
        knots.front().sharp = bool(chain.front().flags & 0x20) || bool(chain.back().flags & 0x80);
        const auto n = knots.size();
        std::size_t cut_at = n;
        for (std::size_t i = 0; i < n; ++i) {
            const auto pos = knots[i].point;
            const double angle = std::remainder(rotation(pos, knots[(i + 1) % n].point) -
                                                    rotation(knots[(i + n - 1) % n].point, pos),
                                                2 * std::numbers::pi);
            if (std::abs(angle) > std::numbers::pi / 3) {
                cut_at = i;
                break;
            }
        }
        if (cut_at == n)
            return smooth_loop(knots, p, samples);
        std::rotate(knots.begin(), knots.begin() + cut_at, knots.end());
        knots.push_back(knots.front());
    }
    knots.back().sharp = true;
    std::vector<Polygon> result;
    std::optional<StrokeHead> first, previous;
    std::size_t begin = 0;
    for (std::size_t i = 1; i < knots.size(); ++i) {
        const bool last = i + 1 == knots.size();
        if (!last) {
            const double turn = std::remainder(rotation(knots[i].point, knots[i + 1].point) -
                                                   rotation(knots[i - 1].point, knots[i].point),
                                               2 * std::numbers::pi);
            if (std::abs(turn) <= std::numbers::pi / 3)
                continue;
        }
        auto head = make_head(std::span(knots).subspan(begin, i - begin + 1), p, samples,
                              !closed && begin == 0, !closed && last);
        if (previous)
            result.push_back(sharp_join(*previous, head, knots[begin].radius, p, samples));
        result.push_back(std::move(head.contour));
        if (closed && !first)
            first = head;
        previous = std::move(head);
        begin = i;
    }
    if (closed && previous && first)
        result.push_back(sharp_join(*previous, *first, knots.front().radius, p, samples));
    return result;
}
} // namespace
std::vector<Shape> stroke_shapes(const std::vector<const Edge *> &records, const Projection &p, int samples) {
    using Key = std::tuple<double, double, Pixel, float>;
    std::map<Key, std::vector<std::size_t>> nodes;
    const auto key = [](Point pt, const Edge &e) { return Key{pt.x, pt.y, e.ink, e.radius}; };
    for (std::size_t i = 0; i < records.size(); ++i) {
        nodes[key(records[i]->start, *records[i])].push_back(i);
        nodes[key(records[i]->end, *records[i])].push_back(i);
    }
    std::vector<bool> used(records.size());
    std::vector<Shape> shapes;
    for (std::size_t root = 0; root < records.size(); ++root) {
        if (used[root])
            continue;
        std::deque<Edge> chain{*records[root]};
        used[root] = true;
        // The first forward record fixes the spline seam of a closed chain.
        for (bool front : {false, true}) {
            for (;;) {
                const auto &tip = front ? chain.front() : chain.back();
                const Point v = front ? tip.start : tip.end;
                const auto &adjacent = nodes.at(key(v, tip));
                if (adjacent.size() != 2)
                    break;
                const auto i = used[adjacent[0]] ? adjacent[1] : adjacent[0];
                if (used[i])
                    break;
                used[i] = true;
                auto e = *records[i];
                const auto contact = front ? e.end : e.start;
                if (contact.x != v.x || contact.y != v.y) {
                    std::swap(e.start, e.end);
                    std::swap(e.start_scale, e.end_scale);
                    e.flags = (e.flags & ~0x1e0u) | ((e.flags & 0x60u) << 2) | ((e.flags & 0x180u) >> 2);
                }
                if (front)
                    chain.push_front(e);
                else
                    chain.push_back(e);
            }
        }
        for (auto &contour : chain_outline({chain.begin(), chain.end()}, p, samples)) {
            Shape shape{records[root]->ink, {}};
            shape.contours.push_back(std::move(contour));
            shapes.push_back(std::move(shape));
        }
    }
    return shapes;
}
} // namespace retas::svd_detail
