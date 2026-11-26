#pragma once
#include "archive.hpp"
#include <stop_token>

namespace retas {
struct CutFolderModelPath {
    Bytes image_path;
    std::int32_t relative_path_flag{};
    bool operator==(const CutFolderModelPath&)const=default;
};
// Native model path transport, without Shell/image binding or filesystem I/O.
// Read the archive's ASCII C-prefix/CR string. Every nonzero flag selects
// colon conversion and RemakePath; flag0 copies the runtime path verbatim.
std::string load_cut_folder_model_path(std::string_view root,
    std::span<const std::uint8_t> wire_path,std::int32_t relative_path_flag,
    std::stop_token stop={});
// Serialize an explicitly supplied bound path. Empty represents no usable
// binding. Native Find uses a case-insensitive substring, not containment;
// a hit sets flag1 even when MakeRelativePath returns its absolute fallback.
CutFolderModelPath save_cut_folder_model_path(std::string_view root,
    std::string_view bound_path,std::stop_token stop={});
}
