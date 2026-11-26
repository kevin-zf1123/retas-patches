#pragma once
#include "archive.hpp"
#include <variant>

namespace retas {
// Rebuild safety limits, not native collection limits. The typed projection
// may reject a payload that the existing opaque Score codec can retain.
constexpr std::size_t score_objects_max_bytes=128U*1024U*1024U;
constexpr std::size_t score_objects_max_entries=100000;
// Per-Pencil and aggregate expansion cap for the typed projection.
constexpr std::size_t score_objects_max_points=1000000;
enum class ScoreMotionKind : std::uint16_t {Arrow=1,Text=2,FadeIn=4,FadeOut=8,Overlap=16};
struct ScoreMotionCommon {
    std::uint16_t kind_flags{},field6{};
    std::int32_t field8{},field_c{},frame0{},frame1{},x0{},y0{},x1{},y1{},field28{};
    Bytes suffix;
    bool operator==(const ScoreMotionCommon&) const=default;
};
struct ScoreArrowAttributes {
    std::uint16_t field2c{};
    std::int32_t field30{};
    std::uint32_t field34{};
    Bytes suffix;
    bool operator==(const ScoreArrowAttributes&) const=default;
};
struct ScoreTextAttributes {
    std::uint32_t field38{},field3c{};
    std::int32_t field40{},field44{};
    Bytes text; // Native RCString wire bytes; no Unicode/CRLF conversion.
    Bytes suffix;
    bool operator==(const ScoreTextAttributes&) const=default;
};
struct ScoreMotionObject {
    ScoreMotionCommon common;
    std::variant<std::monostate,ScoreArrowAttributes,ScoreTextAttributes> attributes;
    ScoreMotionKind kind() const noexcept {return ScoreMotionKind(common.kind_flags&0xcfff);}
    bool operator==(const ScoreMotionObject&) const=default;
};
struct ScorePencilPoint {
    std::int32_t x{},y{};
    bool operator==(const ScorePencilPoint&) const=default;
};
struct ScorePencilObject {
    std::int32_t stroke_width{};
    std::uint32_t color{};
    std::uint16_t coordinate_width{2},mode{};
    std::vector<ScorePencilPoint> points;
    Bytes points_suffix,mode_suffix;
    bool operator==(const ScorePencilObject&) const=default;
};
struct ScoreObjects {
    std::vector<ScoreMotionObject> motion;
    std::vector<ScorePencilObject> pencils;
    Bytes suffix;
    bool operator==(const ScoreObjects&) const=default;
};
enum class ScoreObjectsWriteMode {
    // Exact original widths. A width2 point must remain in 0..65535.
    PreserveWire,
    // Native known-field coordinate encoding: choose width2 if every SIGNED
    // coordinate <=65535, including negatives, then write their low16 bits.
    // Reload can therefore change negative values; the live model is untouched.
    // Raw strings and all extensions remain a rebuild transport policy, not
    // native runtime string normalization or unknown-field retention.
    NativeCanonical
};
// Strict typed admission: supported masked kinds, identical full outer/inner
// tags, no embedded NUL in Text, coordinate widths2/4 and bounded expansion.
// No frame, coordinate, color, style or stroke-width semantics are invented.
ScoreObjects decode_score_objects(std::span<const std::uint8_t> payload);
bool can_decode_score_objects(std::span<const std::uint8_t> payload) noexcept;
void validate_score_objects(const ScoreObjects& objects,ScoreObjectsWriteMode mode);
Bytes encode_score_objects(const ScoreObjects& objects,ScoreObjectsWriteMode mode);
}
