#pragma once
#include "retas/native_preference_host.hpp"
namespace retas::native_preference_detail {
void validate_encoding(NativePreferenceEncoding);
bool lead(std::uint8_t,NativePreferenceEncoding);
bool trail(std::uint8_t,NativePreferenceEncoding);
//rtIsDoubleByteCheckEx's local table probe, not a Unicode character walk.
bool double_byte(std::string_view,std::size_t,NativePreferenceEncoding);
bool equal(std::span<const std::uint8_t>,std::span<const std::uint8_t>,NativePreferenceEncoding);
// RC CRT _stricmp with LC_CTYPE=C: C prefixes, ASCII lowercase, unsigned
// byte order. This is the SBCS branch, not the DBCS/NLS comparison path.
int compare_c_locale(std::string_view,std::string_view);
}
