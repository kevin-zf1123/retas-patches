#pragma once

#include <cstdint>

namespace retas {

enum class RasterStrokeTool { Brush,Airbrush };

// Shared provider memory, not a persistence schema. Brush saves seven fields;
// Airbrush saves nine. The remaining fields are session/internal configuration.
struct RasterStrokeSettings {
    RasterStrokeTool tool_kind{RasterStrokeTool::Brush};
    double width_mm{3.0};
    std::int32_t width_px{30};
    bool pressure_on{},pressure_on_opacity{},sampling_on{};
    std::int32_t sampling_level{4};
    std::int32_t writeback_mode_raw{}; // provider +0x34; modes 0/1/2 are known
    std::int32_t mask_mode_raw{};      // provider +0x38
    std::int32_t hardness{100},interval{15};
    bool fade_on{};
    std::int32_t fade_step{20};
    bool stationary_repeat{};
    std::int32_t pattern_raw{1};       // provider +0x50; only 1 is a circle
    std::int32_t auxiliary_54_raw{};   // coordinate/subpixel and AA gate
};
struct BrushToolSettings : RasterStrokeSettings {};
struct AirbrushToolSettings : RasterStrokeSettings {
    AirbrushToolSettings(){tool_kind=RasterStrokeTool::Airbrush;interval=25;writeback_mode_raw=1;auxiliary_54_raw=1;}
};

// A captured target/configuration packet. Offset-named states are deliberately
// not mapped to Document flags here: the caller must resolve that mapping.
// Native admission is broader than implemented raster kernels; a caller must
// additionally check decoded payload and kernel depth support before painting.
struct RasterStrokeTargetContext {
    bool target_present{true};
    bool host_state_40c{},layer_state_20{},plane_state_20{};
    bool layer_state_28{true},layer_state_1c{true},plane_state_28{true};
    std::uint32_t layer_vslot50_flags{};
    std::uint32_t plane_family{0x12100001u};
    bool physical_width{};
    double owner_dpi{144.0};
    bool target_state_4b0{},target_state_4a8{},target_state_4ac{};
    float target_sampling_basis{}; // incoming op +0x290, no inferred units
    bool caller_allows_mask{};
    std::int32_t mask_arg0{},mask_arg1{};
    bool alpha_plane{}; // resolved by the destination adapter, not family bits
    bool white_key{};
};

enum class RasterStrokePattern { Square,Circle };
enum class RasterStrokeWriteback { Base=0,Blend=1,WeightCopy=2 };
enum class RasterStrokeTerminal { Base,Blend,WeightCopy,AlphaCopy,AlphaBlend,AlphaWeightCopy };
enum class RasterStrokeEvent { Begin,Move,Release };

struct RasterStrokeRuntime {
    float radius16{};
    double radius_pixels{};
    bool sampling_enabled{};
    std::int32_t sampling_level{};
    float target_sampling_basis{};
    std::uint32_t target_flags{};
    int operation_mode{1}; // sub_538CF0: 2 only for unsampled/unflagged raster
    bool pressure_on{},pressure_on_opacity{},pressure_off_copy_mode{true};
    RasterStrokePattern pattern{RasterStrokePattern::Circle};
    std::int32_t pattern_raw{1},hardness{100},interval{15};
    double spacing_factor{0.15};
    std::int32_t fade_step{};
    bool stationary_repeat{},mask_enabled{},circle_antialias{},subpixel_coordinates{};
    bool mask_alpha_only{},mask_white_key{};
    std::int32_t mask_arg0{},mask_arg1{},auxiliary_54_raw{};
    std::int32_t writeback_mode_raw{};
    RasterStrokeWriteback writeback{RasterStrokeWriteback::Base};
    RasterStrokeTerminal terminal{RasterStrokeTerminal::Base};
    //Captured native temporary surface; zeros use the destination dimensions
    //and DPI. Copyback scales its rectangle, not the coverage pixels.
    int surface_width{},surface_height{},view_scale_fixed{4096};
    double surface_dpi{},copy_scale{1};
    bool pencil_line_target{};
};

bool raster_stroke_target_eligible(const RasterStrokeTargetContext&,RasterStrokeTool tool=RasterStrokeTool::Brush) noexcept;
// Detached value transformation; failure cannot mutate settings or the target.
// Rejects unsafe numeric inputs, rather than emulating native integer overflow.
RasterStrokeRuntime configure_raster_stroke(const RasterStrokeSettings&,const RasterStrokeTargetContext&);

// Input pressure is already remapped by the app-global pressure curve. Release
// uses a literal zero with either pressure gate, and literal one without them.
double raster_stroke_sample_strength(const RasterStrokeRuntime&,RasterStrokeEvent,double remapped_pressure);
struct RasterStrokeDab {
    double radius_pixels{},strength{1.0},spacing_pixels{};
    std::int32_t hardness{100};
    RasterStrokePattern pattern{RasterStrokePattern::Circle};
    RasterStrokeTerminal terminal{RasterStrokeTerminal::Base};
    bool antialias{};
};
// Consumes the configured pressure, hardness and interval for a sampled dab.
// Gaussian weight/coverage and terminal pixel arithmetic remain kernel-owned.
RasterStrokeDab raster_stroke_dab(const RasterStrokeRuntime&,double sample_strength);
bool raster_stroke_stationary_repeat_due(const RasterStrokeRuntime&,bool same_position,double elapsed_ms) noexcept;

} // namespace retas
