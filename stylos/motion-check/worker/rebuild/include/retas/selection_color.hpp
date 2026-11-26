#pragma once
#include "selection.hpp"
#include "raster.hpp"
#include "vector_indication.hpp"

namespace retas {
struct DocumentData;
// Native raster admission: owner/plane visibility, auxiliary-owner exclusion,
// and ColorSpace's mode4 exclusion. Busy/selected flags do not participate.
bool can_select_raster_color(const DocumentData&,SelectionColorCommand) noexcept;
// Scan page minus owner origin without seed, tolerance or DPI resampling.
// ColorAdd combines by Add; ColorSpace uses the explicit additive modifier.
// Other commands Replace. Byte targets select reference's WORD high bytes.
// Invalid storage, resource exhaustion or an observed stop throws. Inputs
// remain unchanged; vector color selection belongs to a separate path.
RegionSelection select_raster_color(const DocumentData&,const RegionSelection&,
    SelectionColorCommand,Pixel reference,bool additive_space=false,std::stop_token stop={});
// Explicit VecselectType bit0 bitmap path for current Line/Vert-Line/Face.
// Saved per-object bounds use owner/child DPI; geometry uses raw scale16.
// The global minimum-width preference is captured by the caller.
bool can_select_vector_color_bitmap(const DocumentData&,SelectionColorCommand) noexcept;
RegionSelection select_vector_color_bitmap(const DocumentData&,const RegionSelection&,
    SelectionColorCommand,Pixel reference,bool additive_space=false,bool minimum_width=false,std::stop_token stop={});
struct VectorColorPrimitiveSelection {
    VectorIndicationUpdate indications;
    RegionSelection selection;
};
// Explicit VecselectType bit0-off worker. Color/NotColor Replace clears the
// selection and gated old indications; ColorAdd and additive ColorSpace retain
// the complete selection. Admitted work sets Doc+284/redraw even without hits.
// It marks current-plane LC members or Faces directly, without geometry, page
// sampling or a trailing indication refresh. The caller owns publication/history.
bool can_select_vector_color_primitives(const DocumentData&,SelectionColorCommand) noexcept;
VectorColorPrimitiveSelection prepare_vector_color_primitive_selection(const DocumentData&,bool indication_active,
    const RegionSelection&,SelectionColorCommand,Pixel reference,bool additive_space=false,std::stop_token stop={});
}
