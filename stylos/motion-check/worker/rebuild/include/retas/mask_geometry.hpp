#pragma once
#include "retas/fill.hpp"
#include <limits>
#include <span>

namespace retas {
struct MaskPoint {int x{},y{};bool operator==(const MaskPoint&) const = default;};
enum class PolygonEdgeMode { HalfOpen,Inclusive };
// Native FillPolygon integer scanlines. Adds to the supplied mask. flag0
// uses half-open spans and skips horizontal replay; filter tools use flag1.
// Off-image rows retain their binary64 recurrence and count toward max_rows.
Rect mask_polygon(BinaryMask&,std::span<const MaskPoint>,
    std::int64_t max_rows=std::numeric_limits<std::int64_t>::max(),PolygonEdgeMode edge_mode=PolygonEdgeMode::Inclusive);
}
