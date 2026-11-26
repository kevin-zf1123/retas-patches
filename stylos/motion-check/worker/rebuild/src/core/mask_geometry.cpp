#include "retas/mask_geometry.hpp"
#include "retas/numeric.hpp"
#include "retas/viewport.hpp"
#include <algorithm>
#include <stdexcept>

namespace retas {
Rect mask_polygon(BinaryMask& mask,std::span<const MaskPoint> points,
                   std::int64_t max_rows,PolygonEdgeMode edge_mode){
    // RCVOffscreen::FillPolygon, rcwCmn 0x10556210.
    // This is an integer scanline mask, not QPainter's polygon rasterization.
    struct Edge{int first,last;double x,step;};
    std::vector<Edge> edges;edges.reserve(points.size());
    int min_x=mask.width(),max_x=-1,min_y=mask.height(),max_y=-1;
    for(std::size_t i=0;i<points.size();++i){
        auto a=points[i],b=points[(i+1)%points.size()];
        if(a.y==b.y)continue;
        if(a.y>b.y)std::swap(a,b);
        edges.push_back({a.y,b.y,double(a.x),(double(b.x)-a.x)/(double(b.y)-a.y)});
    }
    if(edges.empty())return {}; // Native all-horizontal polygons write nothing.
    std::sort(edges.begin(),edges.end(),[](const Edge& a,const Edge& b){return a.first<b.first;});
    const auto paint_span=[&](int y,int left,int right){
        if(y<0||y>=mask.height())return;
        left=std::max(left,0);right=std::min(right,mask.width()-1);
        if(left>right)return;
        for(int x=left;x<=right;++x)mask.set(x,y);
        min_x=std::min(min_x,left);max_x=std::max(max_x,right);
        min_y=std::min(min_y,y);max_y=std::max(max_y,y);
    };
    const int last=std::min(mask.height(),std::max_element(edges.begin(),edges.end(),[](const Edge& a,const Edge& b){return a.last<b.last;})->last);
    // Off-image rows still participate in the exact binary64 recurrence.
    // Bound the raw iteration distance as well as the clipped pixel bbox.
    if(std::int64_t(last)-edges.front().first>max_rows)
        throw std::invalid_argument("作用范围过大，请分段操作或缩小范围。");
    std::vector<double> intersections;intersections.reserve(edges.size());
    for(std::int64_t y=edges.front().first;y<last;++y){
        intersections.clear();
        for(auto& edge:edges){
            if(edge.first>y)break;
            if(y>=edge.last)continue;
            intersections.push_back(edge.x);
            // Preserve the per-row binary64 recurrence, including off-image
            // rows; multiplying the slope by the row index changes rounding.
            edge.x=retas::numeric_detail::binary64(edge.x+edge.step);
        }
        if(y<0)continue;
        std::sort(intersections.begin(),intersections.end());
        if(intersections.size()%2)throw std::invalid_argument("多边形的交点无效。");
        for(std::size_t i=0;i<intersections.size();i+=2){
            const auto left=retas::legacy_rt_double_to_int(intersections[i]);
            const auto right=retas::legacy_rt_double_to_int(intersections[i+1]);
            if(!left||!right)throw std::invalid_argument("多边形坐标超出支持范围。");
            if(edge_mode==PolygonEdgeMode::HalfOpen){
                if(*right>0)paint_span(int(y),*left,*right-1);
            }else paint_span(int(y),*left,*right);
        }
    }
    // flag=1 includes both span ends and replays horizontal edges. Sloping
    // edges have no independent outline pass; a bottom apex stays excluded.
    if(edge_mode==PolygonEdgeMode::Inclusive)for(std::size_t i=0;i<points.size();++i){
        const auto a=points[i],b=points[(i+1)%points.size()];
        if(a.y==b.y)paint_span(a.y,std::min(a.x,b.x),std::max(a.x,b.x));
    }
    return max_x<min_x?Rect{}:Rect{min_x,min_y,max_x-min_x+1,max_y-min_y+1};
}
}
