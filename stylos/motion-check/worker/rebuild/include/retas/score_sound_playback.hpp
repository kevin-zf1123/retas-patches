#pragma once
#include "score.hpp"
#include <memory>
#include <stop_token>

namespace retas {
struct ScoreSoundPlayRequest {
    std::uint16_t mode{0}; //0 whole file(default);1 half-open600Hz range
    std::uint32_t start_ticks{},end_ticks{};
    std::optional<std::uint32_t> native_fixed_rate;
};
struct ScoreSoundPcm {
    std::uint32_t fixed_rate{};
    std::uint16_t channels{},bits_per_sample{};
    Bytes bytes; //unsigned8 or signed16 little-endian, interleaved
};
// Snapshots the selected PCM bytes, with native aligned/clamped offsets and
// AIFF playback conversion. Empty ranges return an empty clip. Max512MiB.
ScoreSoundPcm decode_score_sound_pcm(const ScoreSoundBinding&,
    ScoreSoundPlayRequest = {},std::stop_token = {});
// Portable output policy: floor-index/zero-order-hold resampling to an integer device rate.
// Frame selection and original container decoding precede this adaptation.
ScoreSoundPcm resample_score_sound_pcm(const ScoreSoundPcm&,std::uint32_t rate,
    std::stop_token = {});
Bytes encode_score_sound_wave(const ScoreSoundPcm&);
struct ScoreSoundOutput {
    std::function<void(std::shared_ptr<const ScoreSoundPcm>)> start;
    std::function<bool()> is_playing;
    std::function<void()> stop;
};
// One instance is shared by all Score commands and MotionCheck sessions in
// its host. Serial event-thread calls only; callbacks may not reenter it.
class ScoreSoundPlayer {
    ScoreSoundOutput output_;
    std::shared_ptr<const ScoreSoundPcm> clip_;
    bool busy_{};
public:
    explicit ScoreSoundPlayer(ScoreSoundOutput);
    ~ScoreSoundPlayer();
    ScoreSoundPlayer(const ScoreSoundPlayer&)=delete;
    ScoreSoundPlayer& operator=(const ScoreSoundPlayer&)=delete;
    bool is_playing();
    bool play(const ScoreSoundBinding&,ScoreSoundPlayRequest = {},std::stop_token = {});
    bool stop();
};
ScoreSoundPlayer& global_score_sound_player();
struct ScoreSoundSelection {std::int32_t mode{1},first{},last{};};
ScoreSoundPlayRequest score_sound_play_request(const ScoreDocument&,ScoreSoundSelection = {},
    std::optional<std::uint32_t> native_fixed_rate = {});
//41042 uses floor(600/FPS)*boundary; equal endpoints replace last with frame
// count. Selection modes0/16 disable Play.41043 stops global output regardless
// of this Score's binding. Recognized commands are consumed even if disabled.
bool query_score_sound_command(int command,const ScoreDocument&,ScoreSoundPlayer&,
    ScoreSoundSelection = {},bool backend_available=true,int global_guard=0);
bool execute_score_sound_command(int command,const ScoreDocument&,ScoreSoundPlayer&,
    ScoreSoundSelection = {},bool backend_available=true,int global_guard=0,
    std::stop_token = {},std::optional<std::uint32_t> native_fixed_rate = {});
// Optional Qt Multimedia backend. Factory creation performs no device probe;
// start requires the caller's QCoreApplication/event loop and owning thread.
bool score_sound_device_available() noexcept;
ScoreSoundOutput make_score_sound_device_output();
// CLI owner for QCoreApplication and the output event loop. Opens a device
// only when explicitly called; returns after completion/error/cancellation.
void play_score_sound_device(std::shared_ptr<const ScoreSoundPcm>,std::stop_token = {});
}
