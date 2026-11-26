#pragma once
#include "frame_move.hpp"
#include <optional>

namespace retas {
struct VanishingPointTarget {std::size_t layer{},plane{};bool operator==(const VanishingPointTarget&) const = default;};
using VanishingPointPixel=FramePoint;
struct VanishingPoint {FrameLength x{0,1,0},y{0,1,0};std::int32_t selected{};};
struct VanishingPointParameters {
    std::vector<VanishingPoint> points;
    std::int32_t angle_step{5};std::uint32_t color{0x00808080};std::int32_t opacity{50};
    bool legacy_header{};
    Bytes header_suffix,points_suffix,suffix;
};
// Plane.specific: header section12/16+, points section44*N+, outer suffix.
// Raw codec preserves IEEE bits, legacy header and all three tails. Counts are
// bounded100000 and aggregate borrowed/retained/output storage512MiB.
VanishingPointParameters decode_vanishing_point_parameters(std::span<const std::uint8_t>);
Bytes encode_vanishing_point_parameters(const VanishingPointParameters&);
// NativeLoad/clone materialization: unit-2 -> factory1; reference not>0 ->0;
// angle outside1/5/10/15/30 ->5; header becomes16 known bytes. Tails retained
// as a modern lossless extension. Signaling NaNs reject active materialization;
// raw transport retains them. No original x87 NaN payload golden is claimed.
VanishingPointParameters materialize_vanishing_point_parameters(const VanishingPointParameters&);
// Queries consume runtime/effective Length fields. Raw archive DTOs should
// first pass materialize; unit0/ref0 and unit4/unknown return zero directly.
std::vector<VanishingPointPixel> vanishing_point_pixels(const VanishingPointParameters&,double dpi);
VanishingPointParameters vanishing_point_parameters(const DocumentData&,VanishingPointTarget);
enum class VanishingPointHitKind {Neutral=1,Add=4,Delete=5,Move=6};
struct VanishingPointHit {VanishingPointHitKind kind{VanishingPointHitKind::Neutral};std::optional<std::size_t> index;};
// Centers and sample are already integer view coordinates. First half-open
// [cx-4,cx+4)x[cy-4,cy+4) match wins. Ten limits only empty-space Add.
VanishingPointHit hit_vanishing_points(std::span<const VanishingPointPixel> centers,VanishingPointPixel sample,bool alt=false);
struct VanishingPointSample {VanishingPointPixel view,world;};
struct VanishingPointMoveState {VanishingPointTarget target;VanishingPointParameters parameters;bool dragging{};std::uint64_t notifications{},redraws{};};
// Document-exclusive modal. Begin materializes ONLY the target's known fields
// before Bind; primary/Cancel/lazy first-Undo opposite are effective parameters.
// Cancel restores effective complete parameters, keeps owner dirty, adds no
// history. Accept always adds one entry. Release only ends the gesture.
// Pre-mapped integral world/view samples; no zoom/autoscroll/GUI inference.
// Modern atomic allocation/stop/stale policy and bounded logical snapshots
// match Frame Move:512MiB logical aliases,128Mi guard/copy units,100000 points.
// Finalization work is reserved at Begin; growth is precharged before copying.
// History replays complete known fields while retaining current transport tails.
// Stale Cancel releases ownership without foreign rollback. Inert move/release
// do not mutate parameters; headless settings require a released gesture.
class VanishingPointMoveSession {
    friend class Document;
    struct Impl;std::shared_ptr<Impl> impl_;
    explicit VanishingPointMoveSession(std::shared_ptr<Impl>);
    static std::shared_ptr<VanishingPointMoveSession> prepare(const Document&,VanishingPointTarget,std::stop_token);
    std::shared_ptr<VanishingPointMoveSession> press(const Document&,VanishingPointSample,std::span<const VanishingPointPixel>,bool,std::stop_token) const;
    std::shared_ptr<VanishingPointMoveSession> move(const Document&,VanishingPointSample,std::stop_token) const;
    std::shared_ptr<VanishingPointMoveSession> release(const Document&,std::stop_token) const;
    std::shared_ptr<VanishingPointMoveSession> apply(const Document&,const VanishingPointParameters&,std::stop_token,bool cancel=false) const;
    void publish(Document&) noexcept;
    std::shared_ptr<const VanishingPointParameters> primary() const;
public:
    VanishingPointMoveState state() const;
    bool matches(const Document&) const noexcept;
};
}
