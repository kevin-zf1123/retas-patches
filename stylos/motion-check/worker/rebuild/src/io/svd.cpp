#include "svd_document.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <cmath>

namespace retas::svd_detail {
namespace {
Reader file_header(std::span<const std::uint8_t> input) {
    Reader file(input);
    if (file.u32() != 0x5743454c || file.u16() > 0x1ff)
        throw FormatError(0, "Invalid WCEL header");
    return file;
}
double number(Reader &r) {
    const auto value = r.f64();
    if (!std::isfinite(value))
        throw FormatError(r.offset() - 8, "Nonfinite WCEL coordinate");
    return value;
}
Point point(Reader &r) { return {number(r), number(r)}; }
std::array<double, 4> box(Reader &r) { return {number(r), number(r), number(r), number(r)}; }
Geometry geometry(Reader &r) { return {point(r), point(r), number(r), r.i32(), box(r)}; }
std::span<const std::uint8_t> section_bytes(Reader &r) {
    auto s = r.section();
    return s.take(s.remaining());
}
// RCArchive stores most collections as a signed Win32 count. Bound before
// reserving even when an individual record has its own length envelope.
std::size_t count(Reader &r, std::size_t minimum) {
    const auto n = r.i32();
    r.require_count(n, minimum);
    return std::size_t(n);
}
void read_body(Reader body, Document &out) {
    out.geometry = geometry(body);
    out.origin = point(body);
    out.tv_margins = box(body);
    body.take(6 * sizeof(std::int32_t)); // saved editor state
    const auto frames = count(body, 4);
    body.i32(); // current frame selection
    out.frames.reserve(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        auto f = body.section();
        out.frames.push_back({std::clamp(f.i32(), 5, 200), number(f), point(f), point(f)});
    }
    const auto texts = count(body, 4);
    body.i32(); // current text selection
    // The Special view reads past editor annotations to reach its drawing DB.
    for (std::size_t i = 0; i < texts; ++i) {
        auto text = body.section();
        text.take(text.u16());
        (void)point(text);
        text.take(text.u16());
        text.take(16);
        (void)point(text);
        (void)point(text);
        text.u32();
    }
    const auto points = count(body, 16);
    for (std::size_t i = 0; i < points; ++i)
        (void)point(body);
    body.i32();
    out.main_database = section_bytes(body);
    if (body.remaining())
        body.section().take(8);
}
void read_view(Reader &file, int kind, Document &doc) {
    file.section().i32(); // saved view visibility
    if (kind == 4 || kind == 8)
        return;
    auto layer = file.section();
    if (kind == 16) {
        layer.i32(); // overridden by the direction import switch
        doc.direction_density = layer.i32();
        doc.direction_database = section_bytes(layer);
    } else {
        for (int slot : {4, 3, 2, 0, 1}) {
            layer.take(8);
            if (slot != 1)
                (void)layer.section();
        }
    }
}
} // namespace
Document read_document(std::span<const std::uint8_t> input) {
    auto file = file_header(input);
    const auto sections = file.u16();
    Document out;
    unsigned seen{};
    for (unsigned i = 0; i < sections; ++i) {
        const auto kind = file.u16();
        if (kind != 1 && kind != 2 && kind != 4 && kind != 8 && kind != 16 && kind != 32)
            throw FormatError(file.offset() - 2, "Unknown WCEL section");
        if (seen & kind)
            throw FormatError(file.offset() - 2, "Duplicate WCEL section");
        seen |= kind;
        switch (kind) {
        case 1:
            read_body(file.section(), out);
            break;
        case 2:
        case 4:
        case 8:
        case 16:
            read_view(file, kind, out);
            break;
        default:
            (void)file.section();
            break;
        }
    }
    if (!(seen & 1))
        throw FormatError(file.offset(), "WCEL has no drawing");
    if (file.remaining())
        file.take(8); // optional editor paper color
    return out;
}
std::array<int, 2> image_size(const Geometry &g, const SvdImportSettings &settings) {
    if (g.percent <= 0 || !std::isfinite(settings.dpi) || settings.dpi <= 0)
        throw std::invalid_argument("Invalid WCEL frame percentage or DPI");
    const auto size = [&](double extent, double scale, double before, double after) {
        const auto r = numeric_detail::binary64;
        const auto ratio = r(100. / g.percent);
        const double cm = settings.clip ? r(r(extent * scale) * ratio)
                                        : r(r(r(extent * ratio) * scale) + r(after + before));
        const double pixels = std::trunc(r(r(cm * settings.dpi) * inches_per_cm));
        if (!std::isfinite(pixels) || pixels < 1 || pixels > INT32_MAX)
            throw std::invalid_argument("Invalid WCEL output size");
        return int(pixels);
    };
    return {size(g.scale.x, g.size.x, g.margins[0], g.margins[2]),
            size(g.scale.y, g.size.y, g.margins[1], g.margins[3])};
}
} // namespace retas::svd_detail
namespace retas {
std::array<int, 2> svd_image_size(std::span<const std::uint8_t> bytes, const SvdImportSettings &settings) {
    auto file = svd_detail::file_header(bytes);
    const auto count = file.u16();
    for (int i = 0; i < count; ++i) {
        const auto kind = file.u16();
        if (kind != 1 && kind != 2 && kind != 4 && kind != 8 && kind != 16 && kind != 32)
            throw FormatError(file.offset() - 2, "Unknown WCEL section");
        auto s = file.section();
        if (kind == 1)
            return svd_detail::image_size(svd_detail::geometry(s), settings);
        if (kind == 2 || kind == 16)
            (void)file.section();
    }
    throw FormatError(file.offset(), "WCEL has no image geometry");
}
} // namespace retas
