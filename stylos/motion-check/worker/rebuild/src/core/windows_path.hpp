#pragma once
#include <filesystem>
#include <string>

namespace retas::windows_filesystem {
// Win32 extended paths keep long filenames and UTF-16 spelling at API calls.
inline std::wstring api_path(const std::filesystem::path& requested){
    auto path=std::filesystem::absolute(requested).lexically_normal();path.make_preferred();auto value=path.native();
    if(value.starts_with(L"\\\\?\\"))return value;
    if(value.starts_with(L"\\\\"))return L"\\\\?\\UNC\\"+value.substr(2);
    return L"\\\\?\\"+value;
}
}
