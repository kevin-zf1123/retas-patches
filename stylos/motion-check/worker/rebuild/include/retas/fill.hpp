#pragma once

#include "operations.hpp"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace retas {

// Stored values intentionally match PaintMan's IncludeFill profile integers.
// Value 2 is a protected-colour exclusion policy only for the normal raster
// bucket path.  Close Fill and vector fill have different contracts.
enum class IncludeFillMode : std::int32_t {
    None = 0,
    SpecifiedColors = 1,
    ExceptSpecifiedColors = 2,
};

// Original opaque B/G/R constants in canonical RGBA order.
std::array<Pixel, 3> fixed_include_colors();

struct IncludeFillSettings {
    IncludeFillMode mode{IncludeFillMode::SpecifiedColors};
    std::int32_t whole_vector{};
    std::array<std::int32_t, 6> enabled{};
    std::array<Pixel, 3> custom_colors{
        Pixel{65535, 65535, 65535, 65535},
        Pixel{65535, 65535, 65535, 65535},
        Pixel{65535, 65535, 65535, 65535},
    };
    // Rebuild extension: the original first three slots are now editable.
    // Keep the three legacy CustomColor values and their indices unchanged.
    std::array<Pixel, 3> primary_colors{fixed_include_colors()};
    Pixel& color(std::size_t index) {
        return index < 3 ? primary_colors.at(index) : custom_colors.at(index - 3);
    }
    const Pixel& color(std::size_t index) const {
        return index < 3 ? primary_colors.at(index) : custom_colors.at(index - 3);
    }
    auto operator<=>(const IncludeFillSettings&) const = default;
};

struct BucketFillSettings {
    IncludeFillSettings include;
    std::int32_t close_gap_on{};
    double close_gap_distance_mm{0.5};
    std::int32_t close_gap_distance_px{5};
    std::int32_t color_margin{};
    std::int32_t use_light_line{};
    std::int32_t use_light_color{};
    std::int32_t stop_if_leaked{};
    std::int32_t fill_separated{};
    auto operator<=>(const BucketFillSettings&) const = default;
};

// Slot 8 is persisted beside the bucket banks, but is not executed by the
// raster bucket kernel below.  Keeping it distinct prevents a normal flood
// from being presented as PaintMan's rectangle-scoped CLFL solver.
struct CloseFillSettings {
    IncludeFillSettings include{
        IncludeFillMode::None,
        0,
        {},
        {Pixel{65535, 65535, 65535, 65535},
         Pixel{65535, 65535, 65535, 65535},
         Pixel{65535, 65535, 65535, 65535}},
    };
    std::int32_t pen_type{};
    double pen_width_mm{3.0};
    std::int32_t pen_width_px{30};
    std::int32_t pressure_on{};
    std::int32_t sync_view_scale{};
    std::int32_t only_transparent{};
    auto operator<=>(const CloseFillSettings&) const = default;
};

struct SpreadFillSettings {
    double pen_width_mm{3.0};
    std::int32_t pen_width_px{30};
    std::int32_t effect_choice{2};
    std::int32_t drag_closed_area{};
    auto operator<=>(const SpreadFillSettings&) const = default;
};

struct FillSettings {
    std::array<BucketFillSettings, 2> bucket_banks{};
    std::int32_t current_set{};
    // Global PrefInfo::UseMmForTool selector. Both stored value channels stay
    // intact; the host resolves only the selected channel for each execution.
    std::int32_t use_mm_for_tools{};
    // Global PrefInfo::UndoTime, in seconds; retained raw, checked at use time.
    double undo_time_seconds{0.2};
    CloseFillSettings close_fill;
    SpreadFillSettings spread_fill;

    BucketFillSettings& active_bucket();
    const BucketFillSettings& active_bucket() const;
    auto operator<=>(const FillSettings&) const = default;
};

struct FillResult {
    // `handled` describes whether the requested worker accepted the target.
    // `changed` describes actual pixel changes. Vector workers retain saved
    // Region damage and performed history even for same-valued Face writes.
    bool handled{true};
    bool changed{};
    bool cancelled{};
    bool leaked{};
    Rect dirty{};
    std::size_t changed_pixels{};
    std::string error;
    // Native cursor traversal can materialize backing without changing a
    // logical pixel. Keep that publication distinct from pixel damage.
    bool storage_changed{};
    // A nonempty Close Fill solution performs even for same-color writes or
    // native no-write leaves; dirty then records the solved mask's bounds.
    bool performed{};
};

