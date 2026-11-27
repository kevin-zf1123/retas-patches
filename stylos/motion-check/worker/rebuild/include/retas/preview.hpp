#pragma once
#include "floating.hpp"
#include <stdexcept>

namespace retas {
// A caller may retain an older readable-format display path for this error.
// Invalid storage, allocation failures and work limits must not be swallowed.
class UnsupportedPreview : public std::invalid_argument {
public:
    using std::invalid_argument::invalid_argument;
};
// Native main/helper cache RGB stores the remaining complement of white.
// It is neither straight nor premultiplied RGBA. Do not pass it to retas::over
// and convert against an explicit background only when producing display.
struct PreviewPixel {
    std::uint8_t r{255},g{255},b{255},a{};
    bool operator==(const PreviewPixel&) const = default;
};
// Treat source channels as an ordinary raster sample, even when the source
// was itself a flattened kind1 helper. Foreground and result are cache values.
PreviewPixel preview_behind(PreviewPixel foreground,PreviewPixel source) noexcept;
// Unlike an ordinary raster source, an already accumulated LightTable set
// carries alpha in its RGB complement. Do not attenuate that RGB again.
PreviewPixel preview_cache_behind(PreviewPixel foreground,PreviewPixel cache) noexcept;
Pixel preview_display(PreviewPixel cache,Pixel opaque_background=Pixel::rgba8(255,255,255)) noexcept;
Pixel preview_color_check(PreviewPixel cache,bool preserve_primary,bool paint_mono_present) noexcept;

enum class PreviewReferenceMode {Color,Monotone,Halftone};
struct PreviewReferenceOptions {
    // rtMulDiv(row percent, set percent, 100), before the separate 256 scale.
    int effective_opacity_percent{100};
    bool see_through{true};
    PreviewReferenceMode mode{PreviewReferenceMode::Color};
    Pixel monotone_color{Pixel::rgba8(0,0,0)};
    // For adapters retaining an already mapped row cache. Keep mode because
    // the non-SeeThrough mapped-color branch still has a different weight.
    bool color_mapping_applied{};
};
// A prepared 32-bit row-cache sample, before the row's color/opacity mapping.
// Geometry and preparing the reference cache are separate caller operations.
PreviewPixel preview_reference_behind(PreviewPixel foreground,PreviewPixel sample,PreviewReferenceOptions options={});

// Pixels are row-major within the document-coordinate bounds. A damage
// render uses exactly the same kernels as a full render, with no callbacks
// or document/history mutation while its candidate is being constructed.
class PreviewImage {
    Rect bounds_;
    std::vector<PreviewPixel> pixels_;
public:
    explicit PreviewImage(Rect bounds);
    Rect bounds() const noexcept{return bounds_;}
    PreviewPixel pixel(int document_x,int document_y) const noexcept;
    void set_pixel(int document_x,int document_y,PreviewPixel value) noexcept;
    const std::vector<PreviewPixel>& pixels() const noexcept{return pixels_;}
};
struct PreviewOptions {
    // Native LAYP plane-view mode. Default rendering enumerates all planes;
    // restricted raster mode draws the current layer's active mode-4 mask.
    // Ordinary Text/Indication still draws every visible child in color.
    // A live PaintMono helper uses its separate family-insertion path.
    bool plane_mode{};
    bool force_density{};
    std::optional<Pixel> background;
    // Unrotated ColorCheck excludes auxiliary document layers before CRender,
    // retaining the live floating helper and original source-map indices.
    bool color_check{};
    // Reference-source flattening visits the whole visible stack. Main-cache
    // opacity-mode termination belongs only to the main renderer.
    bool reference_source{};
    // Source preparation supplies the family-filtered single-copy decision.
    std::optional<bool> reference_direct_copy;
    // Native440500 MotionCheck request: AA2 Line output, explicit
    // background handled by the caller, production paint visibility and no
    // main-preview cutoff. These flags do not change the ordinary preview.
    bool motion_check{},motion_show_shadow{},motion_only_line{};
    bool stylos_score_pixels{};
    // Resources retained by MotionCheck or an explicit LightTable font source.
    std::array<std::uint64_t,2> resources{};
    // Motion's independent positive-placement Text consumer. It rebuilds
    // private masks from raw payload, ignoring all external display caches.
    bool motion_qt_text_fonts{};
    std::stop_token motion_stop;
    // LightTable's positive-placement Text source keeps its own root copy and
    // normalization decision. It shares mask generation, not Motion's finish.
    // Its bounded vector caches/redraws share costs and the root-copy terminal.
    bool reference_qt_text_fonts{};
    std::stop_token reference_stop;
    //611D80's nonzero quality sets440500 flag200 for direct vector redraws.
    bool reference_vector_aa2{};
    bool minimum_line_width{};
};
class FloatingPreview;
std::shared_ptr<const FloatingPreview> prepare_floating_preview(const DocumentData&,const FloatingSelection&,PreviewOptions options={});
// Native paste helper-to-selection path. Uses primary alpha, or the union of
// both PaintMono helpers with white-key transparency. Retains carrier size,
// resets origin/indication, and preserves visited empty mask blocks when the
// result has selected content. Requires a normalized foreign paste at delta0.
RegionSelection prepare_paste_content_selection(const DocumentData&,const FloatingSelection&,
    const RegionSelection& carrier={});
// Immutable source snapshots keep a render independent of later document or
// history publication. A prepared floating helper may be reused across moves.
// Construction is serialized with document writes; snapshots rely on Raster
// COW mutators and do not freeze externally retained raw tile-byte aliases.
class PreviewScene {
    struct Impl;
    std::shared_ptr<const Impl> impl_;
public:
    PreviewScene(const DocumentData&,const FloatingSelection* floating=nullptr,
                 std::shared_ptr<const FloatingPreview> prepared={},PreviewOptions options={});
    Rect page_bounds() const noexcept;
    Rect floating_bounds() const noexcept;
    bool reference_source_direct() const noexcept;
    void publish_vector_cache_extents(DocumentData&) const noexcept;
    PreviewImage render(Rect document_region) const;
    // LightTable belongs between document content and the native background.
    // These stages allow a view to insert its independent reference set while
    // retaining the same full/damage kernels and pre-ColorCheck sample cache.
    PreviewImage render_layers(Rect document_region) const;
    // Returns cumulative preparation/render admission, including the
    // image-source resource seed. Allows multiple Score columns to share one
    // budget; the immutable scene keeps no per-render mutable counters.
    PreviewImage render_layers(Rect document_region,
        std::array<std::uint64_t,2>& resource_admission) const;
    void finish_background(PreviewImage&) const noexcept;
};
}
