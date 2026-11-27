#pragma once
#include "retas/cut_folder.hpp"
namespace retas::cut_folder_detail {
std::u16string comparison_key(std::string_view,NativePreferenceEncoding);
std::optional<int> shell_compare(const std::filesystem::path&,const std::filesystem::path&);
}
