#pragma once
#include "archive.hpp"
#include <optional>
#include <stop_token>

namespace retas {
// RETAS CutFolder .mng management records, independent of animated PNG MNG.
struct ManagementRecord {
    std::uint32_t tag{};
    Bytes payload;
    bool operator==(const ManagementRecord&) const = default;
};
using ManagementArchive=std::vector<ManagementRecord>;
inline constexpr std::size_t management_max_bytes=64ull*1024*1024;
inline constexpr std::size_t management_max_records=100000;

// ExactRaw: repeated u32LE tag, u32LE length, uninterpreted payload. Empty
// streams/values, unknown tags and duplicates roundtrip without conversion.
ManagementArchive decode_management_archive(std::span<const std::uint8_t>,std::stop_token stop={});
Bytes encode_management_archive(std::span<const ManagementRecord>,std::stop_token stop={});
// Native creation writes tag0 with the one-byte decimal string "0".
Bytes make_management_file();

// First matching tag wins. Search stops before its length or later records;
// unmatched payloads can be skipped past EOF. A reached partial header rejects.
// Strings consume at most255 wire bytes, then return their first-NUL prefix.
std::optional<std::string> management_string(std::span<const std::uint8_t>,
    std::uint32_t tag,std::stop_token stop={});
// Returns whether the string tag exists, even when decimal conversion fails.
// A nonnumeric value leaves current unchanged. Decimal prefix parsing follows
// the ASCII %d subset; overflow rejects. Its C-prefix is limited to99 bytes.
bool management_integer(std::span<const std::uint8_t>,std::uint32_t tag,
    std::int32_t& current,std::stop_token stop={});
// Reproduce native byte positioning: append absent tags; overwrite only the
// first match's length and payload, without moving/truncating following bytes.
// A size-changing replacement can consequently damage following framing.
// Validate/prepare before mutation; strings use a <=255-byte C-prefix.
void set_management_string(Bytes&,std::uint32_t tag,std::string_view,std::stop_token stop={});
void set_management_integer(Bytes&,std::uint32_t tag,std::int32_t,std::stop_token stop={});
}
