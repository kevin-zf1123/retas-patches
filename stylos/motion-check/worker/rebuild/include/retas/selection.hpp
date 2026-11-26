#pragma once
#include "geometry.hpp"
#include <array>
#include <functional>
#include <memory>
#include <optional>
#include <stop_token>

namespace retas {
namespace selection_detail {struct MaskView;}
class BinaryMask;
struct Layer;
enum class SelectionLayerState { Cleared=0,FullPlane=1,Rectangle=2,Mask=3,VectorIndicated=4 };
enum class SelectionCombine { Replace=0,Add=1,Subtract=2,Intersect=3 };
enum class PasteSelection { SourceFrame,Content };
enum class SelectionColorCommand { Color,NotColor,ColorAdd,ColorSpace };

// Document-owned runtime, separate from persisted layers and their edit markers.
// Bounds and mask use selection-owner coordinates; origin is the owner's
// document-space point captured by CCelDoItemSelect. Mask storage is immutable
// so copies retain both logical bits and native block ownership across edits.
struct RegionSelection {
    SelectionLayerState state{SelectionLayerState::Cleared};
    Rect bounds;
    std::array<int,2> origin{};
    std::shared_ptr<const BinaryMask> mask;
    // Physical selection carrier, independent of page and selected bounds.
    // {0,0} is an older, unmaterialized snapshot using the document page size.
    std::array<int,2> carrier_size{};
    // Actual native208 indication cache for state4. Separate from the bitmap
    // carrier used by raster/Line predicates; immutable and history-owned.
    std::shared_ptr<const Layer> vector_cache;
    std::array<int,2> vector_cache_origin{};
    //CAppVecSetting raw VecselectType; startup value321, retained verbatim.
    std::uint32_t vector_flags{0x321};
    //Actual offscreen+18 DPI, independent of page/owner DPI. Zero denotes an
    //unmaterialized carrier whose initial DPI comes from its document.
    double carrier_dpi{};
    bool active() const {return state!=SelectionLayerState::Cleared;}
    bool contains_local(int x,int y) const;
    bool contains_document(int x,int y) const;
    Rect document_bounds() const noexcept;
    bool operator==(const RegionSelection&) const = default;
};
RegionSelection rectangle_selection(Rect bounds,std::array<int,2> origin={},std::array<int,2> carrier_size={});
RegionSelection full_selection(int width,int height,std::array<int,2> origin={},std::array<int,2> carrier_size={});
RegionSelection cleared_selection(std::array<int,2> origin={},std::array<int,2> carrier_size={});
std::array<int,2> selection_carrier_size(const RegionSelection&,int page_width,int page_height);
double selection_carrier_dpi(const RegionSelection&,double document_dpi);
// Shared selection move increment (MoveImage=0). Expands the physical carrier
// to retain page coverage using native tile padding; no artwork is modified.
// The caller owns the begin-time selection history and release normalization.
RegionSelection move_selection(const RegionSelection&,int page_width,int page_height,int dx,int dy);
// Native selection-invert worker: flips page coverage in carrier coordinates,
// retains off-page backing outside the old bounds, then normalizes state/bounds.
// Cleared has the worker's FullPlane fallback; public command admission belongs
// to Document. Mask/VectorIndicated require materialized backing. Input stays
// immutable; invalid dimensions, missing backing and coordinate overflow throw.
RegionSelection invert_selection(const RegionSelection&,int page_width,int page_height);
enum class SelectionMorphology { Expand,Shrink };
// Resolve metric distance using the selection carrier's explicit DPI. The
// native millimeter conversion admits a zero-pixel result.
int selection_morphology_radius(double millimeters,double carrier_dpi);
// Standalone SELECT_EXPAND / SELECT_SHRINK, distinct from tool ShrinkType.
// States 0/1 return unchanged. Admitted states retain native physical block
// visits and use vertical then horizontal windows clipped to the work bounds.
// Invalid/unsafe input or an observed stop throws without changing the input.
RegionSelection morph_selection(const RegionSelection&,int page_width,int page_height,
    SelectionMorphology,int radius_px,std::stop_token stop={});
// Immutable target-local coverage. A rectangle converts directly so existing
// prepared-raster callers can retain their explicit rectangular scope.
// The projected bounds are bounded raster coverage, distinct from state-1's
// unrestricted point-hit predicate. Host tools decide whether full-plane
// state bypasses their selection cleanup at all.
class RasterSelection {
    friend struct selection_detail::MaskView;
    Rect bounds_;
    std::shared_ptr<const BinaryMask> mask_;
    std::array<std::int64_t,2> mask_offset_{};
    bool masked_{};
    friend std::optional<RasterSelection> project_selection(const RegionSelection&,
        std::array<int,2>,int,int) noexcept;
public:
    RasterSelection(Rect bounds):bounds_(bounds){}
    const Rect& bounds() const noexcept {return bounds_;}
    bool contains(int x,int y) const noexcept;
    RasterSelection frozen() const;
    // Thin native-mask adapters must reject a masked selection with no backing
    // carrier before planning or writing; rectangle selections are materialized
    // by definition.
    bool has_materialized_mask() const noexcept;
    // Native thin-write predicate: first applies stored logical bounds, then
    // checks a materialized mask when the point lies inside its physical carrier.
    // Points outside that carrier remain unvisited (true); ordinary contains()
    // keeps its exact false-outside-storage semantics for other callers.
    bool native_mask_contains(int x,int y) const noexcept;
    Rect extent(Rect within) const noexcept;
};
// Same-grid integer translation only; the caller checks resolution identity.
// Retains mask ownership without allocating or copying a target-size bitmap.
std::optional<RasterSelection> project_selection(const RegionSelection& selection,
    std::array<int,2> target_origin,int width,int height) noexcept;
// Native direct-mask sub_616490 path only, with no geometry/validation adapter.
// Replace/Add retain request/union bounds even for zero active bits; Subtract
// and Intersect tighten to the resulting extent and clear an empty result.
// Cleared/FullPlane fallbacks are stateful, not generic Boolean set operations.
// Request and incoming bits use carrier-local coordinates. Page admission is
// request intersected with (page - origin), independent of physical storage.
// Add/Subtract/Intersect retain storage outside their operation rectangle,
// including present-zero blocks and stray bits outside stored bounds. They
// do not share Replace's fresh-surface reset. Subtract/Intersect derive
// their new tight bounds from the complete resulting offscreen.
// Materialized Intersect reads the full incoming mask inside old bounds;
// Replace/Add/Subtract gate incoming bits by the admitted request. Clear resets
// state/bounds/origin but preserves carrier size. Masks must match that size.
// Rebuild resource limit: at most 128 Mi carrier pixels, checked before work.
using SelectionMaskValidator = std::function<bool(Rect&,BinaryMask&)>;
RegionSelection combine_selection_mask(const RegionSelection& current,SelectionCombine mode,
    int page_width,int page_height,Rect request,const BinaryMask& incoming,const SelectionMaskValidator& validate={});
}
