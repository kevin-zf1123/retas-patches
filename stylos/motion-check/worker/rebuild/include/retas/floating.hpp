#pragma once
#include "document.hpp"
#include "vector_geometry.hpp"

namespace retas {
// Live kind is a payload/carrier classification, not a paste command or a
// history mode. A selection-derived capture has no explicit target binding.
enum class FloatingKind { None=0,General=1,PaintMono=2,TargetedPaintMono=3 };
struct FloatingTarget {
    std::size_t layer{},plane{};
    bool operator==(const FloatingTarget&) const = default;
};
struct FloatingSource {
    std::size_t layer{};
    std::vector<std::size_t> planes;
};
struct FloatingSourceIdentity {std::uint64_t owner{};std::vector<std::uint64_t> planes;};
struct FloatingSelection {
    FloatingKind kind{FloatingKind::None};
    std::shared_ptr<const std::vector<Layer>> layers;
    // Indices project native source-object references while the document's
    // topology is unchanged. Order and insertion history remap these references; other
    // ordinary topology edits must settle before changing it.
    std::shared_ptr<const std::vector<FloatingSource>> sources;
    bool same_document_source{};
    std::array<int,2> delta{};
    Rect tight_bounds,capture_frame;
    std::optional<FloatingTarget> target;
    // Resident clipboard source membership survives cloning independently
    // of payload identities. Resolve it only when lowering into artwork.
    std::shared_ptr<const std::vector<FloatingSourceIdentity>> clipboard_sources;
    bool native_clipboard{},clipboard_local{},clipboard_current_plane_only{};
    bool active() const {return kind!=FloatingKind::None;}
};
// Native floating history omits the live capture/source frame. Content is
// immutable and can be shared until an operation prepares replacement data.
std::shared_ptr<const FloatingSelection> floating_history_snapshot(const FloatingSelection&);

struct FloatingPaintSourceHistory {
    std::size_t layer{};
    // Owner-local raw16 context+78, before the binder's16-unit inflation.
    // Null binds the full owner; a present zero rectangle is a partial bind.
    std::optional<VectorBounds> raw_damage;
};
struct FloatingCapture {
    DocumentData remainder;
    std::shared_ptr<const FloatingSelection> floating;
    // Concrete source do-item binding dirties its target independently of
    // payload presence, including an admitted Paint prelude with no output.
    bool source_history{};
    bool indication_active{},indication_redraw{};
    // Paint212 binds before category/child/worker success gates. Keep these
    // descriptors even when no floating payload was produced.
    std::vector<FloatingPaintSourceHistory> paint_source_history{};
};
// Consume the current primitive/plane marks, then perform native615390 once
// on the remainder, including an empty capture. The latch controls clearing;
// it does not authorize recomputing marks before the transfer. State0 can
// consume marked vector content; raster capture requires an active selection.
FloatingCapture prepare_live_floating_capture(const DocumentData&,const RegionSelection&,
    bool indication_active,std::stop_token stop={});
// Normal no-carrier sub_672300 capture. Layer and plane selection markers
// choose the targets; input selection is in its own carrier coordinates.
// Canonical seven-depth raster branches with same-owner/finite child DPI,
// plus admitted native16 Vert, mixed Paint/Pure and marked Ruler/Shape
// geometry, including an actual State4 indication cache. Stage every source
// cut and consumer admission before publication.
// Missing-mask ClrRect and allocated-mask Transmit both use Initialize
// transparent white; physical block visits and pruning remain distinct.
// Standalone activation from an inactive indication latch followed by capture.
// Document Tool MoveImage uses the explicit live entry above.
FloatingCapture prepare_floating_capture(const DocumentData&,const RegionSelection&);
struct FloatingMaterialization {DocumentData data;bool content_history{};};
// Shared local-capture admission for preview and writeback. Validate the
// source-object map and both raster sides without mutating them. Vector
// validation owns private decoded carriers for actual consumer admission.
void validate_local_floating(const DocumentData&,const FloatingSelection&);
// Explicit SELECTPASTE binding, independent of local capture source indices.
// Canonical raster payloads/destinations use native typed cross-depth copy.
// Pure/Vert/Paint targets use the recovered typed family query/execute split;
// admitted fixed-zero Ruler/Shape/241 target slots retain native no-op behavior.
void validate_targeted_floating(const DocumentData&,const FloatingSelection&);
bool floating_target_family_compatible(std::uint32_t target,std::uint32_t source) noexcept;
// Ordinary local-capture writeback to its original source objects. The source
// mapping is valid until a normal edit settles the pending content. Foreign
// paste/import topology and explicit carrier bindings are separate paths.
FloatingMaterialization prepare_floating_materialization(const DocumentData&,const FloatingSelection&);
// TRNS-only remaining aggregate ledger and cancellation. Ordinary MoveImage
// retains its original defaults and callers; values advance only on success.
// Pure preflight of the same native capture walks before source cloning.
void admit_transform_floating_capture_work(const DocumentData&,const RegionSelection&,std::uint64_t work_remaining);
FloatingCapture prepare_transform_floating_capture(const DocumentData&,const RegionSelection&,
    std::uint64_t& work_remaining,std::uint64_t& bytes_remaining,std::stop_token stop);
FloatingMaterialization prepare_transform_floating_materialization(const DocumentData&,const FloatingSelection&,
    std::uint64_t& work_remaining,std::uint64_t& bytes_remaining,std::stop_token stop);
}
