#pragma once
#include "vector_probe.hpp"
#include <optional>
#include <span>

namespace retas::vector_mask_detail {
struct StrokeSample {
    VectorFaceVertex point;
    float pressure{1};
    float length{};
    int mode{},serial{};
};
struct StrokeSpan {
    StrokeSample first,last;
    std::optional<StrokeSample> control;
    bool new_model{};
};
enum class StrokeActionKind { ClearPreview, Main, Preview };
struct StrokeAction {StrokeActionKind kind;StrokeSpan span;};
struct StrokeStep {
    StrokeSample raw;
    bool clear_preview{},selected{},prune_raw{};
    int anchor_serial{-1};
    VectorFaceVertex prune_point;
    std::optional<StrokeSpan> preview;
    std::vector<StrokeSpan> emitted;
    std::vector<StrokeSample> pressure_events;
    //5CC690 may interleave main/preview/main, or consume several packets.
    // Aggregate signals above describe the step; replay uses this order.
    std::vector<StrokeAction> actions;
};
struct StrokeSamplingSettings {
    float accuracy{4},max_perpendicular{96};
};
//5CCD20/5CC690 ordered accepted packets, before width/endpoint materialization.
// The same sampler drives mask contours. Steps retain the native auxiliary
// preview and prune signals needed for TempPen dirty; they are not a mask.
std::vector<StrokeStep> sample_stroke_packets(std::span<const StrokeSample>,
    StrokeSamplingSettings,std::uint64_t& storage,vector_probe_detail::Budget);
// Intermediate 5C9860 product, before 5D5720 normalizes intersections and
// emits Regions. Coordinates are native integers, not sampler float bits.
struct ContourGraph {
    VectorLineArchive graph;
    std::vector<int> vertices,models,lines;
    VectorBounds bounds;
    bool rectangle{};
};
// Recovered5C9860 caller packets, not arbitrary live-pen sampling settings.
// FaceColor uses32.f/200.f;55C020 PolylineOutline uses8.f/50.f.
// Both have accuracy>4, so5C9B70 skips its normalized-angle branch.
//Mapped raw points retain the geometry consumer's +/-134217727 bound;
//native wrap/overflow behavior outside that admitted domain is not exposed.
enum class ContourProfile { FaceColor, PolylineOutline };
ContourGraph trace(const vector_probe_detail::Scope&,std::uint64_t& storage,
    vector_probe_detail::Budget);
ContourGraph trace(const vector_probe_detail::Scope&,std::uint64_t& storage,
    vector_probe_detail::Budget,ContourProfile);
//5C9860 CommonCopy2bit preserves source2 codes; other depths use native
//Get32/Set32 quantization. Scope extent is captured before that conversion.
ContourGraph trace_raster(const vector_probe_detail::Scope&,const Raster&,
    std::uint64_t& storage,vector_probe_detail::Budget);
}
