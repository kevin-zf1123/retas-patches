#pragma once
#include <cstdint>

namespace retas {
// Menu line edits reuse tool parameters but have distinct native entry gates.
enum class LineEditCaller { Tool, Menu, Batch };
enum class FilterToolKind { Blur, Dust };
struct FilterToolRangeSettings {
    std::int32_t pen_type{};
    double pen_width_mm{3.0};
    std::int32_t pen_width_px{30};
    std::int32_t pressure_on{}, sync_view_scale{};
    bool operator==(const FilterToolRangeSettings&) const = default;
};
struct FilterToolSettings {
    FilterToolRangeSettings range;
    double size_mm{0.5};
    std::int32_t size_px{5};
    std::int32_t dust_type{};
    bool operator==(const FilterToolSettings&) const = default;
};
inline FilterToolSettings default_filter_tool_settings(FilterToolKind kind) {
    FilterToolSettings settings;
    if (kind == FilterToolKind::Blur) {
        settings.size_mm = 1.0;
        settings.size_px = 10;
    }
    return settings;
}
} // namespace retas
