#pragma once
#include "retas/fill.hpp"
namespace retas::filter_detail {struct FilterCosts;}
namespace retas::detail {
// Shared PaintMan host Gaussian used by the active tool and public menu.
FillResult gaussian_region(Raster& image,int mode,double radius,Rect bounds,
    const BinaryMask* mask,std::stop_token stop,const std::optional<RasterSelection>& selection={},
    filter_detail::FilterCosts* costs=nullptr,bool retain_backing=false);
}
