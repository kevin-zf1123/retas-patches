#pragma once

#include "document.hpp"
#include "operations.hpp"

#include <array>
#include <cstddef>
#include <compare>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <variant>
#include <vector>

namespace retas {
namespace filter_detail {struct FilterPreviewState;}

// Values are the public Win32 command ids registered by PaintMan 6.6.0.
enum class FilterCommand : std::uint32_t {
    ContrastApply = 40056,
    ContrastSetup = 40057,
    ToneCurveApply = 40058,
    ToneCurveSetup = 40059,
    LevelApply = 40060,
    LevelSetup = 40061,
    Reexecute = 40098,
    SharpHard = 40102,
    SharpSoft = 40103,
    UnsharpMask = 40104,
    BlurHard = 40105,
    BlurSoft = 40106,
    GaussianBlur = 40109,
    AutoContrast = 40110,
    HsvApply = 40111,
    HsvSetup = 40112,
    ColorBalanceApply = 40113,
    ColorBalanceSetup = 40114,
    Dust = 40119,
};

// Apply and setup commands share one backend kind. Reexecute deliberately has
// no kind: the caller must supply the kind and settings from the last commit.
enum class FilterKind {
    Contrast,
    ToneCurve,
    Level,
    AutoContrast,
    SharpHard,
    SharpSoft,
    UnsharpMask,
    BlurHard,
    BlurSoft,
    GaussianBlur,
    Hsv,
    ColorBalance,
    Dust,
    // Native Batch kind11; no ordinary menu/repeat command is registered.
    Invert,
};

// Private internal actions stored by PaintMan for FILTER_REEXECUTE. The three
// dialog-first filters use dedicated direct aliases.
enum class FilterRepeatAction : std::uint32_t {
    Contrast = 0x1388,
    ToneCurve = 0x138a,
    Level = 0x138c,
    AutoContrast = 0x138e,
    SharpSoft = 0x1393,
    SharpHard = 0x1394,
    BlurSoft = 0x1396,
    BlurHard = 0x1397,
    ColorBalance = 0x139a,
    Hsv = 0x139c,
    Dust = 0x13a0,
    UnsharpMask = 0x13a1,
    GaussianBlur = 0x13a2,
};

struct ContrastSettings {
    int contrast{};
    int brightness{};
    auto operator<=>(const ContrastSettings&) const = default;
};

struct LevelChannel {
    int high{255};
    int low{};
    int output_high{255};
    int output_low{};
    double gamma{1.0};
    auto operator<=>(const LevelChannel&) const = default;
};

struct LevelSettings {
    // Master RGB, red, green, blue, in that order.
    std::array<LevelChannel, 4> channels{};
    auto operator<=>(const LevelSettings&) const = default;
};

struct ToneCurvePoint {
    int x{};
    int y{};
    auto operator<=>(const ToneCurvePoint&) const = default;
};

struct ToneCurveRecord {
    std::vector<ToneCurvePoint> points{{0, 0}, {255, 255}};
    auto operator<=>(const ToneCurveRecord&) const = default;
};

struct ToneCurveSettings {
    // 0 = quadratic B-spline, 1 = Bezier.
    // Spline accepts 2..32 points; BYTE Bezier accepts 2..10 loaded points.
    // The standalone WORD Bezier builder accepts 2..4 points.
    int interpolation{};
    // Master RGB, red, green, blue, in that order.
    // Explicit elements retain the throwing vector initialization in the
    // aggregate's exception specification on Clang.
    std::array<ToneCurveRecord, 4> curves{ToneCurveRecord{},ToneCurveRecord{},ToneCurveRecord{},ToneCurveRecord{}};
    auto operator<=>(const ToneCurveSettings&) const = default;
};

struct UnsharpMaskSettings {
    int range{3};
    int contain{3};
    int strength{50};
    auto operator<=>(const UnsharpMaskSettings&) const = default;
};

struct GaussianBlurSettings {
    double size_mm{2.0};
    // Host constructor derives 2 mm at its 50 dpi reference: 4 pixels.
    int size_px{4};
    bool use_millimeters{true};
    auto operator<=>(const GaussianBlurSettings&) const = default;
};

struct HsvSettings {
    double hue{};
    double saturation{};
    double brightness{};
    auto operator<=>(const HsvSettings&) const = default;
};

struct ColorBalanceSettings {
    double red{};
    double green{};
    double blue{};
    // The host persists this flag, but its recovered raster worker does not
    // consult it. It remains part of the settings contract for round trips.
    int keep_luminosity{};
    auto operator<=>(const ColorBalanceSettings&) const = default;
};

enum class DustMode : int {
    Opaque = 0,
    Transparent = 1,
    ExactColor = 2,
};

struct DustSettings {
    DustMode mode{DustMode::Opaque};
    double size_mm{0.5};
    int size_px{5};
    bool use_millimeters{true};
    auto operator<=>(const DustSettings&) const = default;
};

// Native ordinary-filter driver results, separate from stored differences.
enum class DustFilterStatus : int {Rejected=-1,Cancelled=0,Skipped=1,WorkerFailed=2,Applied=0x4c3};
struct DustFilterResult {
    DustFilterStatus status{DustFilterStatus::Skipped};
    std::size_t changed_pixels{};
    bool storage_changed{};
    std::string error;
    bool applied() const {return status==DustFilterStatus::Applied;}
    bool cancelled() const {return status==DustFilterStatus::Cancelled;}
    bool rejected() const {return status==DustFilterStatus::Rejected;}
};
// Ordinary Dust query: current visible owner and an operable marked visible
// child; execution has its separate owner and child gates.
bool can_apply_dust_filter(const DocumentData&);

// Opaque, document-bound private computation. After acquiring the token, a
// run starts from baseline and invalidates previous output; cancelled/rejected
// work leaves it unready. Refused reentry retains the active prepared output.
// cached_preview=false models Apply with preview disabled. Normal Skipped or
// WorkerFailed is a ready result, independently of stored differences. Copies
// share a single-use token. Calls are serialized; join the worker before
// reading ready() or committing. Same-thread reentry is rejected.
class DustFilterPreview {
    using State=filter_detail::FilterPreviewState;
    std::shared_ptr<State> state_;
    explicit DustFilterPreview(std::shared_ptr<State> state):state_(std::move(state)){}
    friend class Document;
public:
    DustFilterPreview()=default;
    DustFilterResult run(const DustSettings&,bool cached_preview=true,std::stop_token stop={});
    bool ready() const noexcept;
};

using FilterSettings = std::variant<std::monostate, ContrastSettings,
                                    ToneCurveSettings, LevelSettings,
                                    UnsharpMaskSettings, GaussianBlurSettings,
                                    HsvSettings, ColorBalanceSettings,
                                    DustSettings>;

using FilterDriverStatus=DustFilterStatus;
using FilterDriverResult=DustFilterResult;
// Native ordinary query for the implemented Document filter drivers.
bool can_apply_document_filter(const DocumentData&,FilterKind);
// Execution admission is separate from the native command's query admission.
bool document_filter_executes_on_layer(FilterKind,std::uint32_t family);
class FilterPreview {
    std::shared_ptr<filter_detail::FilterPreviewState> state_;
    explicit FilterPreview(std::shared_ptr<filter_detail::FilterPreviewState> state):state_(std::move(state)){}
    friend class Document;
public:
    FilterPreview()=default;
    // Same single-use, serialized ownership rules as DustFilterPreview.
    FilterDriverResult run(const FilterSettings&,bool cached_preview=true,std::stop_token stop={});
    bool ready() const noexcept;
    // Cumulative modern storage/work ledger, including accepted ROI history.
    std::array<std::uint64_t,2> resources()const noexcept;
};

enum class FilterStatus {
    Applied,
    Unchanged,
    Cancelled,
    Rejected,
};

struct FilterResult {
    FilterStatus status{FilterStatus::Unchanged};
    // Applied may have no logical pixel change. Ordinary Dust also reports
    // native worker success when a nonempty scan leaves all backing unchanged.
    std::size_t changed_pixels{};
    std::string error;

