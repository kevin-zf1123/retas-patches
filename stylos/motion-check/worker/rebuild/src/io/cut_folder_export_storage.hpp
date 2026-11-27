#pragma once
#include "retas/mn6_archive.hpp"
#include <string_view>
#include "retas/native_preference_host.hpp"
namespace retas::cut_folder_detail {
// Serialize a runtime root clone with the existing native save profile.
Bytes encode_runtime_mn6(const Mn6Archive&,std::string_view native_root,NativePreferenceEncoding,std::stop_token);
Bytes encode_runtime_mng(const Mn6RootInfo&,std::stop_token);
}
