#pragma once
#include "archive.hpp"
#include <array>
#include <filesystem>
#include <optional>

namespace retas {
// Modern bounded metadata profile, not a PCM decoder or playback engine.
// These safety limits are not native RCSound format limits.
inline constexpr std::uint64_t score_sound_max_file_bytes=512ull*1024*1024;
inline constexpr std::size_t score_sound_max_chunks=100000;
enum class ScoreSoundFormat {Wave,Aiff};
struct ScoreSoundMetadata {
    ScoreSoundFormat format{};
    std::uint16_t channels{},bits_per_sample{};
    double sample_rate{},duration_seconds{};
    std::uint64_t sample_frames{},data_offset{},data_bytes{};
    std::array<std::uint8_t,10> aiff_rate{}; // original80-bit COMM rate, zero forWAV
    bool operator==(const ScoreSoundMetadata&) const=default;
};
// WAV: PCM1, mono/stereo8/16, coherent rates/alignment and complete RIFF.
// AIFF: classic FORM AIFF, COMM18, mono/stereo8/16 and complete SSND with
// blockSize0. AIFC/compression and QuickTime rate-rounding parity are external.
// Rates must be finite and positive: WAV<=65535, AIFF<65536 (unsigned16.16).
// Computed duration must also remain finite; fractional metadata rates remain
// admissible when this bound holds.
// Unknown chunks/odd padding are walked safely. Duplicate known chunks fail.
ScoreSoundMetadata inspect_score_sound(std::span<const std::uint8_t>,ScoreSoundFormat);
// Native case-insensitive .wav/.aif/.aiff routing. Reads only metadata and
// chunk headers; validates declared PCM ranges without allocating PCM bytes.
ScoreSoundMetadata read_score_sound_metadata(const std::filesystem::path&);

// FF06 carries ACP bytes without an encoding tag. The caller selects that
// machine policy explicitly:20127 ASCII(default),932,936,949,950,1252,65001;
//0 selects GetACP on Windows. The selection is runtime state, not SXF data.
struct ScoreSoundPathEncoding {
    std::uint32_t code_page{20127};
    bool operator==(const ScoreSoundPathEncoding&) const=default;
};
ScoreSoundPathEncoding normalize_score_sound_path_encoding(ScoreSoundPathEncoding);
// ':' separates relative components; '..' ascends. Conversion is lossless,
// component-aware and preserves DBCS trail bytes. Empty/control/separator
// components and absolute Windows/UNC spellings reject.
Bytes score_sound_relative_reference(const std::filesystem::path& score_file,
    const std::filesystem::path& audio_file,ScoreSoundPathEncoding = {});
std::filesystem::path resolve_score_sound_reference(std::span<const std::uint8_t>,
    const std::filesystem::path& score_file,ScoreSoundPathEncoding = {});

enum class ScoreSoundBindMode {CopyIntoScoreFolder,Reference};
enum class ScoreSoundRemoveMode {OwnedCopyOnly,DeleteBoundFile};
struct ScoreSoundBindCancelled : std::runtime_error {ScoreSoundBindCancelled():std::runtime_error("Score sound binding cancelled") {}};
struct ScoreSoundBinding {
    std::filesystem::path path; // absolute physical audio location
    ScoreSoundMetadata metadata;
    bool owned_copy{}; // explicit modern provenance; never serialized inFF06
    bool operator==(const ScoreSoundBinding&) const=default;
};
struct ScoreSoundSample {
    std::uint8_t left{},right{128};
    bool operator==(const ScoreSoundSample&) const=default;
};
// AIFF ParseAIFFHeader is an external QuickTime dependency. Without captured
// native Fixed bits, the portable policy truncates rate*65536. An explicit
// override carries observed unsigned16.16 bits without guessing the rounding.
std::uint32_t score_sound_fixed_rate(const ScoreSoundMetadata&,
    std::optional<std::uint32_t> native_fixed = {});
// Bounded native waveform projection, not playback: one sample frame at a
// 1/600-second tick, mono right128, native signed16 division toward zero and
// AIFF8's historical subtract255 rule. The binding overload reads the current
// file using cached metadata; it never silently rebinds changed audio. Sampling
// uses trunc(fixed16.16_rate*frame_bytes) before tick arithmetic/alignment.
// Overflow/out-of-data returns nullopt rather
// than reproducing native wrapped offsets or uninitialized short-read output.
std::optional<ScoreSoundSample> sample_score_sound(const ScoreSoundBinding&,std::uint32_t ticks600,
    std::optional<std::uint32_t> native_fixed = {});
std::optional<ScoreSoundSample> sample_score_sound(const std::filesystem::path&,std::uint32_t ticks600,
    std::optional<std::uint32_t> native_fixed = {});
}
