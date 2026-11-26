#pragma once
#include "archive.hpp"
#include "geometry.hpp"
#include <array>
#include <memory>
#include <optional>
#include <stop_token>

namespace retas {
class Document;
struct DocumentData;
struct FrameTarget {std::size_t layer{},plane{};bool operator==(const FrameTarget&) const = default;};
struct FramePoint {std::int32_t x{},y{};bool operator==(const FramePoint&) const = default;};
// Known embedded24-byte BE packet. Origin's point is a getter cache; its
// authoritative position lives in BaseInfo.lengths2/3. Raw IEEE bits survive.
struct FrameParameters {FramePoint point;double scale{100},angle{};};
FrameParameters decode_frame_parameters(std::span<const std::uint8_t>);
Bytes encode_frame_parameters(const FrameParameters&,std::span<const std::uint8_t> suffix={});
struct FrameLength {double value{};std::int32_t unit{};double reference{144};};
struct FrameSettingPacket {std::array<FrameLength,2> position;double scale{100},angle{};};
// Queries do not refresh the Origin cache. Settings project world pixel Lengths;
// Origin dialog settings expose100/0, while geometry uses its stored scalars.
FrameParameters frame_parameters(const DocumentData&,FrameTarget,bool world=true);
FrameSettingPacket frame_settings(const DocumentData&,FrameTarget);
struct FrameGeometry {Rect unscaled,scaled;std::array<FramePoint,4> corners;};
FrameGeometry frame_geometry(const DocumentData&,FrameTarget,bool world=true);
// Native print geometry: solid outside rectangle, rounded safe area and six
// translucent registration segments. Safe-area rotation uses the local pivot.
struct FramePrintGeometry {
    std::array<FramePoint,4> outline;
    std::array<FramePoint,40> safe_area;
    std::array<std::array<FramePoint,2>,6> marks;
};
FramePrintGeometry frame_print_geometry(const DocumentData&,FrameTarget);
enum class FramePreset {TopLeft,BottomLeft,Center,TopRight,BottomRight};
enum class FrameDragOperation {Translate,Scale,Rotate};
// Projected integer view corners, in TL/BL/BR/TR order. Camera corners use
// half-open 8x8 handles; strict interior translates, Camera exterior rotates.
// Exact polygon edges outside handles are not admitted by this profile.
std::optional<FrameDragOperation> hit_frame_move(const std::array<FramePoint,4>&,
    FramePoint sample,bool camera);
// Integral view and world samples already mapped by the caller. No window,
// autoscroll, snapping or hit-test mapping is inferred. Origin only translates.
struct FrameDragPacket {FrameDragOperation operation{FrameDragOperation::Translate};FramePoint view,world,pivot;};
struct FrameMoveState {FrameTarget target;FrameSettingPacket settings;FrameGeometry geometry;bool dragging{};std::uint64_t notifications{};};
// Document owns the exclusive modal session. Retained views are immutable and
// safe after destruction; no mutable raster, payload or Document borrow escapes.
// Begin binds history's primary getter snapshot and dirties the target owner.
// Release ends only the drag. Accept always appends one item, even no-op; Cancel
// applies the projected dialog backup and leaves dirty without adding history.
// Bounded ordinary FrameLayer profile; numeric Camera controls10..1000,
// while Cancel restores its already-admitted raw0..1000 backup.
// Modern policy: finite/int32 arithmetic,512MiB aggregate logical frozen/source
// storage,128Mi cumulative guard-work units with finalization reserved at
// Begin; failed/stopped preparation publishes nothing.
// Raw public-data changes make the session stale. Stale Cancel releases modal
// ownership and rejects without overwriting external changes. Serialized API
// use is required; native6.6.0 static evidence is not a runtime golden.
class FrameMoveSession {
    friend class Document;
    struct Impl;
    std::shared_ptr<Impl> impl_;
    explicit FrameMoveSession(std::shared_ptr<Impl>);
    static std::shared_ptr<FrameMoveSession> prepare(const Document&,FrameTarget,std::stop_token);
    std::shared_ptr<FrameMoveSession> drag(const Document&,const FrameDragPacket&,std::stop_token) const;
    std::shared_ptr<FrameMoveSession> update(const Document&,FramePoint,FramePoint,bool,std::stop_token) const;
    std::shared_ptr<FrameMoveSession> settings(const Document&,const FrameSettingPacket&,std::stop_token,bool controls=true) const;
    std::shared_ptr<FrameMoveSession> preset(const Document&,FramePreset,std::stop_token) const;
    std::shared_ptr<FrameMoveSession> release(const Document&,std::stop_token) const;
    std::shared_ptr<FrameMoveSession> cancellation(const Document&,std::stop_token) const;
    void publish(Document&) noexcept;
    FrameParameters primary() const;
public:
    FrameMoveState state() const;
    bool matches(const Document&) const noexcept;
};
}
