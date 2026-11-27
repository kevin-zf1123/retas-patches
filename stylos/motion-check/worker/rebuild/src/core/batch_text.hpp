#pragma once
#include "retas/native_preference_host.hpp"
#include <algorithm>

namespace retas::batch_text_detail {
inline std::string utf8(const std::filesystem::path& path){
    if(path.native().size()>262140)throw std::length_error("Batch host path exceeds conversion limit");
    const auto s=path.generic_u8string();return {reinterpret_cast<const char*>(s.data()),s.size()};
}
inline std::string decode(std::string_view raw,NativePreferenceEncoding encoding){
    if(raw.size()>32767||raw.find_first_of(std::string_view("\0\r",2))!=raw.npos)throw std::invalid_argument("Batch text exceeds32767 bytes or contains NUL/CR");
    return decode_native_preference_text({reinterpret_cast<const std::uint8_t*>(raw.data()),raw.size()},encoding);
}
inline std::string encode(std::string_view text,NativePreferenceEncoding encoding){
    const auto raw=encode_native_preference_text(text,encoding);
    if(raw.size()>32767)throw std::length_error("Batch encoded text exceeds32767 bytes");
    return {raw.begin(),raw.end()};
}
inline std::filesystem::path host_path(std::string_view raw,NativePreferenceEncoding encoding){
    const auto text=decode(raw,encoding);
    if(std::ranges::any_of(text,[](unsigned char c){return c<32||c==127;}))throw std::invalid_argument("Control character in Batch path");
#ifndef _WIN32
    if(text.find_first_of("\\:")!=text.npos||text.starts_with("//"))throw std::invalid_argument("Windows Batch path requires a host mapping");
#endif
    return std::filesystem::path(std::u8string(text.begin(),text.end()));
}
inline std::string native_path(const std::filesystem::path& path,NativePreferenceEncoding encoding){return encode(utf8(path),encoding);}
}
