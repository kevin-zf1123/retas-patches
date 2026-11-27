#pragma once
#include "archive.hpp"
namespace retas {
std::string decode_cp936(std::span<const std::uint8_t> bytes);
Bytes encode_cp936(std::string_view utf8);
}
