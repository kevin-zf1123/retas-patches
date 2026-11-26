#pragma once
#include "raster.hpp"
#include <functional>

namespace retas {
// Values match the single-choice libpng filter settings used by PNG.rf6.
enum class PngFilter : unsigned { None = 8, Sub = 16, Up = 32, Average = 64, Paeth = 128 };
struct PngSettings {
    PngFilter filter{PngFilter::None};
    bool write_16bit{};
};
struct PngWriteContext {
    // Invoked before row 100, 200, ... only when either dimension exceeds
    // 4000. No final notification or cancellation check occurs in compression.
    std::function<void(std::size_t materialized_rows, std::size_t total_rows)> progress;
    std::function<bool()> cancelled;
};
class PngWriteCancelled : public std::runtime_error {
public:
    PngWriteCancelled() : std::runtime_error("PNG write cancelled") {}
};
Bytes encode_png(const Raster& image, PngSettings settings = {}, const PngWriteContext& context = {});
}
