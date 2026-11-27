#pragma once
#include "archive.hpp"
#include "native_preference_host.hpp"
#include <filesystem>
#include <stop_token>

namespace retas {
struct CelFolderRecord {
    Bytes filename,number;
    std::int32_t position{};
    Bytes suffix;
    bool operator==(const CelFolderRecord&)const=default;
};
struct CelFolderMetadata {
    std::uint16_t version{0x100};
    std::int32_t field_70{10},field_74{1};
    std::vector<CelFolderRecord> records;
    Bytes body_suffix,file_suffix;
    bool operator==(const CelFolderMetadata&)const=default;
};
inline constexpr std::size_t cel_folder_max_bytes=64ull*1024*1024;
inline constexpr std::size_t cel_folder_max_records=100000;
// Strict BE CF06 sections. Lossless transport retains wire strings, version
// and all suffixes. Negative counts and incomplete sections reject.
CelFolderMetadata decode_cel_folder_metadata(std::span<const std::uint8_t>,std::stop_token stop={});
Bytes encode_cel_folder_metadata(const CelFolderMetadata&,std::stop_token stop={});
struct CelFolderEntry {
    std::string filename,number;
    std::int32_t position{};
    bool operator==(const CelFolderEntry&)const=default;
};
struct CelFolderIndex {
    std::int32_t field_70{10},field_74{1};
    std::vector<CelFolderEntry> entries;
    bool operator==(const CelFolderIndex&)const=default;
};
// Cumulative used counters for callers opening several related folders.
struct CelFolderBudget {
    std::uint64_t work{};
    std::size_t allocation_bytes{};
};
// Native RCString projection (default ASCII20127, explicit ACP overloads).
// C-prefix/CR conversion; writer emits version0100
// and known fields. These functions drop raw suffixes, as native saves do.
CelFolderIndex load_native_cel_folder_index(std::span<const std::uint8_t>,std::stop_token stop={});
Bytes save_native_cel_folder_index(const CelFolderIndex&,std::stop_token stop={});
CelFolderIndex load_native_cel_folder_index(std::span<const std::uint8_t>,std::stop_token,NativePreferenceEncoding);
Bytes save_native_cel_folder_index(const CelFolderIndex&,std::stop_token,NativePreferenceEncoding);
// 4C7810/4C5910/4C5BA0: last numeric run in the filename stem, clamped6000,
// normalized decimal + up to two ASCII suffix positions. No number =>empty.
std::string cel_folder_number(std::string_view filename);
std::string cel_folder_number(std::string_view filename,NativePreferenceEncoding);

class CelFolder {
    std::filesystem::path root_;
    CelFolderIndex index_;
    NativePreferenceEncoding encoding_{};
    bool dirty_{},created_{},closed_{},write_suppressed_{};
    CelFolder()=default;
    void publish(std::stop_token);
public:
    CelFolder(const CelFolder&)=delete;
    CelFolder& operator=(const CelFolder&)=delete;
    CelFolder(CelFolder&&)noexcept;
    CelFolder& operator=(CelFolder&&)noexcept;
    // Explicit registry class1/flag0x10 extensions, without leading dots.
    // CEL/DGA always use the native archive-header/layer probe first; TSF
    // stays excluded even when supplied in this image-extension collection.
    static CelFolder open(const std::filesystem::path& directory,
        std::span<const std::string> image_extensions={},std::stop_token stop={});
    static CelFolder open(const std::filesystem::path& directory,
        std::span<const std::string> image_extensions,CelFolderBudget&,
        std::stop_token stop={});
    static CelFolder open(const std::filesystem::path&,std::span<const std::string>,
        CelFolderBudget&,std::stop_token,NativePreferenceEncoding,bool read_only=false);
    const CelFolderIndex& index()const{return index_;}
    bool dirty()const{return dirty_;}
    bool created()const{return created_;}
    bool write_suppressed()const{return write_suppressed_;}
    // Missing metadata publishes during Open; existing changed metadata
    // publishes at explicit Close. Read-only metadata suppresses writes and
    // clears dirty. Destruction performs no implicit I/O.
    void set_position(std::string_view filename,std::int32_t position);
    void close(std::stop_token stop={});
};
}
