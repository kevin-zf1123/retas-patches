#pragma once
#include "retas/selection.hpp"
namespace retas::selection_detail {
//Native same-coordinate Copy1, including physical present-zero blocks and
//partial-width terminal cursor ownership. No page/controller clipping.
void copy_mask_rectangle(BinaryMask& destination,const BinaryMask& source,Rect request,std::stop_token stop={});
//616490 direct mask path with independently sized source/target carriers.
//Operation bounds retain page admission; Copy clips both physical surfaces.
RegionSelection combine_mask_source(const RegionSelection&,SelectionCombine,int page_width,int page_height,
    Rect request,const BinaryMask&,const SelectionMaskValidator& validate={});
}
