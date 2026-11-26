#pragma once
#include "raster.hpp"
#include "geometry.hpp"
#include <array>
#include <memory>
#include <optional>
#include <string_view>
#include <stop_token>
#include <vector>

namespace retas {
class Document;
struct DocumentData;
struct RegionSelection;
struct FloatingSelection;
struct FloatingMaterialization;
namespace raster_history_detail { struct Edit; struct Budget; struct FloatingBinding; }
enum class FloatingTransformOperation : std::uint32_t { Scale=2,Rotate=4 };
enum class FloatingTransformBegin { Started,NoSelection,NoUsablePayload,Unsupported };
struct FloatingTransformPoint {
    double x{},y{};
    bool operator==(const FloatingTransformPoint&) const = default;
};
using FloatingTransformQuad=std::array<FloatingTransformPoint,4>;
struct FloatingTransformPacket {
    FloatingTransformQuad quad{};
    FloatingTransformPoint pivot{};
    std::int32_t angle{}; // numeric preview value; RotateRelease has its own explicit angle
    std::uint32_t reversals{}; // scale crossing bits 0x10/0x20
    bool operator==(const FloatingTransformPacket&) const = default;
};
struct FloatingTransformState {
    FloatingTransformPacket packet;
    Rect bounds;
    std::array<int,2> delta{},base_compensation{};
    FloatingTransformOperation operation{FloatingTransformOperation::Scale};
    std::uint64_t origin{};
    std::size_t records{};
};
struct FloatingTransformOutcome {bool accepted{},changed{},history{};};
// Native public IDs, distinct from whole-page CEL_ROTATE/MIRRER.
enum class FloatingTransformCommand : std::uint32_t {
    Scale=40084,Rotate=40085,ReverseHorizontal=40087,ReverseVertical=40082
};
struct FloatingTransformCommandResult {
    bool handled{},state_changed{},session_active{};
    std::optional<FloatingTransformBegin> begin;
    FloatingTransformOutcome outcome;
    std::size_t records{};
};
std::optional<FloatingTransformCommand> floating_transform_command(std::string_view) noexcept;
// Local CELW query:1 enabled,2 disabled. This does not probe capture/profile
// admission. Application broker/modal gating belongs to the calling host.
int query_floating_transform_command(const Document&) noexcept;

// Double geometry producers. Release is separate from numeric Preview; these
// functions only compute packets and never publish content or append records.
FloatingTransformPacket floating_transform_scale_packet(const FloatingTransformPacket&,
    double x_scale,double y_scale,FloatingTransformPoint anchor);
FloatingTransformPacket floating_transform_rotate_packet(const FloatingTransformPacket&,
    std::int32_t degrees,FloatingTransformPoint pivot);
FloatingTransformPacket floating_transform_translate_packet(const FloatingTransformPacket&,double dx,double dy);
Rect floating_transform_bounds(const FloatingTransformQuad&);
// Document-owned modal service, distinct from ordinary MoveImage floating.
// Public access exposes values or an independent raster copy, never writable
// aliases of admitted snapshots. Normal same-DPI General raster payloads only;
// explicit/foreign/vector/topology materialization is unsupported. Begin
// admits eight retained source-graph equivalents plus two Cancel working
// equivalents before cloning, with matching metadata allowances for staged
// value copies (512MiB aggregate). Capture, resampling, writeback and preview raster work
// share a128Mi sampling/visit budget; raw snapshot storage is billed separately.
// Released selection carriers and records consume additional retained space.
class FloatingTransformSession {
    struct Impl;
    std::shared_ptr<const Impl> impl_;
    explicit FloatingTransformSession(std::shared_ptr<const Impl>);
    friend class Document;
    static std::shared_ptr<FloatingTransformSession> prepare(const Document&,FloatingTransformOperation,
        std::uint64_t origin,std::stop_token,FloatingTransformBegin&);
    bool matches(const Document&) const;
    void bind(const Document&,const DocumentData&,const RegionSelection&,
        std::shared_ptr<const FloatingSelection>,std::array<std::uint64_t,3> generations,std::stop_token stop={});
    std::shared_ptr<FloatingTransformSession> preview(const FloatingTransformPacket&,std::stop_token) const;
    std::shared_ptr<FloatingTransformSession> release(const FloatingTransformPacket&,std::uint32_t flags,
        std::int32_t angle,std::stop_token) const;
    std::shared_ptr<FloatingTransformSession> position(int dx,int dy,std::optional<FloatingTransformPoint> pivot) const;
    std::shared_ptr<FloatingTransformSession> command(std::uint32_t flags,std::stop_token) const;
    std::shared_ptr<FloatingTransformSession> switch_operation(FloatingTransformOperation) const;
    const DocumentData& before() const;
    DocumentData snapshot_before() const;
    const DocumentData& admitted_data() const;
    bool failed_capture_changed() const;
    bool failed_capture_source_history() const;
    bool capture_indication_active() const;
    std::shared_ptr<const DocumentData> raw_baseline() const;
    DocumentData remainder() const;
    const RegionSelection& selection_before() const;
    const RegionSelection& selection_live() const;
    std::shared_ptr<const FloatingSelection> prior_floating() const;
    std::shared_ptr<const FloatingSelection> live_floating() const;
    std::shared_ptr<const raster_history_detail::Edit> capture_history() const;
    std::array<std::uint64_t,2> available_budget() const;
    FloatingMaterialization materialize(std::stop_token,std::uint64_t* work_remaining=nullptr,
        std::uint64_t* bytes_remaining=nullptr,
        std::vector<std::shared_ptr<const raster_history_detail::Edit>>* paste_history=nullptr) const;
public:
    FloatingTransformState state() const;
    Raster preview_raster(int target_depth=32,std::stop_token stop={}) const;
    // Deep copy of the real released record raster, before target writeback.
    Raster transformed_plane(std::size_t layer,std::size_t plane,std::stop_token stop={}) const;
};
}
