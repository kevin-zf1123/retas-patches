#include "retas/filters.hpp"
#include "filter_tool_kernels.hpp"
#include "filter_lut.hpp"
#include "retas/numeric.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <stdexcept>
#include <unordered_set>

namespace retas {
namespace {

using Lut = std::vector<std::uint16_t>;

struct NativePixel {
    int r{}, g{}, b{}, a{};
};

int maximum(const Raster& image) { return image.depth == 32 ? 255 : 65535; }
int native_channel(std::uint16_t value, const Raster& image) {
    return image.depth == 32 ? value / 257 : value;
}
std::uint16_t stored_channel(int value, const Raster& image) {
    value = std::clamp(value, 0, maximum(image));
    return static_cast<std::uint16_t>(image.depth == 32 ? value * 257 : value);
}
NativePixel native_pixel(Pixel value, const Raster& image) {
    return {native_channel(value.r, image), native_channel(value.g, image),
            native_channel(value.b, image), native_channel(value.a, image)};
}
Pixel stored_pixel(NativePixel value, const Raster& image) {
    return {stored_channel(value.r, image), stored_channel(value.g, image),
            stored_channel(value.b, image), stored_channel(value.a, image)};
}

FilterResult rejected(std::string error) {
    return {FilterStatus::Rejected, 0, std::move(error)};
}
FilterResult cancelled() { return {FilterStatus::Cancelled, 0, {}}; }
FilterResult finish(Raster& destination, Raster&& staged, std::size_t changed) {
    if (!changed) return {};
    staged.prune_empty();
    destination = std::move(staged);
    return {FilterStatus::Applied, changed, {}};
}

void validate_raster(const Raster& image) {
    if (!image.can_edit())
        throw std::invalid_argument("滤镜仅支持 32 位或 64 位 RGBA 栅格平面");
    if (image.width <= 0 || image.height <= 0)
        throw std::invalid_argument("无效的栅格尺寸");
    const int columns = (image.width + Raster::tile_side - 1) / Raster::tile_side;
    const int rows = (image.height + Raster::tile_side - 1) / Raster::tile_side;
    for (const auto& [position, bytes] : image.tiles) {
        if (position.x < 0 || position.y < 0 || position.x >= columns || position.y >= rows)
            throw std::invalid_argument("栅格包含越界图块");
        if (!bytes || bytes->size() != image.tile_bytes())
            throw std::invalid_argument("栅格图块大小无效");
    }
}

std::vector<TileCoord> expanded_tiles(const Raster& image, int radius_x, int radius_y) {
    std::set<TileCoord> result;
    const int columns = (image.width + Raster::tile_side - 1) / Raster::tile_side;
    const int rows = (image.height + Raster::tile_side - 1) / Raster::tile_side;
    const int dx = (std::max(0, radius_x) + Raster::tile_side - 1) / Raster::tile_side;
    const int dy = (std::max(0, radius_y) + Raster::tile_side - 1) / Raster::tile_side;
    for (const auto& [position, unused] : image.tiles) {
        (void)unused;
        for (int y = std::max(0, position.y - dy); y <= std::min(rows - 1, position.y + dy); ++y)
            for (int x = std::max(0, position.x - dx); x <= std::min(columns - 1, position.x + dx); ++x)
                result.emplace(TileCoord{x, y});
    }
    return {result.begin(), result.end()};
}

template<class Function>
bool for_tile_pixels(const Raster& image, const std::vector<TileCoord>& tiles,
                     std::stop_token stop, Function&& function) {
    for (const auto tile : tiles) {
        if (stop.stop_requested()) return false;
        const int left = tile.x * Raster::tile_side;
        const int top = tile.y * Raster::tile_side;
        const int right = std::min(image.width, left + Raster::tile_side);
        const int bottom = std::min(image.height, top + Raster::tile_side);
        for (int y = top; y < bottom; ++y) {
            if ((y & 15) == 0 && stop.stop_requested()) return false;
            for (int x = left; x < right; ++x) function(x, y);
        }
    }
    return true;
}

std::vector<TileCoord> materialized_tiles(const Raster& image) {
    std::vector<TileCoord> result;
    result.reserve(image.tiles.size());
    for (const auto& [position, unused] : image.tiles) {
        (void)unused;
        result.push_back(position);
    }
    return result;
}

std::vector<TileCoord> all_tiles(const Raster& image) {
    std::vector<TileCoord> result;
    const int columns = (image.width + Raster::tile_side - 1) / Raster::tile_side;
    const int rows = (image.height + Raster::tile_side - 1) / Raster::tile_side;
    result.reserve(static_cast<std::size_t>(columns) * rows);
    for (int y = 0; y < rows; ++y)
        for (int x = 0; x < columns; ++x) result.push_back({x, y});
    return result;
}

template<class Function>
FilterResult apply_point_filter(Raster& image, std::stop_token stop, Function&& function) {
    const Raster source = image;
    Raster staged = source;
    std::size_t changed{};
    const auto tiles = materialized_tiles(source);
    const bool complete = for_tile_pixels(source, tiles, stop, [&](int x, int y) {
        const Pixel before = source.pixel(x, y);
        const Pixel after = function(before);
        if (before != after) {
            staged.set_pixel(x, y, after);
            ++changed;
        }
    });
    if (!complete) return cancelled();
    return finish(image, std::move(staged), changed);
}

FilterResult apply_coverage_lut(Raster& image, const Lut& table, std::stop_token stop) {
    const Raster source = image;
    Raster staged = source;
    std::size_t changed{};
    const auto tiles = table.front() ? all_tiles(source) : materialized_tiles(source);
    const bool complete = for_tile_pixels(source, tiles, stop, [&](int x, int y) {
        const Pixel before = source.pixel(x, y);
        Pixel after = before;
        after.a = stored_channel(table[static_cast<std::size_t>(native_channel(before.a, source))], source);
        if (before != after) { staged.set_pixel(x, y, after); ++changed; }
    });
    if (!complete) return cancelled();
    return finish(image, std::move(staged), changed);
}

int rounded_linear(double value) {
    const auto result = legacy_rt_double_to_int(value);
    if (!result) throw std::invalid_argument("Filter numeric conversion exceeds supported limits");
    return *result;
}

int rounded_short(double value) {
    // The DLL exports share an address, but these callers sign-extend AX.
    const auto low = static_cast<std::uint32_t>(rounded_linear(value)) & 0xffffu;
    return low < 0x8000u ? int(low) : int(low) - 0x10000;
}

std::uint16_t bezier_store(int value, int limit) {
    // Both workers cap only the upper end, then write AL/AX. Negative signed
    // values therefore wrap to the stored channel rather than saturating to 0.
    return static_cast<std::uint16_t>(static_cast<std::uint32_t>(std::min(value,limit)) &
                                      static_cast<std::uint32_t>(limit));
}

int rounded_spline(double value) {
    // sub_605EA0 uses +0.5 followed by __ftol2, independently of rtDouble2Int.
    return static_cast<int>(value + 0.5);
}

int linear_range(int value, int input_low, int input_high,
                 int output_low, int output_high) {
    if (value <= input_low) return output_low;
    if (value >= input_high) return output_high;
    const double slope = double(output_high - output_low) / (input_high - input_low);
    // The host first converts the line intercept with rtDouble2Int and then
    // converts each sampled value. This differs by one from rounding the
    // mathematically combined expression for some negative intercepts.
    const int intercept = rounded_linear(output_low - input_low * slope);
    return rounded_linear(value * slope + intercept);
}

Lut contrast_lut(const Raster& image, const ContrastSettings& settings) {
    const int limit = maximum(image);
    const int scale = image.depth == 32 ? 1 : 256;
    Lut table(static_cast<std::size_t>(limit) + 1);
    for (int value = 0; value <= limit; ++value) {
        int mapped = value;
        if (settings.contrast > 0) {
            const int inset = settings.contrast * scale;
            mapped = linear_range(mapped, inset, limit - inset, 0, limit);
        } else if (settings.contrast < 0) {
            const int inset = -settings.contrast * scale;
            mapped = linear_range(mapped, 0, limit, inset, limit - inset);
        }
        if (settings.brightness > 0) {
            const int lift = settings.brightness * scale;
            mapped = linear_range(mapped, 0, limit - lift, lift, limit);
        } else if (settings.brightness < 0) {
            const int drop = -settings.brightness * scale;
            mapped = linear_range(mapped, drop, limit, 0, limit - drop);
        }
        // The host word-LUT tail fill duplicates the low byte of its endpoint;
        // all observed endpoints end in 0xff, so the final 16-bit entry stays
        // 0xffff even for negative contrast/brightness settings.
        if (image.depth == 64 && value == limit) mapped = limit;
        table[static_cast<std::size_t>(value)] = static_cast<std::uint16_t>(std::clamp(mapped, 0, limit));
    }
    return table;
}

Lut level_lut(const Raster& image, const LevelChannel& channel) {
    using numeric_detail::binary64;
    const int limit = maximum(image);
    const int low = channel.low * (image.depth == 32 ? 1 : 257);
    const int high = channel.high * (image.depth == 32 ? 1 : 257);
    const int output_low = channel.output_low * (image.depth == 32 ? 1 : 257);
    const int output_high = channel.output_high * (image.depth == 32 ? 1 : 257);
    const auto slope=binary64(double(output_low-output_high)/(low-high));
    const auto reciprocal=image.depth==32?0x1.0101010101010p-8:0x1.0001000100010p-16;
    const auto exponent=binary64(1.0/channel.gamma);
    Lut table(static_cast<std::size_t>(limit) + 1);
    for (int value = 0; value <= limit; ++value) {
        double mapped;
        if (value == 0 || value < low) mapped = output_low;
        else if (value >= high) mapped = output_high;
        // 605E60 is anchored at high, including the nonzero low endpoint.
        else mapped = binary64(binary64(slope*(value-high))+output_high);
        // 6645A0 stores the linear result in an integer table before the
        // separate gamma pass reloads it. Keeping the fraction changes gamma.
        mapped = std::clamp(static_cast<int>(mapped), 0, limit);
        if (channel.gamma != 1.0 && mapped > 0.0)
            mapped = binary64(binary64(std::pow(binary64(mapped*reciprocal),exponent))*limit);
        table[static_cast<std::size_t>(value)] = static_cast<std::uint16_t>(
            std::clamp(static_cast<int>(mapped), 0, limit));
    }
    return table;
}

struct CurveSample { double x{}, y{}; };

int curve_coordinate(int value, int limit) {
    if (value <= 0) return 0;
    if (value >= 255) return limit == 255 ? 255 : 255 * 256;
    return limit == 255 ? value : value * 256;
}

CurveSample bezier_point(const std::vector<CurveSample>& controls, double t) {
    using numeric_detail::binary64;
    std::array<CurveSample,10> work;std::copy(controls.begin(),controls.end(),work.begin());
    const auto complement=binary64(1.0-t);
    for (std::size_t count = controls.size(); count > 1; --count)
        for (std::size_t i = 0; i + 1 < count; ++i) {
            work[i].x=binary64(binary64(complement*work[i].x)+binary64(t*work[i+1].x));
            work[i].y=binary64(binary64(complement*work[i].y)+binary64(t*work[i+1].y));
        }
    return work.front();
}

Lut bezier_curve_lut(const Raster& image, const ToneCurveRecord& record) {
    // 668B10's WORD branch copies controls into only four stack POINTs.
    // Its BYTE branch passes the records directly to the ten-point evaluator.
    if(image.depth==64&&record.points.size()>4)
        throw std::invalid_argument("WORD Bezier exceeds the native four-point buffer");
    const int limit = maximum(image);
    std::vector<CurveSample> controls;
    controls.reserve(record.points.size());
    const int scale = image.depth == 32 ? 1 : 257;
    for (const auto point : record.points)
        controls.push_back({double(point.x * scale), double(point.y * scale)});
    Lut table(static_cast<std::size_t>(limit) + 1);
    for (int value = 0; value <= limit; ++value)
        table[static_cast<std::size_t>(value)] = static_cast<std::uint16_t>(value);
    const int first_x = static_cast<int>(controls.front().x);
    const int first_y = static_cast<int>(controls.front().y);
    const int last_x = static_cast<int>(controls.back().x);
    const int last_y = static_cast<int>(controls.back().y);
    std::fill(table.begin(), table.begin() + first_x, static_cast<std::uint16_t>(first_y));
    const auto line_segment = [&](int end_x, int end_y) {
        const double slope = end_x == first_x ? 0.0 : double(end_y-first_y)/(end_x-first_x);
        const int intercept = rounded_short(first_y-first_x*slope);
        for(int x=first_x;x<end_x;++x)
            table[static_cast<std::size_t>(x)] = bezier_store(rounded_short(x*slope+intercept),limit);
    };
    if (controls.size() == 2) {
        line_segment(last_x,last_y);
    } else {
        const int subdivisions = image.depth == 32 ? 1024 : 65536;
        int previous_x = first_x;
        for (int i = 0; i < subdivisions; ++i) {
            const auto sample = bezier_point(controls, double(i) / subdivisions);
            const int x = std::clamp(std::max(previous_x, rounded_short(sample.x)), first_x, last_x);
            table[static_cast<std::size_t>(x)] = bezier_store(rounded_short(sample.y),limit);
            previous_x = x;
        }
    }
    std::fill(table.begin() + last_x, table.end(), static_cast<std::uint16_t>(last_y));
    return table;
}

Lut tone_curve_lut(const Raster& image, const ToneCurveRecord& record, int interpolation) {
    using numeric_detail::binary64;
    const int limit = maximum(image);
    if (interpolation == 1) return bezier_curve_lut(image, record);
    std::vector<CurveSample> controls;
    controls.reserve(record.points.size());
    for (const auto point : record.points)
        controls.push_back({double(curve_coordinate(point.x, limit)),
                            double(curve_coordinate(point.y, limit))});
    if (controls.size() == 2) {
        // sub_605EA0 samples `domain` points over abs(lastX-firstX)/domain;
        // the final endpoint itself is therefore not sampled. This preserves
        // the host's observable 255->254 and 65535->65279 default endpoints.
        Lut table(static_cast<std::size_t>(limit) + 1);
        const double step = std::abs(controls[1].x - controls[0].x) / (limit + 1.0);
        const auto slope=binary64((controls[1].y-controls[0].y)/(controls[1].x-controls[0].x));
        double x = controls[0].x;
        for (int index = 0; index <= limit; ++index, x += step) {
            const auto y=binary64(binary64(slope*binary64(x-controls[0].x))+controls[0].y);
            table[static_cast<std::size_t>(index)]=std::uint16_t(rounded_spline(y))&limit;
        }
        return table;
    }

    // Native scratch is uninitialized. Rounded local grids can skip an index;
    // zero is the rebuild's deterministic policy for those unwritten slots.
    Lut table(static_cast<std::size_t>(limit)+1,0);
    const auto step=binary64(std::abs(controls.back().x-controls.front().x)/(limit+1.0));
    const auto inverse=binary64(1.0/step);
    bool first=true;double origin{};CurveSample previous;
    for (std::size_t segment = 1; segment + 1 < controls.size(); ++segment) {
        const auto p0 = segment == 1
            ? CurveSample{2 * controls.front().x - controls[1].x,
                          2 * controls.front().y - controls[1].y}
            : controls[segment - 1];
        const auto p1 = controls[segment];
        const auto p2 = segment + 2 == controls.size()
            ? CurveSample{2 * controls.back().x - controls[controls.size() - 2].x,
                          2 * controls.back().y - controls[controls.size() - 2].y}
            : controls[segment + 1];
        for (int i = 0; i <= 32; ++i) {
            const double t = double(i) / 32.0;
            const auto a=binary64(binary64(binary64(1.0-t)*binary64(1.0-t))*0.5);
            const auto square=binary64(t*t);
            const auto b=binary64(binary64(t-square)+0.5),c=binary64(square*0.5);
            const auto weighted=[&](double v0,double v1,double v2){return binary64(binary64(binary64(c*v2)+binary64(b*v1))+binary64(a*v0));};
            const CurveSample current{weighted(p0.x,p1.x,p2.x),weighted(p0.y,p1.y,p2.y)};
            if(first){first=false;origin=current.x;previous=current;continue;}
            const auto left=std::min(previous.x,current.x),distance=binary64(std::abs(current.x-previous.x));
            // 605EA0 restarts the local grid for each sample pair. Later
            // visits to a rounded table index overwrite the earlier value.
            for(double offset=0;offset<=distance;offset=binary64(offset+step)){
                const auto x=binary64(left+offset);
                const auto index=rounded_spline(binary64(binary64(x-origin)*inverse));
                if(unsigned(index)>unsigned(limit))continue;
                auto y=previous.y;
                if(current.x!=previous.x){const auto slope=binary64(binary64(current.y-previous.y)/binary64(current.x-previous.x));
                    y=binary64(binary64(slope*binary64(x-previous.x))+previous.y);}
                table[std::size_t(index)]=std::uint16_t(rounded_spline(y))&limit;
            }
            previous=current;
        }
    }
    return table;
}

Pixel apply_rgb_luts(Pixel value, const Raster& image,
                     const Lut& red, const Lut& green, const Lut& blue) {
    auto native = native_pixel(value, image);
    native.r = red[static_cast<std::size_t>(native.r)];
    native.g = green[static_cast<std::size_t>(native.g)];
    native.b = blue[static_cast<std::size_t>(native.b)];
    return stored_pixel(native, image);
}

FilterResult apply_convolution(Raster& image, const std::array<int, 9>& kernel,
                               int divisor, std::stop_token stop) {
    const Raster source = image;
    Raster staged = source;
    std::size_t changed{};
    const auto tiles = expanded_tiles(source, 1, 1);
    const bool complete = for_tile_pixels(source, tiles, stop, [&](int x, int y) {
        std::array<std::int64_t, 4> sums{};
        int index{};
        for (int dy = -1; dy <= 1; ++dy) for (int dx = -1; dx <= 1; ++dx) {
            const auto sample = native_pixel(source.pixel(std::clamp(x + dx, 0, source.width - 1),
                                                          std::clamp(y + dy, 0, source.height - 1)), source);
            const int weight = kernel[static_cast<std::size_t>(index++)];
            sums[0] += std::int64_t(sample.r) * weight;
            sums[1] += std::int64_t(sample.g) * weight;
            sums[2] += std::int64_t(sample.b) * weight;
            sums[3] += std::int64_t(sample.a) * weight;
        }
        const int limit = maximum(source);
        NativePixel out;
        out.r = std::clamp(static_cast<int>(sums[0] / divisor), 0, limit);
        out.g = std::clamp(static_cast<int>(sums[1] / divisor), 0, limit);
        out.b = std::clamp(static_cast<int>(sums[2] / divisor), 0, limit);
        out.a = std::clamp(static_cast<int>(sums[3] / divisor), 0, limit);
        const Pixel after = stored_pixel(out, source);
        if (after != source.pixel(x, y)) {
            staged.set_pixel(x, y, after);
            ++changed;
        }
    });
    if (!complete) return cancelled();
    return finish(image, std::move(staged), changed);
}

FilterResult apply_gaussian(Raster& image,const GaussianBlurSettings& settings,
                            const FilterExecutionOptions& options,std::stop_token stop){
    const auto radius=settings.use_millimeters?settings.size_mm*image.resolution/25.4:double(settings.size_px);
    const auto result=detail::gaussian_region(image,options.coverage_only?4:1,radius,
        Rect{0,0,image.width,image.height},nullptr,stop);
    if(result.cancelled)return cancelled();
    if(!result.handled)return rejected(result.error);
    return {result.changed?FilterStatus::Applied:FilterStatus::Unchanged,result.changed_pixels,{}};
}

std::string validate_settings(FilterKind kind, const FilterSettings& settings) {
    if (!settings_match_filter(kind, settings)) return "滤镜参数类型与命令不匹配";
    if (const auto* value = std::get_if<ContrastSettings>(&settings)) {
        if (value->contrast < -127 || value->contrast > 127 ||
            value->brightness < -127 || value->brightness > 127)
            return "对比度和亮度必须在 -127 到 127 之间";
    } else if (const auto* value = std::get_if<LevelSettings>(&settings)) {
        for (const auto& channel : value->channels)
            if (channel.low < 0 || channel.low > 255 || channel.high < 0 || channel.high > 255 ||
                channel.high - channel.low < 2 || channel.output_low < 0 || channel.output_low > 255 ||
                channel.output_high < 0 || channel.output_high > 255 || !std::isfinite(channel.gamma) ||
                channel.gamma < 0.1 || channel.gamma > 9.99)
                return "色阶参数超出可用范围";
    } else if (const auto* value = std::get_if<ToneCurveSettings>(&settings)) {
        if (value->interpolation < 0 || value->interpolation > 1) return "未知的色调曲线插值方式";
        for (const auto& curve : value->curves) {
            const std::size_t maximum_points = value->interpolation ? 10 : 32;
            if (curve.points.size() < 2 || curve.points.size() > maximum_points)
                return "色调曲线控制点数量无效";
            int previous = -1;
            for (const auto point : curve.points) {
                if (point.x < 0 || point.x > 255 || point.y < 0 || point.y > 255 || point.x <= previous)
                    return "色调曲线控制点必须按输入值递增并位于 0 到 255";
                previous = point.x;
            }
        }
    } else if (const auto* value = std::get_if<UnsharpMaskSettings>(&settings)) {
        if (value->range < 1 || value->range > 100 || value->contain < 0 || value->contain > 255 ||
            value->strength < 1 || value->strength > 500)
            return "反遮罩锐化参数超出可用范围";
    } else if (const auto* value = std::get_if<GaussianBlurSettings>(&settings)) {
        if (!std::isfinite(value->size_mm) || value->size_mm < 0.0 || value->size_mm > 1000.0 ||
            value->size_px < 0 || value->size_px > 20000)
            return "高斯模糊半径超出可用范围";
    } else if (const auto* value = std::get_if<HsvSettings>(&settings)) {
        if (!std::isfinite(value->hue) || !std::isfinite(value->saturation) || !std::isfinite(value->brightness) ||
            value->hue < -180.0 || value->hue > 180.0 || value->saturation < -100.0 ||
            value->saturation > 100.0 || value->brightness < -100.0 || value->brightness > 100.0)
            return "HSV 参数超出可用范围";
    } else if (const auto* value = std::get_if<ColorBalanceSettings>(&settings)) {
        if (!std::isfinite(value->red) || !std::isfinite(value->green) || !std::isfinite(value->blue))
            return "色彩平衡参数必须是有限数值";
    } else if (const auto* value = std::get_if<DustSettings>(&settings)) {
        if (int(value->mode) < 0 || int(value->mode) > 2 || !std::isfinite(value->size_mm) ||
            value->size_mm < 0.0 || value->size_mm > 1000.0 || value->size_px < 0 || value->size_px > 20000)
            return "除尘参数超出可用范围";
    }
    return {};
}

bool admitted_raster_layer(std::uint32_t family) {
    switch (family) {
    case 0x10000100: case 0x10000400: case 0x11000800: case 0x11001000:
    case 0x18004000: case 0x10008000: case 0x12000200: case 0x14002000:
        return true;
    default:
        return false;
    }
}

bool primary_filter_layer(std::uint32_t family) {
    return family == 0x10000100 || family == 0x11000800 ||
           family == 0x11001000 || family == 0x18004000;
}

bool filter_executes_on_layer(FilterKind kind, std::uint32_t family) {
    const bool fixed = kind == FilterKind::SharpHard || kind == FilterKind::SharpSoft ||
                       kind == FilterKind::BlurHard || kind == FilterKind::BlurSoft;
    if (fixed) return primary_filter_layer(family) || family == 0x14002000;
    if (kind == FilterKind::Dust)
        return primary_filter_layer(family) || family == 0x10000400 ||
               family == 0x10008000 || family == 0x12000200 || family == 0x14002000;
    return primary_filter_layer(family);
}

} // namespace

void filter_detail::validate_filter_settings(FilterKind kind,const FilterSettings& settings) {
    if (const auto error=validate_settings(kind,settings);!error.empty())throw std::invalid_argument(error);
}
ToneCurveSettings filter_detail::freeze_tone_curve_settings(const ToneCurveSettings& settings) {
    const auto interpolation=settings.interpolation;
    if(interpolation<0||interpolation>1)throw std::invalid_argument("Invalid tone curve interpolation");
    std::array<std::array<ToneCurvePoint,32>,4> points;
    std::array<std::size_t,4> counts;
    for(std::size_t c=0;c<4;++c){
        counts[c]=settings.curves[c].points.size();
        if(counts[c]<2||counts[c]>(interpolation?10u:32u))throw std::invalid_argument("Invalid tone curve point count");
        std::copy(settings.curves[c].points.begin(),settings.curves[c].points.end(),points[c].begin());
    }
    // All caller-owned dynamic records are now in bounded stack storage.
    // Allocating the private vectors may reenter code that changes that caller.
    ToneCurveSettings result;result.interpolation=interpolation;
    for(std::size_t c=0;c<4;++c)result.curves[c].points.assign(points[c].begin(),points[c].begin()+counts[c]);
    validate_filter_settings(FilterKind::ToneCurve,result);return result;
}
filter_detail::ByteRgbTables filter_detail::byte_rgb_tables(FilterKind kind,const FilterSettings& settings) {
    validate_filter_settings(kind,settings);
    const Raster domain(1,1,32);
    ByteRgbTables result;
    if(kind==FilterKind::Contrast){
        const auto table=contrast_lut(domain,std::get<ContrastSettings>(settings));
        for(auto& channel:result)for(std::size_t i=0;i<256;++i)channel[i]=std::uint8_t(table[i]);
        return result;
    }
    std::array<Lut,4> tables;
    if(kind==FilterKind::Level){
        const auto& value=std::get<LevelSettings>(settings);
        for(std::size_t i=0;i<4;++i)tables[i]=level_lut(domain,value.channels[i]);
    }else if(kind==FilterKind::ToneCurve){
        const auto& value=std::get<ToneCurveSettings>(settings);
        for(std::size_t i=0;i<4;++i)tables[i]=tone_curve_lut(domain,value.curves[i],value.interpolation);
    }else throw std::invalid_argument("Adjust helper does not generate RGB tables");
    for(std::size_t c=0;c<3;++c)for(std::size_t i=0;i<256;++i)result[c][i]=std::uint8_t(tables[0][tables[c+1][i]]);
    return result;
}

std::optional<FilterKind> filter_kind_for_command(FilterCommand command) {
    switch (command) {
    case FilterCommand::ContrastApply: case FilterCommand::ContrastSetup: return FilterKind::Contrast;
    case FilterCommand::ToneCurveApply: case FilterCommand::ToneCurveSetup: return FilterKind::ToneCurve;
    case FilterCommand::LevelApply: case FilterCommand::LevelSetup: return FilterKind::Level;
    case FilterCommand::SharpHard: return FilterKind::SharpHard;
    case FilterCommand::SharpSoft: return FilterKind::SharpSoft;
    case FilterCommand::UnsharpMask: return FilterKind::UnsharpMask;
    case FilterCommand::BlurHard: return FilterKind::BlurHard;
    case FilterCommand::BlurSoft: return FilterKind::BlurSoft;
    case FilterCommand::GaussianBlur: return FilterKind::GaussianBlur;
    case FilterCommand::AutoContrast: return FilterKind::AutoContrast;
    case FilterCommand::HsvApply: case FilterCommand::HsvSetup: return FilterKind::Hsv;
    case FilterCommand::ColorBalanceApply: case FilterCommand::ColorBalanceSetup: return FilterKind::ColorBalance;
    case FilterCommand::Dust: return FilterKind::Dust;
    case FilterCommand::Reexecute: return std::nullopt;
    }
    return std::nullopt;
}

bool is_setup_command(FilterCommand command) {
    switch (command) {
    case FilterCommand::ContrastSetup: case FilterCommand::ToneCurveSetup:
    case FilterCommand::LevelSetup: case FilterCommand::HsvSetup:
    case FilterCommand::ColorBalanceSetup: return true;
    default: return false;
    }
}

bool is_interactive_command(FilterCommand command) {
    return is_setup_command(command) || command == FilterCommand::UnsharpMask ||
           command == FilterCommand::GaussianBlur || command == FilterCommand::Dust;
}

FilterRepeatAction canonical_repeat_action(FilterKind kind) {
    switch (kind) {
    case FilterKind::Contrast: return FilterRepeatAction::Contrast;
    case FilterKind::ToneCurve: return FilterRepeatAction::ToneCurve;
    case FilterKind::Level: return FilterRepeatAction::Level;
    case FilterKind::AutoContrast: return FilterRepeatAction::AutoContrast;
    case FilterKind::SharpHard: return FilterRepeatAction::SharpHard;
    case FilterKind::SharpSoft: return FilterRepeatAction::SharpSoft;
    case FilterKind::UnsharpMask: return FilterRepeatAction::UnsharpMask;
    case FilterKind::BlurHard: return FilterRepeatAction::BlurHard;
    case FilterKind::BlurSoft: return FilterRepeatAction::BlurSoft;
    case FilterKind::GaussianBlur: return FilterRepeatAction::GaussianBlur;
    case FilterKind::Hsv: return FilterRepeatAction::Hsv;
    case FilterKind::ColorBalance: return FilterRepeatAction::ColorBalance;
    case FilterKind::Dust: return FilterRepeatAction::Dust;
    case FilterKind::Invert: throw std::invalid_argument("Batch Invert has no ordinary repeat action");
    }
    return FilterRepeatAction::Contrast;
}

FilterSettings default_filter_settings(FilterKind kind) {
    switch (kind) {
    case FilterKind::Contrast: return ContrastSettings{};
    case FilterKind::ToneCurve: return ToneCurveSettings{};
    case FilterKind::Level: return LevelSettings{};
    case FilterKind::UnsharpMask: return UnsharpMaskSettings{};
    case FilterKind::GaussianBlur: return GaussianBlurSettings{};
    case FilterKind::Hsv: return HsvSettings{};
    case FilterKind::ColorBalance: return ColorBalanceSettings{};
    case FilterKind::Dust: return DustSettings{};
    default: return std::monostate{};
    }
}

bool settings_match_filter(FilterKind kind, const FilterSettings& settings) {
    switch (kind) {
    case FilterKind::Contrast: return std::holds_alternative<ContrastSettings>(settings);
    case FilterKind::ToneCurve: return std::holds_alternative<ToneCurveSettings>(settings);
    case FilterKind::Level: return std::holds_alternative<LevelSettings>(settings);
    case FilterKind::UnsharpMask: return std::holds_alternative<UnsharpMaskSettings>(settings);
    case FilterKind::GaussianBlur: return std::holds_alternative<GaussianBlurSettings>(settings);
    case FilterKind::Hsv: return std::holds_alternative<HsvSettings>(settings);
    case FilterKind::ColorBalance: return std::holds_alternative<ColorBalanceSettings>(settings);
    case FilterKind::Dust: return std::holds_alternative<DustSettings>(settings);
    default: return std::holds_alternative<std::monostate>(settings);
    }
}

FilterResult apply_filter(Raster& image, FilterKind kind, const FilterSettings& settings,
                          std::stop_token stop) {
    return apply_filter(image, kind, settings, FilterExecutionOptions{}, stop);
}

FilterResult apply_filter(Raster& image, FilterKind kind, const FilterSettings& settings,
                          const FilterExecutionOptions& options, std::stop_token stop) {
    if(kind==FilterKind::Dust)return filter_detail::apply_dust(image,settings,{},options,stop,{});
    if(kind==FilterKind::ColorBalance)return filter_detail::apply_color_balance(image,settings,{},stop,{});
    if(kind==FilterKind::GaussianBlur&&image.depth!=32&&image.depth!=64)return filter_detail::apply_low_gaussian(image,settings,{},stop);
    if(kind==FilterKind::UnsharpMask)return filter_detail::apply_unsharp(image,settings,{},stop,{});
    if(kind==FilterKind::Hsv)return filter_detail::apply_hsv(image,settings,{},stop,{});
    if(kind==FilterKind::AutoContrast)return filter_detail::apply_auto_contrast(image,settings,{},options,stop,{});
    if(filter_detail::fixed_convolution(kind)&&image.depth!=32&&image.depth!=64)return filter_detail::apply_low_convolution(image,kind,settings,{},stop,{});
    if(image.depth==24&&filter_detail::table_filter(kind))return filter_detail::apply_rgb24(image,kind,settings,{},options,stop,{});
    try {
        validate_raster(image);
        if (const auto error = validate_settings(kind, settings); !error.empty()) return rejected(error);
        if (stop.stop_requested()) return cancelled();
        switch (kind) {
        case FilterKind::Invert:return rejected("Batch Invert requires its Document worker");
        case FilterKind::Contrast: {
            const auto table = contrast_lut(image, std::get<ContrastSettings>(settings));
            if (options.coverage_only) return apply_coverage_lut(image, table, stop);
            return apply_point_filter(image, stop, [&](Pixel value) {
                if (!value.a) return value;
                return apply_rgb_luts(value, image, table, table, table);
            });
        }
        case FilterKind::Level: {
            const auto& value = std::get<LevelSettings>(settings);
            std::array<Lut, 4> tables;
            for (std::size_t i = 0; i < tables.size(); ++i) tables[i] = level_lut(image, value.channels[i]);
            const int limit = maximum(image);
            for (int sample = 0; sample <= limit; ++sample)
                for (std::size_t channel = 1; channel < 4; ++channel)
                    tables[channel][static_cast<std::size_t>(sample)] =
                        tables[0][tables[channel][static_cast<std::size_t>(sample)]];
            if (options.coverage_only) return apply_coverage_lut(image, tables[1], stop);
            return apply_point_filter(image, stop, [&](Pixel pixel) {
                if (!pixel.a) return pixel;
                return apply_rgb_luts(pixel, image, tables[1], tables[2], tables[3]);
            });
        }
        case FilterKind::ToneCurve: {
            const auto& value = std::get<ToneCurveSettings>(settings);
            std::array<Lut, 4> tables;
            for (std::size_t i = 0; i < tables.size(); ++i)
                tables[i] = tone_curve_lut(image, value.curves[i], value.interpolation);
            const int limit = maximum(image);
            for (int sample = 0; sample <= limit; ++sample)
                for (std::size_t channel = 1; channel < 4; ++channel)
                    tables[channel][static_cast<std::size_t>(sample)] =
                        tables[0][tables[channel][static_cast<std::size_t>(sample)]];
            if (options.coverage_only) return apply_coverage_lut(image, tables[1], stop);
            return apply_point_filter(image, stop, [&](Pixel pixel) {
                if (!pixel.a) return pixel;
                return apply_rgb_luts(pixel, image, tables[1], tables[2], tables[3]);
            });
        }
        case FilterKind::AutoContrast: return filter_detail::apply_auto_contrast(image,settings,{},options,stop,{});
        case FilterKind::SharpSoft:
            return apply_convolution(image, {1,-12,1,-12,144,-12,1,-12,1}, 100, stop);
        case FilterKind::SharpHard:
            return apply_convolution(image, {0,-1,0,-1,6,-1,0,-1,0}, 2, stop);
        case FilterKind::BlurSoft:
            return apply_convolution(image, {0,1,0,1,4,1,0,1,0}, 8, stop);
        case FilterKind::BlurHard:
            return apply_convolution(image, {1,2,1,2,4,2,1,2,1}, 16, stop);
        case FilterKind::UnsharpMask: return filter_detail::apply_unsharp(image,settings,{},stop,{});
        case FilterKind::GaussianBlur:
            return apply_gaussian(image, std::get<GaussianBlurSettings>(settings), options, stop);
        case FilterKind::Hsv: return filter_detail::apply_hsv(image,settings,{},stop,{});
        case FilterKind::ColorBalance: return filter_detail::apply_color_balance(image,settings,{},stop,{});
        case FilterKind::Dust:
            return filter_detail::apply_dust(image,settings,{},options,stop,{});
        }
    } catch (const std::exception& error) {
        return rejected(error.what());
    }
    return rejected("未知滤镜命令");
}

FilterResult apply_gaussian_filter(DocumentData& document,const GaussianBlurSettings& settings,
    const std::optional<Rect>& selection,std::stop_token stop,const std::optional<RasterSelection>& coverage){
    try{
        if(const auto error=validate_settings(FilterKind::GaussianBlur,settings);!error.empty())return rejected(error);
        if(stop.stop_requested())return cancelled();
        if(document.current_layer<0||document.current_layer>=int(document.layers.size()))return rejected("当前文档没有活动图层");
        const auto& layer=document.layers[std::size_t(document.current_layer)];
        if(layer.opaque||!admitted_raster_layer(layer.family))return rejected("当前图层没有高斯模糊栅格适配器");
        if(!filter_executes_on_layer(FilterKind::GaussianBlur,layer.family))return {};
        auto* plane=document.active_plane();
        if(!plane||plane->opaque||!plane->raster)return rejected("当前平面没有高斯模糊栅格适配器");
        if(!layer.common.flags[1]||!plane->common.flags[1])return {};
        const bool low=filter_detail::low_convolution_depth(plane->raster->depth);
        if(!low)validate_raster(*plane->raster);
        auto bounds=Rect{0,0,plane->raster->width,plane->raster->height};
        if(selection||coverage){
            if(plane->raster->resolution!=layer.common.resolution||plane->raster->resolution!=document.base.dpi||
               plane->raster->resolution!=plane->common.resolution)
                return rejected("高斯模糊选区的不同分辨率映射尚未接入");
            if(selection)bounds=raster_local_rect(layer,*plane->raster,*selection);
        }
        if(low){
            const auto result=filter_detail::apply_low_gaussian(*plane->raster,settings,bounds,stop);
            if(result.modified())plane->pixels_changed=true;
            return result;
        }
        const auto radius=settings.use_millimeters?settings.size_mm*plane->raster->resolution/25.4:double(settings.size_px);
        const auto result=detail::gaussian_region(*plane->raster,plane->common.opacity_mode,radius,bounds,nullptr,stop,coverage);
        if(!result.handled)return rejected(result.error);
        if(result.cancelled)return cancelled();
        if(result.changed)plane->pixels_changed=true;
        return {result.changed?FilterStatus::Applied:FilterStatus::Unchanged,result.changed_pixels,{}};
    }catch(const std::exception& error){return rejected(error.what());}
}
FilterResult apply_filter_region(Raster& image,FilterKind kind,const FilterSettings& settings,
    Rect bounds,const FilterExecutionOptions& options,std::stop_token stop,const std::optional<RasterSelection>& coverage){
    if(kind==FilterKind::Dust)return filter_detail::apply_dust(image,settings,bounds,options,stop,coverage);
    if(kind==FilterKind::ColorBalance)return filter_detail::apply_color_balance(image,settings,bounds,stop,coverage);
    if(kind==FilterKind::GaussianBlur&&image.depth!=32&&image.depth!=64)return filter_detail::apply_low_gaussian(image,settings,bounds,stop);
    if(kind==FilterKind::UnsharpMask)return filter_detail::apply_unsharp(image,settings,bounds,stop,coverage);
    if(kind==FilterKind::Hsv)return filter_detail::apply_hsv(image,settings,bounds,stop,coverage);
    if(kind==FilterKind::AutoContrast)return filter_detail::apply_auto_contrast(image,settings,bounds,options,stop,coverage);
    if(filter_detail::fixed_convolution(kind)&&image.depth!=32&&image.depth!=64)return filter_detail::apply_low_convolution(image,kind,settings,bounds,stop,coverage);
    if(image.depth==24&&filter_detail::table_filter(kind))return filter_detail::apply_rgb24(image,kind,settings,bounds,options,stop,coverage);
    try{
        validate_raster(image);
        if(const auto error=validate_settings(kind,settings);!error.empty())return rejected(error);
        if(stop.stop_requested())return cancelled();
        // Clip in int64 before converting the half-open edges back to int.
        const int left=int(std::clamp<std::int64_t>(bounds.x,0,image.width));
        const int top=int(std::clamp<std::int64_t>(bounds.y,0,image.height));
        const int right=int(std::clamp<std::int64_t>(std::int64_t(bounds.x)+std::max(0,bounds.width),left,image.width));
        const int bottom=int(std::clamp<std::int64_t>(std::int64_t(bounds.y)+std::max(0,bounds.height),top,image.height));
        bounds={left,top,right-left,bottom-top};if(!bounds.width||!bounds.height)return {};
        if(kind==FilterKind::GaussianBlur){
            const auto& gaussian=std::get<GaussianBlurSettings>(settings);
            const auto radius=gaussian.use_millimeters?gaussian.size_mm*image.resolution/25.4:double(gaussian.size_px);
            const auto result=detail::gaussian_region(image,options.coverage_only?4:options.white_is_transparent?5:1,radius,bounds,nullptr,stop,coverage);
            if(!result.handled)return rejected(result.error);if(result.cancelled)return cancelled();
            return {result.changed?FilterStatus::Applied:FilterStatus::Unchanged,result.changed_pixels,{}};
        }
        // Compute with the full immutable source. In particular, clipping the
        // input would change convolution halos.
        auto computed=image;const auto result=apply_filter(computed,kind,settings,options,stop);
        if(result.rejected()||result.cancelled()||!result.modified())return result;
        auto staged=image;std::set<TileCoord> tile_set;
        for(const auto& [tile,unused]:image.tiles)tile_set.insert(tile);
        for(const auto& [tile,unused]:computed.tiles)tile_set.insert(tile);
        const std::vector<TileCoord> tiles(tile_set.begin(),tile_set.end());std::size_t changed{};
        const bool complete=for_tile_pixels(image,tiles,stop,[&](int x,int y){
            if(x<left||x>=right||y<top||y>=bottom||(coverage&&!coverage->contains(x,y)))return;
            const auto after=computed.pixel(x,y);if(after==image.pixel(x,y))return;
            staged.set_pixel(x,y,after);++changed;
        });
        if(!complete||stop.stop_requested())return cancelled();
        if(!changed)return {};
        // Preserve physical backing outside the selected pixels, including
        // preexisting allocated transparent tiles and latent RGB lanes.
        image=std::move(staged);return {FilterStatus::Applied,changed,{}};
    }catch(const std::exception& error){return rejected(error.what());}
}
FilterResult apply_filter(DocumentData& document, FilterKind kind,
                          const FilterSettings& settings, std::stop_token stop) {
    return apply_filter_region(document,kind,settings,{},stop);
}
FilterResult apply_filter_region(DocumentData& document, FilterKind kind,
    const FilterSettings& settings,const std::optional<Rect>& selection,std::stop_token stop,
    const std::optional<RasterSelection>& coverage) {
    if(kind==FilterKind::GaussianBlur){
        if(!settings_match_filter(kind,settings))return rejected("滤镜参数类型与命令不匹配");
        return apply_gaussian_filter(document,std::get<GaussianBlurSettings>(settings),selection,stop,coverage);
    }
    try {
        if (document.current_layer < 0 || document.current_layer >= static_cast<int>(document.layers.size()))
            return rejected("当前文档没有活动图层");
        const auto& layer = document.layers[static_cast<std::size_t>(document.current_layer)];
        if (layer.opaque || layer.family == 0x40000200 || (layer.family & 0xf0000000u) == 0x40000000u)
            return rejected("调整图层滤镜尚未接入栅格后端");
        if (!admitted_raster_layer(layer.family))
            return rejected("当前矢量或未知图层族没有栅格滤镜适配器");
        if (!filter_executes_on_layer(kind, layer.family)) return {};
        if (layer.current_plane < 0 || layer.current_plane >= static_cast<int>(layer.planes.size()))
            return rejected("当前图层没有活动平面");
        const auto& source_plane = layer.planes[static_cast<std::size_t>(layer.current_plane)];
        if (source_plane.opaque || !source_plane.raster)
            return rejected("矢量、文字或未恢复活动平面不能使用栅格滤镜");
        if (primary_filter_layer(layer.family) && source_plane.common.opacity_mode == 4 &&
            (kind == FilterKind::AutoContrast || kind == FilterKind::Dust))
            return rejected("当前平面模式不支持自动对比度或除尘");
        DocumentData staged = document;
        auto& target_layer = staged.layers[static_cast<std::size_t>(staged.current_layer)];
        std::vector<std::size_t> targets;
        if (source_plane.common.opacity_mode == 4) {
            targets.push_back(static_cast<std::size_t>(target_layer.current_plane));
        } else {
            for (std::size_t index = 0; index < target_layer.planes.size(); ++index) {
                const auto& plane = target_layer.planes[index];
                if (plane.common.flags[1] && plane.raster) targets.push_back(index);
            }
        }
        std::size_t changed{};bool modified{};filter_detail::FilterCosts filter_costs;
        for (const auto index : targets) {
            auto& plane = target_layer.planes[index];
            if (plane.opaque || !plane.raster || (!plane.raster->can_edit()
                &&!(plane.raster->depth==24&&(filter_detail::table_filter(kind)||kind==FilterKind::AutoContrast||kind==FilterKind::Hsv||kind==FilterKind::ColorBalance))
                &&!(kind==FilterKind::UnsharpMask&&filter_detail::low_convolution_depth(plane.raster->depth))
                &&!(kind==FilterKind::Dust&&filter_detail::low_convolution_depth(plane.raster->depth))
                &&!(filter_detail::fixed_convolution(kind)&&filter_detail::low_convolution_depth(plane.raster->depth))))
                return rejected("图层包含此滤镜尚未支持的可见栅格平面");
            if(kind==FilterKind::AutoContrast&&plane.common.depth!=plane.raster->depth)
                return rejected("自动对比度的平面与栅格深度必须匹配");
            if(kind==FilterKind::Dust&&(!std::isfinite(target_layer.common.resolution)||target_layer.common.resolution<=0
                ||plane.common.resolution!=plane.raster->resolution))
                return rejected("除尘需要有效owner分辨率且plane与offscreen分辨率匹配");
            const FilterExecutionOptions options{plane.common.opacity_mode == 5,
                                                 plane.common.opacity_mode == 4};
            if((selection||coverage)&&(plane.raster->resolution!=document.base.dpi
                ||plane.raster->resolution!=target_layer.common.resolution||plane.raster->resolution!=plane.common.resolution))
                return rejected("滤镜选区的不同分辨率映射尚未接入");
            const auto bounds=selection?raster_local_rect(target_layer,*plane.raster,*selection):Rect{0,0,plane.raster->width,plane.raster->height};
            const auto local_bounds=selection||coverage?std::optional<Rect>(bounds):std::nullopt;
            const Raster* companion{};
            if(kind==FilterKind::Dust&&plane.family==0x12100001){
                for(const auto& other:target_layer.planes)if(other.family==0x11110002){
                    if(other.common.flags[1]){if(other.opaque||!other.raster)return rejected("除尘伴随平面没有有效光栅");companion=&*other.raster;}break;
                }
            }
            auto result = kind==FilterKind::Dust?filter_detail::apply_dust(*plane.raster,settings,local_bounds,options,stop,coverage,&filter_costs,target_layer.common.resolution,plane.common.resolution,companion)
                :kind==FilterKind::ColorBalance?filter_detail::apply_color_balance(*plane.raster,settings,local_bounds,stop,coverage,&filter_costs)
                :kind==FilterKind::UnsharpMask?filter_detail::apply_unsharp(*plane.raster,settings,local_bounds,stop,coverage,&filter_costs)
                :kind==FilterKind::Hsv?filter_detail::apply_hsv(*plane.raster,settings,local_bounds,stop,coverage,&filter_costs)
                :kind==FilterKind::AutoContrast?filter_detail::apply_auto_contrast(*plane.raster,settings,local_bounds,options,stop,coverage,&filter_costs)
                :filter_detail::fixed_convolution(kind)&&filter_detail::low_convolution_depth(plane.raster->depth)?filter_detail::apply_low_convolution(*plane.raster,kind,settings,local_bounds,stop,coverage,&filter_costs)
                :plane.raster->depth==24?filter_detail::apply_rgb24(*plane.raster,kind,settings,local_bounds,options,stop,coverage,&filter_costs)
                :selection||coverage?apply_filter_region(*plane.raster,kind,settings,bounds,options,stop,coverage)
                :apply_filter(*plane.raster, kind, settings, options, stop);
            if (result.cancelled() || result.rejected()) return result;
            if (result.modified()) {
                modified = true;
                changed += result.changed_pixels;
                plane.pixels_changed = true;
            }
        }
        if (!modified) return {};
        document = std::move(staged);
        return {FilterStatus::Applied, changed, {}};
    } catch (const std::exception& error) {
        return rejected(error.what());
    }
}

} // namespace retas
