#pragma once
#include "raster.hpp"
#include <array>

namespace retas {
struct SvdImportSettings {
    double dpi{144};
    std::int32_t clip{}, frame{}, direction{}, antialias{};
};

// WCEL GetImageInfo reads only the geometry prefix of section 1.
std::array<int, 2> svd_image_size(std::span<const std::uint8_t>, const SvdImportSettings & = {});
Raster decode_svd(std::span<const std::uint8_t>, const SvdImportSettings & = {});
} // namespace retas
