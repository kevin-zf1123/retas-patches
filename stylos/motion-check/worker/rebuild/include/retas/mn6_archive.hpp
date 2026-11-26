#pragma once
#include "archive.hpp"
#include <array>
#include <stop_token>

namespace retas {
// CT06 is the archive stored in .mn6 files. Floating fields use IEEE-754
// bits so raw transport also preserves signed zero and NaN payloads.
struct Mn6Length {
    std::uint64_t value_bits{};
    std::uint32_t unit{};
    std::uint64_t dpi_bits{};
    bool operator==(const Mn6Length&) const = default;
};
struct Mn6BaseInfo {
    std::array<Mn6Length,16> lengths;
    std::int32_t frame_mode{},flags{};
    std::uint64_t scale_bits{},dpi_bits{},dialog_dpi_bits{};
    Bytes suffix;
    bool operator==(const Mn6BaseInfo&) const = default;
};
struct Mn6RootInfo {
    std::uint16_t kind{};
    Bytes text_04,text_10,text_1c,text_28;
    std::int32_t field_34{},field_38{};
    Bytes text_48;
    std::int32_t field_54{};
    Bytes scalar_suffix,paper_name,paper_name_suffix;
    Mn6BaseInfo base;
    Bytes paper_suffix,suffix;
    bool operator==(const Mn6RootInfo&) const = default;
};
struct Mn6CategoryChild {
    Bytes first_text;
    std::int32_t field_0c{};
    Bytes second_text;
    std::uint32_t time{UINT32_MAX};
    Bytes suffix;
    bool operator==(const Mn6CategoryChild&) const = default;
};
struct Mn6Category {
    std::int32_t kind{};
    Bytes name,second_text;
    std::int32_t field_1c{};
    std::uint32_t time{UINT32_MAX};
    std::vector<Mn6CategoryChild> children;
    Bytes suffix;
    bool operator==(const Mn6Category&) const = default;
};
struct Mn6Model {
    std::int32_t field_00{};
    std::uint32_t time{UINT32_MAX};
    Bytes text_54,text_60,text_6c;
    std::uint32_t field_78{0x333333},field_7c{0xffffff},field_80{0x663300};
    std::int32_t field_84{};
    Bytes image_path;
    std::int32_t relative_path_flag{};
    Bytes suffix;
    bool operator==(const Mn6Model&) const = default;
};
struct Mn6Point {
    std::int32_t x{},y{};
    bool operator==(const Mn6Point&) const = default;
};
struct Mn6Stroke {
    std::int32_t width{};
    std::uint32_t color{};
    // Exactly 2 selects unsigned 16-bit pairs; all other values select i32.
    std::uint16_t coordinate_width{2};
    std::vector<Mn6Point> points;
    Bytes suffix;
    bool operator==(const Mn6Stroke&) const = default;
};
struct Mn6PencilGroup {
    std::int32_t canvas_width{640},canvas_height{480};
    std::vector<Mn6Stroke> strokes;
    bool operator==(const Mn6PencilGroup&) const = default;
};
struct Mn6Archive {
    std::uint16_t version{0x100};
    Mn6RootInfo info;
    std::array<std::vector<Mn6Category>,3> categories;
    Bytes categories_suffix;
    std::int32_t selected_model{};
    std::vector<Mn6Model> models;
    Bytes models_suffix;
    std::vector<Mn6PencilGroup> pencil_groups;
    Bytes pencil_groups_suffix,tail;
    bool operator==(const Mn6Archive&) const = default;
};
inline constexpr std::size_t mn6_max_bytes=64ull*1024*1024;
inline constexpr std::size_t mn6_max_records=100000;
inline constexpr std::size_t mn6_max_points=4ull*1024*1024;
// ExactRaw owns every byte string and nested unknown suffix. Versions
// 0000..01ff are admitted. Counts are strict nonnegative and cumulatively
// bounded. No path binding, image access or native text normalization occurs.
Mn6Archive decode_mn6_archive(std::span<const std::uint8_t>,std::stop_token stop={});
Bytes encode_mn6_archive(const Mn6Archive&,std::stop_token stop={});
// New-root defaults recovered from CT06 constructors: nine kinds grouped
// 0..2/3..5/6..8, one empty pencil group, 720x540 at144dpi. Labels are supplied
// by the caller's resource pack; the file itself carries no language identity.
Mn6Archive make_mn6_archive(std::uint16_t kind,const std::array<Bytes,9>& category_names);
}
