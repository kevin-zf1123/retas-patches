#pragma once
#include "archive.hpp"
#include "score_objects.hpp"
#include "score_sound.hpp"
#include <array>
#include <filesystem>
#include <functional>
#include <optional>
#include <variant>

namespace retas {
constexpr std::size_t score_max_file_bytes=128U*1024U*1024U;
constexpr std::int32_t score_max_frames=6000,score_max_layers=100;
constexpr std::size_t score_copy_max_cell_visits=64U*1024U*1024U;
// Rebuild budget for Pencil scan, trim/shift and projection work; not native.
constexpr std::size_t score_pencil_cleanup_max_work=64U*1024U*1024U;
// Rebuild bound for opt-in motion adjustment's owned history records.
constexpr std::size_t score_motion_adjust_max_history_bytes=64U*1024U*1024U;
using ScoreNumber=std::array<std::uint8_t,8>; // BE number + four uninterpreted suffix bytes
struct ScoreGeneral {
    std::int32_t frame_count{},layer_count{};
    std::uint16_t fps{};
    ScoreNumber scene{},cut{};
    std::int32_t current_frame{},current_layer{};
    std::uint16_t frame_display{},guide_line{},page{};
    std::vector<std::uint16_t> settings;
    std::vector<std::int32_t> visibility;
    Bytes suffix;
    bool operator==(const ScoreGeneral&) const=default;
};
struct ScoreCell {
    std::uint16_t marker{};
    std::array<std::uint8_t,8> label{}; // native multibyte label; NOT RCNumber
    Bytes suffix;
    bool operator==(const ScoreCell&) const=default;
};
struct ScoreLayer {
    Bytes name; // native RCString wire bytes, not assumed to be UTF-8
    std::int32_t field{1};
    std::uint32_t cell_stride{10};
    std::vector<ScoreCell> cells;
    Bytes suffix;
    bool operator==(const ScoreLayer&) const=default;
};
enum class ScoreBank {Primary,Derived};
enum class ScoreLane : std::size_t {KeyAnimation,Inbetween,Voice,Sound,Camera,Direction,Info};
struct ScoreData {
    std::uint16_t version{0x100};
    ScoreGeneral general;
    Bytes direction; // RCString wire bytes; preserve native CR/DBCS encoding
    std::optional<std::int32_t> direction_mode;
    Bytes direction_suffix;
    std::vector<ScoreLayer> primary_layers,derived_layers;
    Bytes primary_suffix,derived_suffix;
    std::uint32_t voice_stride{6};
    std::vector<Bytes> voice;
    Bytes voice_suffix;
    Bytes sound_reference,sound_suffix;
    // Complete FF07 payload, including typed object/pencil sections. Kept
    // opaque so an exposure edit never discards camera/motion annotations.
    Bytes objects;
    std::array<std::uint16_t,7> section_order{0xff01,0xff02,0xff03,0xff04,0xff05,0xff06,0xff07};
    Bytes trailing;
    bool operator==(const ScoreData&) const=default;
};
ScoreData decode_score(std::span<const std::uint8_t> bytes);
Bytes encode_score(const ScoreData& score);
void validate_score(const ScoreData& score);
struct ScoreSaveCancelled : std::runtime_error {ScoreSaveCancelled():std::runtime_error("Score save cancelled") {}};
struct ScoreCopyCancelled : std::runtime_error {ScoreCopyCancelled():std::runtime_error("Score cel-number copy cancelled") {}};

class ScoreDocument {
    struct ObjectState {ScoreObjects objects;std::vector<std::uint64_t> identities;};
    struct Snapshot {ScoreData data;std::optional<ObjectState> objects;};
    struct CellRange {
        ScoreBank bank;
        std::size_t layer,frame;
        std::vector<std::vector<ScoreCell>> rows;
    };
    struct Metadata {
        std::uint16_t fps{};
        ScoreNumber scene{},cut{};
        std::uint16_t frame_display{},guide_line{},page{};
    };
    struct LayerPair {ScoreLayer primary,derived;};
    struct LayerChange {
        std::size_t index{};
        bool attach{};
        std::optional<LayerPair> pair;
    };
    struct FrameSlice {
        std::vector<std::vector<ScoreCell>> primary,derived;
        std::vector<Bytes> voice;
    };
    struct FrameChange {
        std::size_t index{},count{};
        bool insert{};
        // Deletion retains its original strips for every replay. Insertion
        // has no saved strips: redo creates zero frames, including extensions.
        std::optional<FrameSlice> original;
    };
    struct MotionDelete {std::uint64_t identity{};bool restore{};std::optional<ScoreMotionObject> removed;};
    struct MotionMove {std::uint64_t identity{};bool restore_before{};ScoreMotionObject before;std::optional<ScoreMotionObject> after;};
    struct PencilChange {bool restore_before{};std::vector<ScorePencilObject> before;std::optional<std::vector<ScorePencilObject>> after;};
    using CompoundChild=std::variant<Metadata,LayerChange,FrameChange,MotionDelete,MotionMove,PencilChange>;
    struct Compound {
        std::vector<CompoundChild> children;
        bool reverse{};
    };
    using RevisionPayload=std::variant<Snapshot,Bytes,CellRange,Metadata,LayerChange,FrameChange,Compound>;
    struct Revision {RevisionPayload payload;std::string label;std::uint64_t id{};};
    ScoreData data_;
    mutable std::optional<ObjectState> object_cache_;
    std::vector<Revision> undo_,redo_;
    bool undo_next_{true};
    std::filesystem::path path_;
    std::uint64_t revision_{},saved_{},next_{1},generation_{};
    struct SoundFileIdentity {std::uint64_t device{},file{},file_high{};bool operator==(const SoundFileIdentity&) const=default;};
    std::optional<ScoreSoundBinding> sound_binding_;
    std::optional<SoundFileIdentity> bound_sound_identity_;
    bool sound_dirty_{};
    ScoreSoundPathEncoding sound_encoding_;
    bool busy_{};
    bool transact_impl(const std::string& label,const std::function<void(ScoreData&)>& operation,bool direction_command);
    static Metadata metadata(const ScoreGeneral& general) noexcept;
    static void apply_metadata(ScoreGeneral& general,const Metadata& metadata) noexcept;
    static void attach_pair(ScoreData& data,std::size_t index,const LayerPair& pair);
    static LayerPair detach_pair(ScoreData& data,std::size_t index);
    static FrameSlice frame_slice(const ScoreData& data,std::size_t index,std::size_t count);
    static void insert_frame_range(ScoreData& data,std::size_t index,std::size_t count,const FrameSlice* saved);
    static void delete_frame_range(ScoreData& data,std::size_t index,std::size_t count);
    static ObjectState project_objects(const ScoreData& data);
    static void mirror_objects(ScoreData& data,const ObjectState& objects);
    void publish_pencils(ObjectState,const std::string&);
    static void cleanup_pencils(ScoreObjects& objects,std::int32_t frames,std::int32_t layers);
    static CompoundChild replay_child(ScoreData& data,std::optional<ObjectState>& objects,const CompoundChild& child);
    void swap_contents(ScoreDocument& other) noexcept;
    void replay(std::vector<Revision>& from,std::vector<Revision>& to);
    static std::optional<SoundFileIdentity> sound_identity(const std::filesystem::path&);
    static void delete_owned_sound(const std::filesystem::path&,SoundFileIdentity);
public:
    explicit ScoreDocument(ScoreData initial,ScoreSoundPathEncoding = {});
    ScoreDocument(const ScoreDocument& other);
    ScoreDocument(ScoreDocument&& other);
    // Assignment advances this object's generation independently of the
    // replacement's counter, invalidating any prepared save of older state.
    ScoreDocument& operator=(const ScoreDocument& other);
    ScoreDocument& operator=(ScoreDocument&& other);
    static ScoreDocument open(const std::filesystem::path& path,ScoreSoundPathEncoding = {});
    // Rebuild NEW packet: six settings and seven visibility slots, no opaque
    // General suffix. Native initialization replaces the first five settings,
    // all visibility values and both cursors. The rebuild requires and keeps
    // an explicit caller-supplied sixth setting because its native constructor
    // value is unproven; native accepted General does not supply these settings.
    // Background consumes one of the total layer slots.
    static ScoreDocument create(ScoreGeneral accepted,bool add_background=false);
    const ScoreData& data() const noexcept {return data_;}
    // View cursor changes are transient and do not create undo entries.
    void set_cursor(std::int32_t frame,std::int32_t layer);
    // Lazy strict projection. Signed live coordinates survive native low16
    // serialization; opaque input is untouched until an object semantic edit.
    const ScoreObjects& typed_objects() const;
    const std::filesystem::path& path() const noexcept {return path_;}
    bool modified() const noexcept {return revision_!=saved_||sound_dirty_;}
    bool can_undo() const noexcept {return !undo_.empty();}
    bool can_redo() const noexcept {return !redo_.empty();}
    std::uint64_t state_generation() const noexcept {return generation_;}
    // A candidate is validated before publication. False denotes a no-op.
    // Opaque direction extensions are immutable; mode changes require the
    // dedicated direction command because mode is outside replay history.
    bool transact(const std::string& label,const std::function<void(ScoreData&)>& operation);
    // Callers provide encoded native label bytes, at most eight without NUL.
    bool set_exposure(ScoreBank bank,std::size_t layer,std::size_t first,std::size_t count,std::span<const std::uint8_t> label);
    bool clear_exposure(ScoreBank bank,std::size_t layer,std::size_t first,std::size_t count);
    // Fill the inclusive rectangle in one cell-only history step. A repeat
    // count of zero cycles until the last selected frame.
    void auto_input(ScoreBank,int first_layer,int last_layer,int first_frame,int last_frame,
                    int start,int end,int spacing,int repeats);
    // Native COPY_CELNUMBER normalizes enabled Derived rows from Primary
    // anchors/relative markers. Acceptance always records one dirty cell-only
    // history step, including unchanged output. Cancellation/errors publish
    // nothing; repeated prefix shifts are bounded by the visit limit above.
    bool copy_cel_numbers(const std::function<bool()>& cancelled={});
    bool add_pencil(ScorePencilObject);
    bool erase_pencil(std::uint16_t mode,std::span<const ScorePencilPoint> centers,
                      int width,int height);
    bool rename_layer(std::size_t layer,Bytes name);
    // Standalone paired row commands. -1 appends; other insert indices are
    // gaps in 0..layer_count. Native count gates return false without history.
    // Attach leaves cursors current; detach only clamps an out-of-range layer
    // cursor. History captures the actual live pair when it is detached.
    bool insert_layer(std::int32_t index,Bytes name);
    bool delete_layer(std::size_t index);
    // Standalone frame commands use the native adjust=false contract: insert
    // leaves both FF07 collections untouched; delete removes/clamps only
    // motion anchors beyond the new end. Semantic object edits require strict
    // typed projection; untouched opaque insert payloads remain admissible.
    // Rebuild validation requires positive count; -1 appends or removes the
    // final capped slice. Preserved nonnative stride extensions in new blank
    // records are zero-filled as a rebuild transport policy.
    // Native 4..6000 bounds and available tail cap the actual count; a bound
    // gate returns false without history. Insertion keeps cursors; deletion
    // only upper-clamps current_frame. Insert redo creates blank frames;
    // delete undo restores the original cells/voice, leaving cursors current.
    bool insert_frames(std::int32_t index,std::size_t count);
    bool delete_frames(std::int32_t index,std::size_t count);
    // Opt-in native FIRST-motion adjustment; SECOND Pencil and sound stay
    // current. Requires a positive int32 raw request, which drives anchor
    // arithmetic even when the inserted/deleted cell count is capped.
    // Indexed delete adapter: interval start=min(resolved index,newF-1),
    // leaving the live cursor current. Native public delete instead reads its
    // post-clamp live cursor; these agree when input cursor==resolved index.
    // Interval length=min(raw request,oldF-4), ignoring available tail. Native
    // wrap32/sign-clamp and branch-specific point updates remain unchanged.
    bool insert_frames(std::int32_t index,std::size_t count,bool adjust_motion);
    bool delete_frames(std::int32_t index,std::size_t count,bool adjust_motion);
    bool order_layers(const std::vector<std::size_t>& destination_to_source);
    // Accepted General always records one dirty history item. Same dimensions
    // copy accepted cursors; metadata replay leaves them current. Dimension
    // changes require typed Pencil cleanup, ignore accepted cursors and clamp
    // live cursors. One compound resizes layers first, tail frames next, then
    // six metadata fields and Pencil cleanup; undo reverses this order. Motion
    // anchors remain unchanged. Settings,
    // visibility and suffix cannot be changed through this command.
    bool set_general(ScoreGeneral accepted);
    // Accepted native direction commands always append history, even for
    // identical values. Replay restores text only; mode and automatic lane
    // reveal remain current when this command is undone or redone.
    bool set_direction(Bytes text,std::int32_t mode);
    // Native lane toggles serialize without dirtying or adding history.
    bool set_lane_visible(ScoreLane lane,bool visible);
    // Metadata-only binding. Requires a saved Score folder; source/path/cache
    // admission is bounded and does not decode/play PCM. Copy mode stages and
    // validates a new owned file; unrelated destination collisions reject.
    // Explicit Reference and best-effort OPEN bindings never own their file.
    // Acceptance reveals Sound, dirties without adding/truncating history;
    // Undo/Redo leaves live binding and FF06 current. Failures are atomic.
    bool bind_sound(const std::filesystem::path& source,ScoreSoundBindMode mode=ScoreSoundBindMode::CopyIntoScoreFolder,
        const std::function<bool()>& cancelled={});
    // Clear binding; delete only an identity-checked modern owned copy when
    // requested. Referenced/opened original files are preserved. Strict delete
    // errors retain the binding, unlike native ignored shell-operation errors.
    // Explicit modern clear also removes a dangling nonempty FF06 record,
    // preserving its extension bytes and touching no unvalidated file. Native
    // UI SOUND_DELETE requires a live binding; passive OPEN remains lossless.
    bool remove_sound(bool delete_owned_copy=true);
    // Explicit native SOUND_DELETE intent, with a strict modern filesystem
    // adapter: DeleteBoundFile can delete a validated reopened/reference file.
    // Missing files clear normally; other filesystem failures retain state.
    bool remove_sound(ScoreSoundRemoveMode mode);
    const std::optional<ScoreSoundBinding>& sound_binding() const noexcept{return sound_binding_;}
    ScoreSoundPathEncoding sound_path_encoding() const noexcept{return sound_encoding_;}
    void undo();
    void redo();
    bool undo_next() const noexcept {return undo_next_;}
    void undo_or_redo();
    // Cancellation and callback edits reject before atomic replacement. A
    // resolved sound binding stays at its absolute audio location; FF06 is
    // recomputed against the destination folder. Unresolved opaque references
    // cannot move to another physical folder safely.
    void save(const std::filesystem::path& path,const std::function<bool()>& cancelled={});
    // Ordinary native CSV/TSF SaveAs: successful output clears dirty while
    // keeping the SXF source path, live content, undo/redo and sound binding.
    // Modern output is atomic; failed/cancelled preparation retains baseline.
    void save_interchange(const std::filesystem::path& path,const std::function<bool()>& cancelled={});
};
}
