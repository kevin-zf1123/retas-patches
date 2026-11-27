#pragma once
#include "selection.hpp"
#include <stop_token>
#include <string>

namespace retas {
struct DocumentData;
class Raster;
struct SelectWandSettings {
    std::int32_t create_type{},move_image{},close_gap_on{};
    double close_gap_mm{0.5};
    std::int32_t close_gap_px{5},color_margin{};
    bool operator==(const SelectWandSettings&) const = default;
};
struct SelectionWandResult {
    bool handled{true},solved{},cancelled{};
    // Solver-reported bounds, not a recomputed tight selected-bit extent.
    // Public solve returns selection-carrier coordinates and physical size.
    Rect bounds;
    std::shared_ptr<const BinaryMask> mask;
    std::string error;
};
// Native query: visible current owner/plane, owner family excludes 0x40000000,
// plane family excludes 0x200000. Busy/edit-marker fields are not query gates.
bool can_select_wand_native(const DocumentData&) noexcept;
// Canonical seven-depth rasters and certified vector-owner helpers. Native
// mixed-DPI seed scales toward zero; result bits retain carrier translation.
bool can_select_wand(const DocumentData&) noexcept;
// Read-only click solve. Caller owns selection history/combine, with no
// ShrinkType validator (native sub_616490's control argument is literal zero).
SelectionWandResult solve_selection_wand(const DocumentData&,const RegionSelection& carrier,
    Point document_seed,const SelectWandSettings&,bool metric,std::stop_token stop={});
namespace selection_wand_detail {
// Private cross-translation-unit bridge to the shared CSelectWand primitives.
// Work/seed/output are source-raster local. It does not run Bucket setup,
// install IncludeFill lists, apply a live selection, or modify either source.
SelectionWandResult solve_surface(const Raster&,const Raster* companion,Rect work,
    Point seed,int target_mode,int gap_px,int color_margin,std::stop_token stop);
}
}
