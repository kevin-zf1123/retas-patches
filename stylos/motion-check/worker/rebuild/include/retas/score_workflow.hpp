#pragma once
#include "score.hpp"
#include "cut_folder_root.hpp"

namespace retas {
struct CutFolderScoreOptions {
    // Accepted New Cut Length; its default in the original dialog is not
    // established. LayerCount comes from CutFolderNewSettings::layout.
    std::int32_t frame_count{};
    bool create_score{true}, add_background{};
};
struct CutFolderScoreCreation {
    bool created{}, score_created{};
    std::filesystem::path directory, metadata_path, score_path;
    std::string score_error;
};
// Parses the native RCNumber profile:1..9999 followed by0..3 ASCII letters,
// uppercased. Invalid text produces zero, as the New Cut caller ignores the
// native parse result. Arithmetic overflow rejects instead of calling atol.
ScoreNumber score_cut_number(std::span<const std::uint8_t>);
// Commit the CutFolder first, then optionally create <root-name>.sxf with
//24FPS/FrameDisp8/GuideLine6/Page144 and the accepted Scene/Cut strings.
// An ordinary Score failure is reported without rolling back the new root.
CutFolderScoreCreation create_score_cut_folder(const std::filesystem::path& parent,
    const CutFolderNewSettings&, const CutFolderScoreOptions&,
    CutFolderRootLoadSettings = {}, std::stop_token = {});

// Headless owner for the native Score file/CutFolder association. The basic
// ScoreDocument codec/editor remains independent of filesystem context.
class ScoreWorkflow {
    struct State;
    std::unique_ptr<State> state_;
public:
    explicit ScoreWorkflow(ScoreDocument, CutFolderRootLoadSettings = {});
    ~ScoreWorkflow();
    ScoreWorkflow(ScoreWorkflow&&) noexcept;
    ScoreWorkflow& operator=(ScoreWorkflow&&) = delete;
    ScoreWorkflow(const ScoreWorkflow&) = delete;
    ScoreWorkflow& operator=(const ScoreWorkflow&) = delete;
    static ScoreWorkflow open(const std::filesystem::path&,
        CutFolderRootLoadSettings = {}, std::stop_token = {},ScoreSoundPathEncoding = {});
    ScoreDocument& document();
    const ScoreDocument& document() const;
    const CutFolderRoot* cut_folder() const;
    const std::string& association_error() const;
    // Replaces the prior association. An ordinary admission/load failure
    // leaves no association and a diagnostic; cancellation/resource limits
    // propagate. A failed explicit release retains the old owner for retry.
    void refresh_cut_folder(std::stop_token = {});
    void save(const std::filesystem::path&, std::stop_token = {});
    // Explicit native Save As refreshes even when the chosen path is equal.
    void save_as(const std::filesystem::path&, std::stop_token = {});
    // CSV/TSF Save As keeps the SXF binding, then refreshes its context.
    void save_interchange(const std::filesystem::path&, std::stop_token = {});
    // Closes cached CF06 owners explicitly. Destruction never publishes files.
    void close(std::stop_token = {});
};
}
