#include "retas/document.hpp"
#include "retas/numeric.hpp"
#include "overlay_lengths.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace retas {
namespace overlay_detail {
using numeric_detail::binary64;
double millimeters(const Length& length){
    if(!std::isfinite(length.value))throw std::invalid_argument("网格长度无效。");
    double result{};
    // rcwCmn RCLength::GetAs(2), 0x10522965. Preserve each operation order.
    switch(length.unit){
    case 0:
        if(!std::isfinite(length.reference)||length.reference<=0)throw std::invalid_argument("网格参考分辨率无效。");
        result=binary64(binary64(length.value/length.reference)*25.4);break;
    case 1:result=binary64(length.value*10);break;
    case 2:result=length.value;break;
    case 3:result=binary64(length.value*25.4);break;
    case 4:result=binary64(length.value*152.4);break;
    case 5:result=binary64(length.value*0.3527777777777777);break;
    case 6:result=binary64(length.value*0.25);break;
    default:throw std::invalid_argument("网格长度单位尚未支持。");
    }
    if(!std::isfinite(result))throw std::invalid_argument("网格长度超出支持范围。");
    return result;
}
int integer(std::int64_t value){
    if(value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())
        throw std::invalid_argument("网格移动坐标超出支持范围。");
    return static_cast<int>(value);
}
double inches(const Length& length){
    if(!std::isfinite(length.value))throw std::invalid_argument("参考线长度无效。");
    double value{};
    // rcwCmn RCLength::GetAs(3), 0x105229BD. These direct conversions have
    // different last-bit rounding from converting through millimeters.
    switch(length.unit){
    case 0:
        if(!std::isfinite(length.reference)||length.reference<=0)throw std::invalid_argument("参考线分辨率无效。");
        value=binary64(length.value/length.reference);break;
    case 1:value=binary64(length.value*0.3937007874015748);break;
    case 2:value=binary64(length.value*0.03937007874015748);break;
    case 3:value=length.value;break;
    case 4:value=binary64(length.value*6.0);break;
    case 5:value=binary64(length.value*0.01388888888888889);break;
    case 6:value=binary64(length.value*0.00984251968503937);break;
    default:throw std::invalid_argument("参考线长度单位尚未支持。");
    }
    if(!std::isfinite(value))throw std::invalid_argument("参考线长度超出支持范围。");
    return value;
}
int pixels(const Length& length,double dpi){
    if(!std::isfinite(dpi)||dpi<=0)throw std::invalid_argument("网格分辨率无效。");
    double value{};
    if(length.unit==0){
        if(!std::isfinite(length.reference)||length.reference<=0)throw std::invalid_argument("网格参考分辨率无效。");
        value=binary64(binary64(length.value/length.reference)*dpi);
    }else{
        // The live Grid starts in mm, as do move/spacing normalization paths.
        // Keep direct unit conversions for user-entered settings.
        switch(length.unit){
        case 1:value=binary64(binary64(dpi*length.value)*0.3937007874015748);break;
        case 2:value=binary64(binary64(dpi*length.value)*0.03937007874015748);break;
        case 3:value=binary64(dpi*length.value);break;
        // Native GetAsPixel deliberately routes its valid unit-4 slot to
        // the zero-return branch, despite GetAs(3) converting it to inches.
        case 4:return 0;
        case 5:value=binary64(binary64(dpi*length.value)*0.01388888888888889);break;
        case 6:value=binary64(binary64(dpi*length.value)*0.00984251968503937);break;
        default:throw std::invalid_argument("网格间隔单位尚未支持。");
        }
    }
    const auto result=legacy_rt_double_to_int(value);
    if(!result)throw std::invalid_argument("网格坐标超出支持范围。");
    return *result;
}
}
using namespace overlay_detail;
GridState make_grid(double dpi){
    if(!std::isfinite(dpi)||dpi<=0)throw std::invalid_argument("网格分辨率无效。");
    GridState grid;grid.dpi=dpi;grid.position={Length{0,2,dpi},Length{0,2,dpi}};grid.spacing.reference=dpi;return grid;
}
std::array<Length,2> grid_anchor_position(const BaseInfo& page,GridAnchor anchor){
    const double width=millimeters(page.lengths[0]),height=millimeters(page.lengths[1]);
    if(!std::isfinite(page.dpi)||page.dpi<=0||width<=0||height<=0)throw std::invalid_argument("网格页面尺寸无效。");
    double x{},y{};
    switch(anchor){
    case GridAnchor::TopLeft:break;
    case GridAnchor::TopRight:x=width;break;
    case GridAnchor::BottomLeft:y=height;break;
    case GridAnchor::BottomRight:x=width;y=height;break;
    case GridAnchor::Center:x=binary64(width*0.5);y=binary64(height*0.5);break;
    default:throw std::invalid_argument("网格原点位置无效。");
    }
    return {Length{x,2,page.dpi},Length{y,2,page.dpi}};
}
std::optional<GridAnchor> matching_grid_anchor(const GridState& grid,const BaseInfo& page){
    const std::array<double,2> position{millimeters(grid.position[0]),millimeters(grid.position[1])};
    for(const auto anchor:{GridAnchor::Center,GridAnchor::TopLeft,GridAnchor::TopRight,GridAnchor::BottomLeft,GridAnchor::BottomRight}){
        const auto candidate=grid_anchor_position(page,anchor);
        if(binary64(position[0]-1e-8)<candidate[0].value&&candidate[0].value<binary64(position[0]+1e-8)
            &&binary64(position[1]-1e-8)<candidate[1].value&&candidate[1].value<binary64(position[1]+1e-8))return anchor;
    }
    return {};
}
GridSettings grid_settings(const GridState& grid){return {grid.position,grid.spacing,grid.subdivisions};}
double grid_spacing_cm(const GridState& grid){
    const auto& length=grid.spacing;double result{};
    // RCLength::GetAs(1) uses direct source-unit factors, not a detour via mm.
    switch(length.unit){
    case 0:
        if(!std::isfinite(length.reference)||length.reference<=0)throw std::invalid_argument("网格参考分辨率无效。");
        result=binary64(binary64(length.value/length.reference)*2.54);break;
    case 1:result=length.value;break;
    case 2:result=binary64(length.value*0.1);break;
    case 3:result=binary64(length.value*2.54);break;
    case 4:result=binary64(length.value*15.24);break;
    case 5:result=binary64(length.value*0.03527777777777778);break;
    case 6:result=binary64(length.value*0.025);break;
    default:throw std::invalid_argument("网格间隔单位尚未支持。");
    }
    if(!std::isfinite(result))throw std::invalid_argument("网格间隔无效。");
    return result;
}
void set_grid_settings(GridState& grid,const GridSettings& settings,bool apply_origins){
    if(settings.subdivisions<=0||millimeters(settings.spacing)<=0)
        throw std::invalid_argument("网格间隔与细分数必须大于零。");
    (void)pixels(settings.spacing,grid.dpi);
    auto candidate=grid;
    if(apply_origins){
        for(int axis=0;axis<2;++axis)candidate.origin[axis]=pixels(settings.position[axis],grid.dpi);
        candidate.position=settings.position;
    }
    candidate.spacing=settings.spacing;candidate.subdivisions=settings.subdivisions;
    grid=candidate;
}
void move_grid(GridState& grid,int dx,int dy){
    if(!std::isfinite(grid.dpi)||grid.dpi<=0)throw std::invalid_argument("网格分辨率无效。");
    const std::array<int,2> delta{dx,dy};auto candidate=grid;
    for(int axis=0;axis<2;++axis){
        candidate.origin[axis]=integer(std::int64_t(grid.origin[axis])+delta[axis]);
        const auto value=binary64(millimeters(grid.position[axis])+millimeters({double(delta[axis]),0,grid.dpi}));
        if(!std::isfinite(value))throw std::invalid_argument("网格移动长度超出支持范围。");
        candidate.position[axis]={value,2,grid.dpi};
    }
    grid=candidate;
}
void move_grid_to(GridState& grid,std::array<int,2> origin){
    const auto dx=integer(std::int64_t(origin[0])-grid.origin[0]);
    const auto dy=integer(std::int64_t(origin[1])-grid.origin[1]);
    move_grid(grid,dx,dy);
}
overlay_detail::AdjustedGrid overlay_detail::adjusted_grid(const GridState& grid,double zoom){
    if(!std::isfinite(zoom)||zoom<=0||grid.subdivisions<=0||millimeters(grid.spacing)<=0)
        throw std::invalid_argument("网格间隔或视图倍率无效。");
    auto spacing=grid.spacing;int divisions=grid.subdivisions;
    const double percent=binary64(zoom*100);
    if(!std::isfinite(percent))throw std::invalid_argument("网格视图倍率超出支持范围。");
    // sub_625980 is passed zoom*100 by BOTH drawing and snapping. Its 8.0
    // threshold is therefore 0.08 screen pixels, not an 8px spacing rule.
    while(binary64(binary64(double(pixels(spacing,grid.dpi))/divisions)*percent)<8){
        if(divisions>1)divisions/=2;
        else spacing={binary64(millimeters(spacing)*2),2,spacing.reference};
    }
    return {spacing,divisions};
}
GridLayout grid_layout(const GridState& grid,double zoom){
    const auto adjusted=overlay_detail::adjusted_grid(grid,zoom);
    return {millimeters(adjusted.spacing),adjusted.subdivisions};
}
std::vector<GridLine> grid_lines(const GridState& grid,int axis,int extent,double zoom){
    if(axis<0||axis>1||extent<0)throw std::invalid_argument("网格显示范围无效。");
    const auto layout=grid_layout(grid,zoom);const auto origin=millimeters(grid.position[axis]);
    const double step=binary64(layout.spacing_mm/layout.subdivisions);
    const double step_pixels=binary64(binary64(grid.dpi*step)*0.03937007874015748);
    const double origin_pixels=binary64(binary64(grid.dpi*origin)*0.03937007874015748);
    if(!std::isfinite(origin_pixels)||!std::isfinite(step_pixels)||step_pixels<=0)
        throw std::invalid_argument("网格显示坐标无效。");
    // Jump past off-page indices while retaining a two-pixel margin for the
    // native rtDouble2Int negative-near-integer branch. Each remaining line
    // uses the original origin +/- index*spacing/division operation order.
    std::vector<GridLine> result;
    for(const int direction:{-1,1}){
        const double skipped=direction<0?(origin_pixels-extent-2)/step_pixels:(-origin_pixels-2)/step_pixels;
        const double first=std::max(direction<0?0.0:1.0,std::floor(skipped));
        if(!std::isfinite(first)||first>std::numeric_limits<int>::max())throw std::invalid_argument("网格显示索引超出支持范围。");
        for(std::int64_t index=static_cast<int>(first);; ++index){
            if(index>std::numeric_limits<int>::max()||result.size()>=1000000)
                throw std::invalid_argument("网格显示密度超出支持范围。");
            const auto offset=binary64(binary64(double(index)*layout.spacing_mm)/layout.subdivisions);
            const auto mm=binary64(direction<0?origin-offset:origin+offset);
            const int coordinate=pixels({mm,2,grid.dpi},grid.dpi);
            if(direction<0?coordinate<0:coordinate>=extent)break;
            if(coordinate>=0&&coordinate<=extent)result.push_back({coordinate,index%layout.subdivisions==0});
        }
    }
    return result;
}
}
