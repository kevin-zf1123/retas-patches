#pragma once
#include "cel_folder.hpp"
#include "management_file.hpp"
#include <functional>
#include <optional>

namespace retas {
struct MngCutFolderInfo {
    std::int32_t width{},height{},dpi{},rate{};
    std::string field_0c,field_18,field_24,field_30;
    bool operator==(const MngCutFolderInfo&)const=default;
};
// Required tags1..8; missing/nonnumeric values reject instead of consuming
// native uninitialized locals. Tag0, ordering and unused trailing bytes do
// not participate. Narrow text is preserved with native first-NUL lookup.
MngCutFolderInfo decode_mng_cut_folder_info(std::span<const std::uint8_t>,std::stop_token stop={});
struct MngCatalogOptions {
    std::vector<std::string> image_extensions;
    // Callback overrides Windows Shell ordering within one reserved rank.
    // Nullopt selects the encoded CRT-unit fallback. UTF-8 uses deterministic
    // unsigned bytes and ASCII folding on hosts without a Shell result.
    std::function<std::optional<int>(const std::filesystem::path&,const std::filesystem::path&)> compare_paths;
    NativePreferenceEncoding text_encoding{};
};
struct MngCelFolder {
    std::filesystem::path path;
    std::uint16_t flags{};
    CelFolder owner;
};
struct MngFolderFailure {
    std::filesystem::path path;
    std::string error;
};
struct MngCutFolder {
    std::filesystem::path management_path,archive_path;
    MngCutFolderInfo info;
    std::vector<MngCelFolder> folders;
    std::vector<MngFolderFailure> rejected_folders;
    // Per-child Close, retryable after a failure; destructor performs no I/O.
    void close(std::stop_token stop={});
};
// MNG mode2: visible direct outer folders except exact "-Key". Visible inner
// folders replace their outer even when every inner Open fails. Per-root sort
// precedes return; Open may create cfinfo.cf6 in admitted child directories.
MngCutFolder open_mng_cut_folder(const std::filesystem::path& management_file,
    const MngCatalogOptions& options={},std::stop_token stop={});
MngCutFolder open_mng_cut_folder(const std::filesystem::path& management_file,
    const MngCatalogOptions&,CelFolderBudget&,std::stop_token stop={});
struct MngOutputCatalog {
    std::filesystem::path scan_path;
    bool management_found{};
    std::vector<std::filesystem::path> folders;
    std::vector<MngFolderFailure> rejected_folders;
};
// Direct visible .mng files in discovery order. Each temporary root closes
// after its sorted paths append; no global sort/dedup. Existing regular file
// scan paths yield an empty catalog. Missing paths also yield empty results.
MngOutputCatalog scan_mng_output_catalog(const std::filesystem::path&,
    const MngCatalogOptions& options={},std::stop_token stop={});
MngOutputCatalog scan_mng_output_catalog(const std::filesystem::path&,
    const MngCatalogOptions&,CelFolderBudget&,std::stop_token stop={});
}
