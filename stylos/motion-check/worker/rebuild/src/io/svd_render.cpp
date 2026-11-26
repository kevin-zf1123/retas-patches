#include "svd_render.hpp"
#include <algorithm>
#include <cmath>

namespace retas::svd_detail {
int sample_coordinate(double value) {
    if (!std::isfinite(value) || std::abs(value) > 16000000)
        throw std::length_error("WCEL projected coordinate exceeds supported extent");
    return int(value);
}
void spend(std::uint64_t &remaining, std::uint64_t cost) {
    if (cost > remaining)
        throw std::length_error("WCEL rasterization exceeds work limit");
    remaining -= cost;
}
Projection project(const Document &doc, const SvdImportSettings &settings) {
    const auto &g = doc.geometry;
    const auto &m = g.margins;
    const auto &tv = doc.tv_margins;
    const auto &o = doc.origin;
    const int percent = sample_coordinate(g.percent);
    const double ratio = 100 / g.percent;
    const double frame_scale = (percent == 100 ? 1 : ratio) * std::clamp(percent, 5, 200) / 100.;
    const double safe_scale = ratio * std::clamp(percent, 5, 200) / 100.;
    Projection p;
    p.outer = {o.x, o.y, o.x + g.size.x * inches_per_cm * frame_scale,
               o.y + g.size.y * inches_per_cm * frame_scale};
    p.safe = {o.x + tv[0] * inches_per_cm * safe_scale, o.y + tv[1] * inches_per_cm * safe_scale,
              p.outer.right - tv[2] * inches_per_cm * safe_scale,
              p.outer.bottom - tv[3] * inches_per_cm * safe_scale};
    Box canvas{m[0] * inches_per_cm, m[1] * inches_per_cm,
               ((m[2] + m[0]) + g.size.x * g.scale.x * ratio) * inches_per_cm - m[2] * inches_per_cm,
               ((m[3] + m[1]) + g.size.y * g.scale.y * ratio) * inches_per_cm - m[3] * inches_per_cm};
    const int frame_width =
        sample_coordinate(p.outer.right * settings.dpi) - sample_coordinate(o.x * settings.dpi);
    const int frame_height =
        sample_coordinate(p.outer.bottom * settings.dpi) - sample_coordinate(o.y * settings.dpi);
    const int width = sample_coordinate(frame_width * g.scale.x * ratio);
    const int height = sample_coordinate(frame_height * g.scale.y * ratio);
    p.dpi = width / (canvas.right * 72 - canvas.left * 72) * 72;
    if (!(p.dpi > 0) || !std::isfinite(p.dpi))
        throw std::invalid_argument("Invalid WCEL rendering DPI");
    const int left = sample_coordinate(canvas.left * p.dpi), top = sample_coordinate(canvas.top * p.dpi);
    if (settings.clip) {
        p.x = left;
        p.y = top;
        p.width = width;
        p.height = height;
    } else {
        p.width = sample_coordinate(canvas.right * p.dpi) - left +
                  sample_coordinate(m[0] * p.dpi * inches_per_cm) +
                  sample_coordinate(m[2] * p.dpi * inches_per_cm);
        p.height = sample_coordinate(canvas.bottom * p.dpi) - top +
                   sample_coordinate(m[1] * p.dpi * inches_per_cm) +
                   sample_coordinate(m[3] * p.dpi * inches_per_cm);
    }
    return p;
}
} // namespace retas::svd_detail

namespace retas {
Raster decode_svd(std::span<const std::uint8_t> bytes, const SvdImportSettings &settings) {
    using namespace svd_detail;
    if (bytes.size() > 128ull * 1024 * 1024)
        throw std::length_error("WCEL input exceeds 128 MiB");
    auto document = read_document(bytes);
    const auto size = image_size(document.geometry, settings);
    Raster output(size[0], size[1], 32, settings.dpi);
    constexpr std::uint64_t memory_limit = 512ull * 1024 * 1024;
    const auto tiles = std::uint64_t((size[0] + 255) / 256) * ((size[1] + 255) / 256);
    const bool direction = settings.direction && !document.direction_database.empty();
    const auto metadata = document.frames.capacity() * sizeof(DrawFrame);
    // Include sparse tile padding and map overhead, the borrowed source, and
    // the coverage tiles used for one ink color. The two DBs render serially.
    const auto fixed = bytes.size() + metadata + tiles * (output.tile_bytes() + 128) * (direction ? 2 : 1) +
                       20ull * 1024 * 1024;
    if (fixed > memory_limit)
        throw std::length_error("WCEL image exceeds memory limit");
    const auto workspace = memory_limit - fixed;
    auto projection = project(document, settings);
    // FPI Read copies only the intersection with the caller's destination.
    projection.width = std::clamp(projection.width, 0, output.width);
    projection.height = std::clamp(projection.height, 0, output.height);
    std::uint64_t budget = 128ull * 1024 * 1024;
    const int samples = settings.antialias ? 4 : 1;
    render_vectors(output, read_drawing(document.main_database, workspace), projection, samples, 100, false,
                   budget);
    if (direction) {
        render_vectors(output, read_drawing(document.direction_database, workspace), projection, samples,
                       document.direction_density, true, budget);
    }
    if (settings.frame)
        render_frames(output, document, projection, budget);
    return output;
}
} // namespace retas
