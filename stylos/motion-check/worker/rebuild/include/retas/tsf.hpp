#pragma once
#include "archive.hpp"
#include <array>

namespace retas {
struct ScoreData;
// Modern safety limits, not native format/allocator limits.
inline constexpr std::size_t tsf_max_file_bytes=64ull*1024*1024;
inline constexpr std::size_t tsf_max_decoded_bytes=128ull*1024*1024;
inline constexpr std::uint32_t tsf_max_sources=100000,tsf_max_family_records=10000,tsf_max_children=100000;
struct TsfTriple {
    std::array<std::uint64_t,3> bits{}; // raw IEEE binary64, no arithmetic or normalization
    std::array<std::uint8_t,3> flags{};
    bool operator==(const TsfTriple&) const=default;
};
struct TsfTransform {
    std::array<TsfTriple,3> groups;
    std::array<std::uint64_t,2> anchor_bits{};
    std::array<std::uint8_t,2> anchor_flags{};
    bool operator==(const TsfTransform&) const=default;
};
struct TsfSourceRecord {
    TsfTransform transform;
    std::uint64_t final_bits{};
    std::uint8_t final_flag{};
    bool operator==(const TsfSourceRecord&) const=default;
};
struct TsfCell {
    std::uint32_t exposure{};
    TsfTransform transform;
    std::uint64_t percent_bits{},final_bits{};
    std::uint8_t percent_flag{},final_flag{};
    bool operator==(const TsfCell&) const=default;
};
struct TsfMain {
    std::uint32_t source_count{},page_count{},extra_count{},item_count{};
    std::uint32_t width{},height{},dpi{};
    std::uint16_t flag0{};
    std::uint32_t fps{};
    std::uint16_t flag1{};
    std::uint32_t step{},guide{};
    std::array<Bytes,10> strings; // bare encoded byte strings, at most63 bytes
    Bytes comment;
    std::array<std::uint8_t,94> tail{};
    Bytes suffix;
    bool operator==(const TsfMain&) const=default;
};
struct TsfFixedControl {
    std::array<std::uint32_t,2> words0{};
    std::array<std::uint16_t,3> shorts{};
    std::array<std::uint32_t,4> words1{};
    Bytes suffix;
    bool operator==(const TsfFixedControl&) const=default;
};
struct TsfFolder {
    Bytes folder,filename; // at most255 bytes each
    std::array<std::uint8_t,72> metadata{};
    std::uint32_t tail{};
    Bytes suffix;
    bool operator==(const TsfFolder&) const=default;
};
struct TsfGlobal {
    Bytes name;
    std::array<std::uint32_t,2> words0{};
    std::uint16_t short0{};
    std::array<std::uint32_t,3> words1{};
    std::vector<TsfSourceRecord> records;
    Bytes suffix;
    bool operator==(const TsfGlobal&) const=default;
};
struct TsfPage {
    Bytes name;
    std::array<std::uint32_t,7> words0{};
    std::array<std::uint16_t,3> shorts{};
    std::array<std::uint32_t,7> words1{};
    std::vector<TsfCell> cells;
    // Exact wire order after all cells: mode1[j],mode3[j], then next frame.
    std::vector<std::array<std::uint32_t,2>> modes;
    std::uint32_t tail{};
    Bytes suffix;
    bool operator==(const TsfPage&) const=default;
};
struct TsfChild {
    Bytes name;
    std::array<std::uint32_t,2> words0{};
    std::uint64_t bits{};
    std::array<std::uint32_t,2> words1{};
    bool operator==(const TsfChild&) const=default;
};
struct TsfItem {
    Bytes name,path;
    std::array<std::uint8_t,72> metadata{};
    std::array<std::uint32_t,2> words{};
    std::vector<TsfChild> children;
    std::array<std::uint32_t,3> tail{};
    Bytes suffix;
    bool operator==(const TsfItem&) const=default;
};
struct TsfExtra {
    Bytes name;
    std::uint32_t color{};
    std::vector<TsfTransform> records;
    Bytes suffix;
    bool operator==(const TsfExtra&) const=default;
};
struct TsfData {
    // Logical 5600 (0x15e0); raw bytes are 15 e0, without CRTS/CRDT prefix.
    std::uint16_t version{0x15e0};
    std::array<std::uint32_t,4> prologue{};
    TsfMain main;
    TsfFixedControl fixed;
    TsfFolder folder;
    TsfGlobal global;
    std::vector<TsfPage> pages;
    std::vector<TsfItem> items;
    std::vector<TsfExtra> extras;
    Bytes trailing;
    bool operator==(const TsfData&) const=default;
};
// Strict exact reads/counts and known5600 grammar; transport retention of
// every unnamed scalar, IEEE bit/flag, section suffix and file tail. Raw
// prologue words are preserved even when not canonical producer values.
TsfData decode_tsf(std::span<const std::uint8_t>);
Bytes encode_tsf(const TsfData&);
// Stateless, lossy scaffold from SXF Derived rows and selected General fields;
// it is not full SaveAs or an SXF document. Requires 4..6000 frames, 1..100
// layers, positive FPS, matching Derived rows, and source-profile ASCII text
// without NUL/CR. Direction is capped at 32767 bytes; Scene/Cut suffixes after
// the signed BE32 prefix must contain NUL. Primary rows, row fields, other
// settings/cursors, visibility, voice/sound and FF07 are ignored. Input is
// unchanged and no file I/O occurs. Transport comments allow 0..65535 bytes.
TsfData make_tsf_score_scaffold(const ScoreData&);
// Native standalone Score SaveAs projection (4F9FB0 with no CutFolder host).
// Uses the original default canvas/assets, Derived exposure holds and native
// byte strings, including CP936-aware archive CR expansion. Primary, Voice,
// Sound and FF07 are not read by this native writer. No asset discovery/I/O.
TsfData make_tsf_score(const ScoreData&);
// Associated TSF profile for a fresh direct-SWF output root. SWF is not a
// readable CF06 image format, so these output groups have no child records.
// Text and group names are runtime C-string bytes, rather than MN6 wire text.
struct TsfSwfSidecarContext {
    std::uint32_t width{},height{},dpi{};
    Bytes title,episode;
    std::vector<Bytes> groups;
};
TsfData make_tsf_swf_sidecar(const ScoreData&,const TsfSwfSidecarContext&);

}
