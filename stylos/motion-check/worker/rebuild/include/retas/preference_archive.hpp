#pragma once
#include "archive.hpp"

namespace retas {
struct PrefRecord {
    Bytes label;
    Bytes payload;
    bool operator==(const PrefRecord&) const = default;
};
using PrefArchive = std::vector<PrefRecord>;
// Modern safety limits, not limits proved for the native RCRegKey runtime.
inline constexpr std::size_t preference_archive_max_bytes = 64ull * 1024 * 1024;
inline constexpr std::size_t preference_archive_max_records = 100000;

// ExactRaw stores records as u16BE label byte count + raw label bytes + i16BE
// payload byte count + raw payload bytes. There is no envelope, text/CR
// conversion, or NUL interpretation; zero payload is valid, negative sizes
// and short reads reject. Modern 64 MiB/100000-record caps are not native
// format limits. Native C-string label scan and empty-pending no-rewrite
// lifecycle are not modeled by this codec.
PrefArchive decode_preference_archive(std::span<const std::uint8_t> bytes);
Bytes encode_preference_archive(std::span<const PrefRecord> records);

enum class PrefLookupStatus { Found, Missing, WidthMismatch };
struct PrefLookupResult {
    PrefLookupStatus status{PrefLookupStatus::Missing};
    // Borrows the first matching record even when its width does not match.
    std::span<const std::uint8_t> payload;
};
// Comparison is raw byte equality, not CRT __mbscmp or RCString semantics.
// The source archive must outlive the returned span; changing its matching
// payload may invalidate the span.
PrefLookupResult lookup_preference_record(std::span<const PrefRecord> records,
    std::span<const std::uint8_t> label, std::size_t expected_width);

Bytes pref_i32_payload(std::int32_t value);
Bytes pref_u64_payload(std::uint64_t value);
Bytes pref_f64_payload(double value);
std::int32_t read_pref_i32(std::span<const std::uint8_t> payload, std::size_t origin = 0);
std::uint64_t read_pref_u64(std::span<const std::uint8_t> payload, std::size_t origin = 0);
double read_pref_f64(std::span<const std::uint8_t> payload, std::size_t origin = 0);

enum class PrefMergeMode { NativeClose, PreserveUnshadowed };
// Both modes retain pending duplicates and shadow old labels against labels
// from the full pending archive. NativeClose stops scanning old records at the
// first zero payload; PreserveUnshadowed retains unshadowed zero records and
// records after them. Matching here is exact raw-byte equality, while native
// labels use an NUL-terminated multibyte scan. Newline/locale semantics,
// unsafe old tails, and the native empty-pending lifecycle are not modeled.
PrefArchive merge_preference_archives(std::span<const PrefRecord> pending,
    std::span<const PrefRecord> old_records, PrefMergeMode mode);
}