    bool modified() const { return status == FilterStatus::Applied; }
    bool cancelled() const { return status == FilterStatus::Cancelled; }
    bool rejected() const { return status == FilterStatus::Rejected; }
};

struct FilterExecutionOptions {
    // HSV, ColorBalance and RGBA Unsharp map stored RGB and preserve alpha
    // independently of both flags.
    // Plane mode 5 extends the RGBA transparency predicate to exact white;
    // the native RGB24 table worker only tests the fourth physical byte.
    // This is runtime context and is intentionally not serialized in settings.
    bool white_is_transparent{};
    // Plane mode 4 remaps the RGBA coverage lane through the first table.
    // RGB24 remaps the next physical byte, which may be RGB or padding.
    bool coverage_only{};
};

std::optional<FilterKind> filter_kind_for_command(FilterCommand command);
bool is_setup_command(FilterCommand command);
bool is_interactive_command(FilterCommand command);

FilterRepeatAction canonical_repeat_action(FilterKind kind);
FilterSettings default_filter_settings(FilterKind kind);
bool settings_match_filter(FilterKind kind, const FilterSettings& settings);

// Gaussian's menu region is transient and uses document coordinates; it is
// not stored in provider settings or in the remembered repeat command.
// It writes only the current visible plane. At RGBA depth the document region
// and target-local selection gate output only; source halo remains available.
// Selected execution requires matching document/layer/plane DPI; no region
// means the full plane.
// The menu caller independently clips its work region to the document page.
// Canonical depths below RGBA preserve content and retain the native final
// Copy footprint, including typed missing edge blocks and the 1/2-bit tail.
// Their copyback ignores actual coverage bits; empty clipped bounds are a no-op.
FilterResult apply_gaussian_filter(DocumentData& document,const GaussianBlurSettings& settings,
    const std::optional<Rect>& document_selection={},std::stop_token stop={},
    const std::optional<RasterSelection>& raster_selection={});

// All overloads are transactional: cancellation, invalid settings, malformed
// tiles, and unsupported targets leave the input object unchanged.
// Fixed Sharp/Blur presets accept canonical 1/2/8/16-bit content no-ops and
// RGB24 identity copyback with top-left white materialization, even for empty
// output bounds. An admitted RGB24 fourth-byte access outside a tile rejects.
// Unsharp accepts all canonical depths: below RGBA it preserves content and
// materializes typed top-left backing for nonempty bounds. RGBA uses its
// depth-specific native arithmetic and selected output backing policy.
// Dust accepts all seven canonical depths. Ordered source/neighbor probes can
// materialize backing even at size0 or after a component is rejected; actual
// mask cleanup precedes typed voting and fill. A normally completed nonempty
// scan is Applied even without byte changes or positive component damage;
// empty clipped bounds are Unchanged. See filter-dust-contract.md.
FilterResult apply_filter(Raster& raster, FilterKind kind,
                          const FilterSettings& settings,
                          std::stop_token stop = {});
FilterResult apply_filter(Raster& raster, FilterKind kind,
                          const FilterSettings& settings,
                          const FilterExecutionOptions& options,
                          std::stop_token stop = {});
FilterResult apply_filter(DocumentData& document, FilterKind kind,
                          const FilterSettings& settings,
                          std::stop_token stop = {});

// Transient output selection, separate from persisted/repeat settings. Raster
// coordinates are target-local; Document rectangles are document coordinates
// and coverage is owner-local. Source neighbors and AutoContrast statistics
// remain whole-image. Dust instead gates components before choosing neighbors.
// Document selection mapping currently requires matching document/owner/plane
// DPI for every consumed target; unsupported mappings reject atomically.
FilterResult apply_filter_region(Raster& raster, FilterKind kind,
    const FilterSettings& settings, Rect bounds,
    const FilterExecutionOptions& options = {}, std::stop_token stop = {},
    const std::optional<RasterSelection>& coverage = {});
FilterResult apply_filter_region(DocumentData& document, FilterKind kind,
    const FilterSettings& settings, const std::optional<Rect>& document_selection,
    std::stop_token stop = {}, const std::optional<RasterSelection>& coverage = {});

} // namespace retas
