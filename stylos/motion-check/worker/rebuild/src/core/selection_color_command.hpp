#pragma once
#include "retas/selection_color.hpp"
#include <algorithm>
namespace retas::selection_color_detail {
enum class BitmapAction { Unchanged, Clear, Refresh };
struct BitmapSelection {RegionSelection selection;BitmapAction action{BitmapAction::Unchanged};};
inline BitmapAction apply_action(const RegionSelection& current,bool add,int page_width,int page_height,Rect request){
    const auto left=std::max<std::int64_t>(request.x,-std::int64_t(current.origin[0]));
    const auto top=std::max<std::int64_t>(request.y,-std::int64_t(current.origin[1]));
    const auto right=std::min<std::int64_t>(std::int64_t(request.x)+request.width,std::int64_t(page_width)-current.origin[0]);
    const auto bottom=std::min<std::int64_t>(std::int64_t(request.y)+request.height,std::int64_t(page_height)-current.origin[1]);
    if(left>=right||top>=bottom)return add?BitmapAction::Unchanged:BitmapAction::Clear;
    if(add&&current.state==SelectionLayerState::FullPlane)return BitmapAction::Unchanged;
    return BitmapAction::Refresh;
}

//The command has already passed674D60 and settled floating. Its execute
//wrapper does not repeat the auxiliary/mode4 query gates on the new target.
BitmapSelection raster_after_admission(const DocumentData&,const RegionSelection&,SelectionColorCommand,Pixel,bool,std::stop_token);
BitmapSelection vector_after_admission(const DocumentData&,const RegionSelection&,SelectionColorCommand,Pixel,bool,bool,std::stop_token);
VectorColorPrimitiveSelection primitives_after_admission(const DocumentData&,bool,const RegionSelection&,SelectionColorCommand,Pixel,bool,std::stop_token);
}