struct FillReference {
    const Raster* raster{};
    // target-local (x,y) -> reference-local (m11*x + m12*y + dx,
    //                                        m21*x + m22*y + dy)
    std::array<double, 6> target_to_reference{1.0, 0.0, 0.0,
                                              0.0, 1.0, 0.0};
    bool white_is_empty{};
};

// Borrowed immutable native vector source. Translation uses target-local raw16.
struct FillVectorReference {
    const Layer* owner{};
    std::array<std::int32_t,2> source_to_target_translation{};
    bool enabled{true};
};

struct FillReferences {
    std::optional<FillReference> light_line;
    std::optional<FillReference> light_color;
    std::vector<FillVectorReference> vector_light_lines;
};

// A caller-prepared target-sized one-bit gesture surface in active-plane-local
// integer coordinates. Coordinates are half-open; out-of-range reads are
// clear and out-of-range writes are ignored. CLFL and SPFL consume the
// original tool's scratch mask; they deliberately do not invent a polygon or
// brush rasterizer when the corresponding host gesture contract is unknown.
class BinaryMask {
    int width_{};
    int height_{};
    std::size_t stride_{};
    std::vector<std::uint32_t> words_;
    // Native one-bit offscreens distinguish absent blocks from allocated
    // blocks containing only zero bits. Keep that ownership separate from
    // dense logical storage; set/clear-bit remain allocation-free.
    std::vector<std::uint8_t> blocks_;
    std::uint64_t mutation_generation_{};

public:
    BinaryMask() = default;
    BinaryMask(int width, int height);
    BinaryMask(const BinaryMask&);
    BinaryMask& operator=(const BinaryMask&);
    BinaryMask(BinaryMask&&) noexcept;
    BinaryMask& operator=(BinaryMask&&) noexcept;
    // Per-object runtime identity for allocation-free capture checks. Copies
    // start an independent generation; writes and replacement advance it.
    // This value is neither serialized nor a cross-thread synchronization API.
    std::uint64_t mutation_generation() const noexcept { return mutation_generation_; }
    int width() const { return width_; }
    int height() const { return height_; }
    bool contains(int x, int y) const;
    bool test(int x, int y) const;
    // Tight selected-bit extent inside a half-open region. Scans bounded
    // words without allocating a second bitmap or iterating every pixel.
    Rect extent(Rect region) const noexcept;
    void set(int x, int y, bool value = true);
    // Coordinates here name physical 256x256 blocks. Materializing a block
    // does not select any pixels; clearing a bit does not release its block.
    bool block_present(int x,int y) const noexcept;
    void materialize_block(int x,int y) noexcept;
    void release_block(int x,int y) noexcept;
    // Scan complete logical blocks touched by region, including pixels
    // outside region. Empty blocks are released, not merely filled with zero.
    void optimize_blocks(Rect region) noexcept;
    // Destroy all backing as well as clearing logical pixels.
    void clear();
};

// `line` is an aligned line-evidence raster.  Opaque black is always treated
// as main-line evidence; selected coloured evidence participates in
// IncludeFill. FillSeparated ignores all line evidence. The raw-raster entry consumes close_gap_distance_px as an
// already resolved target-pixel radius; metric/global unit lowering belongs
// to fill_document. Mutation is deferred until solving finishes, so
// cancellation leaves the raster unchanged. RasterSelection supports an immutable
// translated mask; normal flood treats holes as barriers, separated fill as
// per-pixel vetoes. This lower entry retains the explicit leak-stop flag.
FillResult fill_raster(Raster& target,
                       Point seed,
                       Pixel color,
                       const BucketFillSettings& settings,
                       const Raster* line = nullptr,
                       const std::optional<RasterSelection>& selection = {},
                       std::stop_token stop = {},
                       const FillReferences& references = {});

// The seed is active-plane-local.  When the active plane is a raster paint
// plane, line siblings share its owner-local raster origin. Stored plane
// coordinates are metadata; siblings are protected from paint-plane writes,
// except in FillSeparated mode. A supplied selection disables the live host
// StopIfLeaked policy without changing the saved settings.
FillResult fill_document(DocumentData& document,
                         Point seed,
                         Pixel color,
                         const FillSettings& settings,
                         const std::optional<RasterSelection>& selection = {},
                         std::stop_token stop = {},
                         const FillReferences& references = {});

