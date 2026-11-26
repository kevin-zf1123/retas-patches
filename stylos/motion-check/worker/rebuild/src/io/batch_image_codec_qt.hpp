#pragma once
#include "retas/raster.hpp"
#include <stop_token>
namespace retas::batch_image_detail {
bool qt_readable(std::string_view);
Raster qt_decode(std::string_view,std::span<const std::uint8_t>,std::stop_token);
}
