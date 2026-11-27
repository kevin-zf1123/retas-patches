#pragma once
#include "cel_sequence.hpp"
#include "preference_archive.hpp"

namespace retas {
struct MotionCheckSettings {
    double scale{100};
    std::int32_t pause{},pause_time{1},show_light_table{},selected_area{};
    std::uint32_t background{0xffffff},blank_color{0xc0c0c0};
    std::uint16_t show_score_layer{2};
    std::int32_t show_voice{},play_sound{},darken{},fps_index{2},show_shadow_layer{},show_all{};
    bool operator==(const MotionCheckSettings&)const=default;
};
MotionCheckSettings load_stylos_motion_preferences(std::span<const PrefRecord>);
MotionCheckSettings load_motion_check_preferences(std::span<const PrefRecord>,MotionCheckSettings={});
PrefArchive store_motion_check_preferences(const MotionCheckSettings&,std::span<const PrefRecord> old={});
enum class MotionTextFonts { None, Qt };
struct MotionFrameRequest {
    MotionCheckSettings settings;
    Rect fixed_area{}; // current page or current selection bounds + origin
    bool only_line{};
    std::shared_ptr<const Raster> light_table;
    // Explicit modern font adapter for ordinary Text/Indication. A visible
    // nonempty Text source requires Qt; hidden/OnlyLine/empty sources do not.
    // Qt preparation runs on the application main thread, without a window.
    MotionTextFonts text_fonts{MotionTextFonts::None};
};
Raster prepare_motion_check_frame(const DocumentData&,const MotionFrameRequest&,std::stop_token={});
struct MotionCheckRange {std::int32_t first{1},last{};}; // inclusive, one based
struct MotionCheckEvent {
    std::size_t index{};
    std::shared_ptr<const Raster> frame;
    bool blank{},exited{},playing{},hud{},warming{};
    std::int64_t deadline_ms{};
};
// Commands1..16 have the native CEL MotionCheck meanings. Escape27 is
// accepted during the first cycle and pause blank; other commands wait until
// warm-up completes. The caller supplies a monotonic clock, without GDI or
// keyboard ownership. Automatic clock advancement occurs at most once per
// tick and never catches up; explicit commands execute in supplied order.
class MotionCheckSession {
    CelSequence& sequence_;
    MotionFrameRequest request_;
    std::size_t first_{},last_{},index_{},live_index_{};
    DocumentData live_;
    std::vector<std::shared_ptr<const Raster>> cache_;
    std::uint64_t storage_{};
    std::int64_t deadline_{},last_clock_{};
    bool entered_{},clock_started_{},started_{},playing_{true},hud_{},warming_{true},blank_{},exited_{},closed_{};
    std::shared_ptr<const Raster> frame(std::size_t,std::stop_token);
public:
    MotionCheckSession(CelSequence&,MotionFrameRequest,MotionCheckRange={},std::stop_token={});
    MotionCheckEvent tick(std::int64_t now_ms,std::span<const int> commands={},std::stop_token={});
    const MotionCheckSettings& settings()const noexcept{return request_.settings;}
    std::size_t cached_frames()const noexcept;
    // Normal close is the only persistence point. Exceptions/cancellation do
    // not implicitly save settings. The returned records can be stored in the
    // native Data/MotionCheckDlg/pref.pff owner by an embedding application.
    PrefArchive close(std::span<const PrefRecord> old={});
};
}