// Ordinary vector Bucket: seed retains subpixel precision until truncation
// to the native raw16 POINT. Consume the actual selection/cache, not a
// projected raster rectangle. Changes are staged before publication.
FillResult fill_vector_document(DocumentData&,Point,Pixel,const FillSettings&,
    const RegionSelection& selection={},std::stop_token stop={},const FillReferences& references={});

// BFIL-only normal Bucket entry. The caller temporarily binds its last
// matching target plane and supplies owner-local integral seeds and the final
// row/LightColor-resolved raw color. UseLightColor is inactive at this lower
// worker boundary. There is no RegionSelection argument: native69DB30 passes
// a null selection source. The ordinary same-DPI first line companion may be
// depth1; its source factory uses a plain black/transmit RCVOffscreen1Bit.
// Its seed must have alpha0; bulk companion pixels use alpha0-or-white as a
// hard gate before IncludeFill. LightLine only applies for target seed alpha0
// and compares raw32 reference colors with the cached reference seed.
// It ignores StopIfLeaked/FillSeparated, uses high-byte32/raw-word64 colors,
// writes same-valued selected pixels, and materializes every physical tile in
// the solver's reported scan rectangle, including holes. No alpha optimizer
// runs here. pixels.changed_pixels remains a difference count; performed/scan
// report the native successful solver/writeback boundary independently.
// Modern bounded normal same-DPI32/64 admission and512MiB/work caps apply.
struct ConsecutiveBucketResult {FillResult pixels;bool performed{},storage_changed{};Rect scan;};
ConsecutiveBucketResult fill_consecutive_document(DocumentData& document,
    Point seed,Pixel raw_color,const FillSettings& settings,
    std::stop_token stop={},const FillReferences& references={});

// Direct Close Fill (CLFL) consumes a target-sized one-bit request/candidate
// surface, not a line-evidence image. request_bounds is an active-plane-local
// half-open rectangle and is expanded by one pixel inside the solver. Policy 2
// intentionally follows the original no-list path rather than Bucket's
// protected-colour exclusion policy. Selection first cleans request coverage;
// IncludeFill support may subsequently regenerate cleared bits. It is not a
// final write clip, and does not narrow the original work/support bounds.
FillResult close_fill_raster(Raster& target,
                             const BinaryMask& request,
                             Rect request_bounds,
                             Pixel color,
                             const CloseFillSettings& settings,
                             int target_mode = 1,
                             const Raster* companion = nullptr,
                             bool structural_black_guard = false,
                             const std::optional<RasterSelection>& selection = {},
                             std::stop_token stop = {});

FillResult close_fill_document(DocumentData& document,
                               const BinaryMask& request,
                               Rect request_bounds,
                               Pixel color,
                               const CloseFillSettings& settings,
                               const std::optional<RasterSelection>& selection = {},
                               std::stop_token stop = {});

FillResult close_fill_vector_document(DocumentData&,const BinaryMask&,Rect,Pixel,
    const CloseFillSettings&,const std::optional<RasterSelection>& selection={},
    std::stop_token stop={},bool rectangle_scope=false);

// Face identity is frozen at SPFL press; segment writes copy its complete
// same-plane material, including gradient/texture ownership backlinks.
std::optional<int> capture_vector_spread_fill_face(const DocumentData&,Point,std::stop_token stop={});
FillResult spread_fill_vector_document(DocumentData&,const BinaryMask&,Rect,int sampled_face,
    const SpreadFillSettings&,const std::optional<RasterSelection>& selection={},std::stop_token stop={});

// SPFL's exact raster worker also starts after gesture rasterization. Set bits
// are the current width-controlled segment after any AreaMask cleanup.
// work_bounds is the caller-expanded (`width + 2`) active-plane-local half-open
// rectangle; this kernel only clips it. The occupancy snapshot is not updated
// while accepted pixels are written. sampled_color is the encoded target
// sample locked at pointer-down, not the current foreground color. Selection
// vetoes candidate writes but does not clip the occupancy snapshot bounds.
FillResult spread_fill_raster(Raster& target,
                              const BinaryMask& scratch,
                              Rect work_bounds,
                              Pixel sampled_color,
                              const SpreadFillSettings& settings,
                              int target_mode = 1,
                              const Raster* companion = nullptr,
                              const std::optional<RasterSelection>& selection = {},
                              std::stop_token stop = {});

FillResult spread_fill_document(DocumentData& document,
                                const BinaryMask& scratch,
                                Rect work_bounds,
                                Pixel sampled_color,
                                const SpreadFillSettings& settings,
                                const std::optional<RasterSelection>& selection = {},
                                std::stop_token stop = {});

} // namespace retas
