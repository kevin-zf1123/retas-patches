#pragma once
#include "preference_archive.hpp"
#include <filesystem>
#include <fstream>
#include <optional>
#include <string_view>

namespace retas {
struct NativePreferenceEncoding {
    std::uint32_t code_page{20127}; //Explicit, frozen;20127 retains the ASCII profile.
};
//Strict text bridges for callers with UTF8. Archive labels/values themselves
//remain raw code-page bytes; these helpers do not perform CR conversion.
Bytes encode_native_preference_text(std::string_view utf8,NativePreferenceEncoding);
std::string decode_native_preference_text(std::span<const std::uint8_t>,NativePreferenceEncoding);
// RCRegKey profile. Subkeys contain ordinary relative components split
// by '\\' or ':'. Mode bit1 reads and takes priority over bit2 (write).
// Missing read directories return nullopt. Missing/empty pref.pff reads miss.
class NativePreferenceHost {
    std::filesystem::path path_;
    bool reading_{},closed_{},loaded_{};
    std::ifstream reader_;
    std::size_t read_size_{};
    Bytes loaded_bytes_,pending_;
    std::size_t pending_records_{};
    NativePreferenceEncoding encoding_;
    NativePreferenceHost(std::filesystem::path,bool,NativePreferenceEncoding);
    bool load();
    void require_open() const;
public:
    static std::optional<NativePreferenceHost> open(const std::filesystem::path& root,
        std::string_view subkey,unsigned mode);
    static std::optional<NativePreferenceHost> open(const std::filesystem::path& root,
        std::string_view encoded_subkey,unsigned mode,NativePreferenceEncoding);
    NativePreferenceEncoding encoding() const noexcept{return encoding_;}
    NativePreferenceHost(const NativePreferenceHost&)=delete;
    NativePreferenceHost& operator=(const NativePreferenceHost&)=delete;
    NativePreferenceHost(NativePreferenceHost&&)=default;
    NativePreferenceHost& operator=(NativePreferenceHost&&)=default;
    // No implicit writes on destruction. Call close explicitly to observe
    // errors; failure retains pending data and permits a later retry.
    bool reading() const noexcept{return reading_;}
    bool closed() const noexcept{return closed_;}
    const std::filesystem::path& path() const noexcept{return path_;}
    std::span<const std::uint8_t> pending_bytes() const noexcept{return pending_;}
    // First match controls width. WidthMismatch returns an empty payload and
    // does not read that record's payload. Found borrows the loaded bytes.
    PrefLookupResult lookup(std::span<const std::uint8_t> label,std::size_t width);
    // Accumulate scan/string-copy work across a typed owner's field reads.
    PrefLookupResult lookup(std::span<const std::uint8_t> label,std::size_t width,
        std::uint64_t& used_work);
    std::optional<Bytes> string_value(std::span<const std::uint8_t> label);
    std::optional<Bytes> string_value(std::span<const std::uint8_t> label,
        std::uint64_t& used_work);
    // Ordered append, including duplicates. String labels/values use C-prefix
    // and RCArchive CR conversion. Empty values use the single-space sentinel.
    void append(const PrefRecord&);
    void append_string(std::span<const std::uint8_t> label,std::span<const std::uint8_t> value);
    void close();
};
}
