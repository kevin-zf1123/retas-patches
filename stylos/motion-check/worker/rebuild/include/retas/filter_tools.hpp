#pragma once
#include "fill.hpp"
#include "filter_tool_settings.hpp"

namespace retas {
// Active BLUR/DUST tools have their own plane gates and operation settings;
// they do not execute the document-wide menu command or update its repeat slot.
bool can_apply_filter_tool(const DocumentData& document,FilterToolKind kind);

// Bounds, mask and dirty use owner pixels. Dust executes marked visible
// canonical raster children in order, without implicit current inclusion. Its
// admission requires a nonbusy target; execution also visits marked busy ones.
// Dust scales ROI/size and returns performed only for positive native damage;
// storage changes and bound-plane pixels_changed can occur without performed.
// Blur executes current/marked siblings with numeric ROI/px radius and mask.
// Both paths stage atomically. This data API does not publish Document history.
FillResult apply_filter_tool(DocumentData& document,FilterToolKind kind,
    const FilterToolSettings& settings,bool use_millimeters,
    const std::optional<Rect>& bounds={},const BinaryMask* mask=nullptr,
    std::stop_token stop={},const std::optional<RasterSelection>& selection={});
}
