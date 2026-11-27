#pragma once
#include "raster.hpp"

namespace retas {
struct TgaSettings {bool compressed{true};int depth{24};};
enum class TgaPacketPolicy { LegacyRows, Continuous };
Raster decode_tga(std::span<const std::uint8_t> bytes,TgaPacketPolicy policy=TgaPacketPolicy::LegacyRows);
Bytes encode_tga(const Raster& image,TgaSettings settings={});
}
