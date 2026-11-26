#pragma once
#include "motion_check.hpp"
#include "score_workflow.hpp"

namespace retas {
class ScoreSoundPlayer;
enum class ScoreMotionRenderer { PaintMan, Stylos260 };
struct ScoreMotionRange {
    std::int32_t first{}, last{}; // zero based, inclusive; (0,0) means whole
};
struct ScoreMotionAudioAction {
    enum class Kind {Play, Stop};
    Kind kind{Kind::Stop};
    std::int32_t mode{1}, start_ticks{}, end_ticks{};
    bool only_if_playing{true};
    bool operator==(const ScoreMotionAudioAction&) const = default;
};
// The sink owns global sound-device integration. Without a sink, actions
// remain observable in events; this is also the deterministic CLI adapter.
struct ScoreMotionAudio {
    std::function<void(const ScoreMotionAudioAction&)> dispatch;
};
// Capture binding metadata by value; player is the host's shared global
// output and must outlive the session. Uses the real PCM engine on Play.
ScoreMotionAudio make_score_motion_audio(ScoreSoundPlayer&,
    std::optional<ScoreSoundBinding>,std::optional<std::uint32_t> native_fixed_rate = {});
struct ScoreMotionEvent {
    std::size_t frame_index{}, cache_id{};
    std::shared_ptr<const Raster> frame;
    Bytes voice;
    std::vector<ScoreMotionAudioAction> audio;
    bool blank{}, exited{}, playing{}, hud{true}, warming{true};
    int fps{};
    std::int64_t deadline_ms{};
};
struct ScoreMotionSettingsResult {
    PrefArchive preferences;
    bool save{}, play{};
};
// Dialog result2 cancels. Every other result saves;1219 also requests whole
// playback when that command was enabled before opening the dialog.
ScoreMotionSettingsResult accept_score_motion_settings(int dialog_result,
    bool whole_enabled, const MotionCheckSettings&, std::span<const PrefRecord> old = {});

// Snapshots Score data, CF06 entries, paths, settings and codec descriptors.
// Files are loaded lazily (ShowAll also probes them for page dimensions).
// No borrowed document/root survives construction. Commands1/2 navigate
// distinct exposure caches while paused;3/4 navigate frames. Navigation1..6
// during playback first pauses without moving;17 restores the Score FPS.
// The caller supplies a monotonic clock and serializes construction with
// source mutation. Playback never saves preferences or changes Score history.
class ScoreMotionCheckSession {
    struct State;
    std::unique_ptr<State> state_;
public:
    ScoreMotionCheckSession(const ScoreDocument&, const CutFolderRoot&,
        MotionCheckSettings = {}, ScoreMotionRange = {},
        BatchImageCodecs = default_batch_image_codecs(), ScoreMotionAudio = {},
        std::stop_token = {}, ScoreMotionRenderer = ScoreMotionRenderer::PaintMan);
    ~ScoreMotionCheckSession();
    ScoreMotionCheckSession(const ScoreMotionCheckSession&) = delete;
    ScoreMotionCheckSession& operator=(const ScoreMotionCheckSession&) = delete;
    ScoreMotionEvent tick(std::int64_t now_ms, std::span<const int> commands = {},
        std::stop_token = {});
    // Independent frame jobs share the frozen exposure plan. Do not call tick or
    // close concurrently. The visitor receives the exact bytes used to decode each cel.
    using SourceVisitor = std::function<void(const std::filesystem::path&,std::span<const std::uint8_t>)>;
    Raster render_frame(std::size_t frame,const SourceVisitor& = {},std::stop_token = {}) const;
    std::span<const std::size_t> unique_frames() const noexcept;
    std::array<int,2> unscaled_size() const noexcept;
    std::size_t representative(std::size_t frame) const;
    std::size_t cached_frames() const noexcept;
    const MotionCheckSettings& settings() const noexcept;
    // Native global StopIfPlaying, independent of PlaySound/binding, also
    // dispatched on error/destruction. Explicit close returns it without a sink.
    std::vector<ScoreMotionAudioAction> close();
};
}
