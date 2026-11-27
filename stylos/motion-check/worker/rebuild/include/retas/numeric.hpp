#pragma once

#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>

namespace retas {
namespace numeric_detail {
// Match the original CRT's 53-bit x87 precision even on hosts with wider
// floating-point registers. Separate stores also prevent fused operations.
inline double binary64(double value) noexcept {
    volatile double stored = value;
    return stored;
}
}

// PaintMan rtwUtl.dll rtDouble2Int/rtDouble2Short (same export address).
// Supported contract: round-to-nearest binary64 arithmetic, finite input and
// an int32-representable result. Invalid inputs/results return nullopt rather
// than reproducing the original unchecked CRT conversion outside this domain.
// This is NOT std::round: fractions are quantized to thousandths, exact half
// ties go toward zero, and the negative near-integer branch is intentional.
inline std::optional<std::int32_t> legacy_rt_double_to_int(double value) noexcept {
    static_assert(std::numeric_limits<double>::is_iec559 &&
                  std::numeric_limits<double>::digits == 53);
    if (!std::isfinite(value)) return std::nullopt;
    double integer{};
    const double fraction = std::modf(value, &integer);
    const double scaled = numeric_detail::binary64(std::abs(fraction) * 1000.0);
    double thousandths = std::trunc(scaled);
    if (numeric_detail::binary64(scaled - thousandths) >= 0.5) ++thousandths;
    const double quantized = numeric_detail::binary64(thousandths * 0.001);
    double result = integer;
    if (quantized >= 1.0) {
        // Original loc_1100EF27 adds one even for negative values:
        // rtDouble2Int(-0.9999) == +1, not -1.
        result = numeric_detail::binary64(integer + 1.0);
    } else if (quantized > 0.5) {
        result = numeric_detail::binary64(integer + (value < 0.0 ? -1.0 : 1.0));
    }
    if (result < std::numeric_limits<std::int32_t>::min() ||
        result > std::numeric_limits<std::int32_t>::max()) return std::nullopt;
    return static_cast<std::int32_t>(result);
}

inline std::optional<std::int32_t> legacy_rt_mul_div(
    std::int32_t value, std::int32_t multiplier, std::int32_t divisor) noexcept {
    if (divisor == 0) return 0;
    const double product = numeric_detail::binary64(double(value) * double(multiplier));
    return legacy_rt_double_to_int(numeric_detail::binary64(product / double(divisor)));
}
}
