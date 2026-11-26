#pragma once
#include <array>
#include <cmath>
#include <stdexcept>

namespace retas::raster_stroke_detail {
// CTempPenLineOp same-DPI mapping: floor into 1/16-pixel TempLine
// coordinates, then truncate at the one-bit renderer's scale of sixteen.
inline double temp_grid_coordinate(double local){
    if(!std::isfinite(local)||std::abs(local)>10000000)
        throw std::invalid_argument("Temporary stroke coordinate exceeds the supported domain");
    return std::trunc(std::floor(local*16.0)/16.0);
}
// Internal integer span primitive shared by color and one-bit stroke sinks.
// Callers validate diameter >= 1 and 0 <= row < diameter.
// rcwCmn CircleFill small-diameter table, followed by its analytic row spans.
inline std::array<int,2> circle_span(int origin,int diameter,int row){
    static constexpr int spans[4][4][2]={{{0,0}},{{0,1},{0,1}},
        {{1,1},{0,2},{1,1}},{{1,2},{0,3},{0,3},{1,2}}};
    if(diameter<=4)return {origin+spans[diameter-1][row][0],origin+spans[diameter-1][row][1]};
    const double r=diameter*0.5,mid=origin+r,dy=r-0.5-row;
    const int left=int(mid-std::sqrt(r*r-dy*dy));
    return {left,2*int(mid)-left-(diameter%2==0?1:0)};
}
}
