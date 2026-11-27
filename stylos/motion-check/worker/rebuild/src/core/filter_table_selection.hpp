#pragma once
#include "retas/document.hpp"
#include "retas/fill.hpp"
#include <algorithm>
#include <climits>
#include <cmath>

namespace retas::filter_detail {
// The table/color workers intersect the unscaled carrier rectangle, then test inverse
// Common-DPI coordinates. Raster and carrier DPI do not enter this predicate.
class TableSelection {
    const RegionSelection& selection_;
    Rect roi_;
    std::int64_t dx_,dy_;
    double inverse_;
    int mapped(int value)const{
        const auto v=double(value)*inverse_;
        if(!std::isfinite(v)||v>INT32_MAX)throw std::invalid_argument("Table filter selection mapping exceeds int32");
        return int(v);
    }
public:
    TableSelection(const Plane& plane,const Layer& owner,const RegionSelection& selection,bool maps_pixels=true)
        :selection_(selection),roi_{0,0,plane.raster->width,plane.raster->height},
        dx_(std::int64_t(selection.origin[0])-owner.common.x),dy_(std::int64_t(selection.origin[1])-owner.common.y),
        inverse_(owner.common.resolution/plane.common.resolution){
        if(int(selection.state)<2)return;
        const auto x=std::int64_t(selection.bounds.x)+dx_,y=std::int64_t(selection.bounds.y)+dy_;
        const auto left=std::clamp<std::int64_t>(x,0,roi_.width),top=std::clamp<std::int64_t>(y,0,roi_.height);
        const auto right=std::clamp<std::int64_t>(x+std::max(0,selection.bounds.width),left,roi_.width);
        const auto bottom=std::clamp<std::int64_t>(y+std::max(0,selection.bounds.height),top,roi_.height);
        roi_={int(left),int(top),int(right-left),int(bottom-top)};
        if(maps_pixels&&!empty()){(void)mapped(plane.raster->width-1);(void)mapped(plane.raster->height-1);}
    }
    bool empty()const{return !roi_.width||!roi_.height;}
    bool contains(int x,int y)const{
        if(int(selection_.state)<2)return true;
        const auto qx=mapped(x),qy=mapped(y);if(!roi_.contains(qx,qy))return false;
        if(int(selection_.state)<3)return true;
        const auto mx=std::int64_t(qx)-dx_,my=std::int64_t(qy)-dy_;
        return selection_.mask&&mx>=0&&my>=0&&mx<selection_.mask->width()&&my<selection_.mask->height()
            &&selection_.mask->test(int(mx),int(my));
    }
};
}
