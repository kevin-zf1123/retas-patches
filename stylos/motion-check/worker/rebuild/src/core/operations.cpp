#include "retas/operations.hpp"
#include <algorithm>
#include <cmath>
#include <deque>

namespace retas {
std::optional<RasterSelection> raster_selection(const Document& document,const Layer& owner,const Raster& raster,RasterSelectionSource source){
    if(!document.region_selection.active())return {};
    const auto dpi=document.data.base.dpi;
    if(!std::isfinite(dpi)||dpi<=0||dpi!=owner.common.resolution||dpi!=raster.resolution)
        throw std::invalid_argument("不同分辨率的选区映射尚未接入。");
    auto selected=document.region_selection;
    if(source==RasterSelectionSource::BackingSurface
        &&(selected.state==SelectionLayerState::Rectangle||selected.state==SelectionLayerState::FullPlane)){
        // sub_615830 retains the requested bounds but RectFill clips actual
        // backing bits to the selection surface. AreaMask reads that surface,
        // whereas Bucket's state2 classifier uses the stored bounds directly.
        const auto r=selected.bounds;
        const auto size=selection_carrier_size(selected,document.data.base.width(),document.data.base.height());
        const auto width=size[0],height=size[1];
        const auto left=std::clamp<std::int64_t>(r.x,0,width),top=std::clamp<std::int64_t>(r.y,0,height);
        const auto right=std::clamp<std::int64_t>(std::int64_t(r.x)+std::max(0,r.width),left,width);
        const auto bottom=std::clamp<std::int64_t>(std::int64_t(r.y)+std::max(0,r.height),top,height);
        selected.bounds={int(left),int(top),int(right-left),int(bottom-top)};
        if(selected.mask)selected.state=SelectionLayerState::Mask;
    }
    return project_selection(selected,{owner.common.x,owner.common.y},raster.width,raster.height);
}
Rect raster_local_rect(const Layer& owner,const Raster& raster,Rect region) noexcept{
    const auto x=std::int64_t(region.x)-owner.common.x,y=std::int64_t(region.y)-owner.common.y;
    const auto width=std::max(0,raster.width),height=std::max(0,raster.height);
    const auto left=std::clamp<std::int64_t>(x,0,width),top=std::clamp<std::int64_t>(y,0,height);
    const auto right=std::clamp<std::int64_t>(x+std::max(0,region.width),left,width);
    const auto bottom=std::clamp<std::int64_t>(y+std::max(0,region.height),top,height);
    return {int(left),int(top),int(right-left),int(bottom-top)};
}
bool paint_segment(Raster& image,Point from,Point to,const PaintOptions& o,const std::optional<RasterSelection>& selection){
    if(!std::isfinite(o.radius)||o.radius<=0||o.radius>1000)throw std::invalid_argument("Invalid brush radius");
    bool changed=false;
    const double dx=to.x-from.x,dy=to.y-from.y,dist=std::hypot(dx,dy);
    const int steps=std::max(1,int(std::ceil(dist/std::max(0.5,o.radius*0.25))));
    for(int s=0;s<=steps;++s){
        const double cx=from.x+dx*s/steps,cy=from.y+dy*s/steps;
        const int left=std::max(0,int(std::floor(cx-o.radius))),right=std::min(image.width-1,int(std::ceil(cx+o.radius)));
        const int top=std::max(0,int(std::floor(cy-o.radius))),bottom=std::min(image.height-1,int(std::ceil(cy+o.radius)));
        for(int y=top;y<=bottom;++y)for(int x=left;x<=right;++x){
            if(selection&&!selection->contains(x,y))continue;
            double r=std::hypot(x+0.5-cx,y+0.5-cy)/o.radius;if(r>1)continue;
            double coverage=r<=o.hardness?1:(1-r)/std::max(0.001,1-o.hardness);
            auto alpha=static_cast<std::uint16_t>(std::clamp(coverage,0.0,1.0)*o.opacity);
            const auto before=image.pixel(x,y);
            auto after=before;
            if(o.eraser){after.a=std::uint16_t(std::uint64_t(after.a)*(65535-alpha)/65535);if(!after.a)after={};}
            else after=over(before,o.color,alpha);
            if(image.depth==32){after.r=after.r/257*257;after.g=after.g/257*257;after.b=after.b/257*257;after.a=after.a/257*257;}
            if(after==before)continue;
            image.set_pixel(x,y,after);changed=true;
        }
    }
    return changed;
}
void fill_region(Raster& image,int sx,int sy,Pixel color,int tolerance,const std::optional<RasterSelection>& selection,std::stop_token stop){
    if(!image.contains(sx,sy)||(selection&&!selection->contains(sx,sy)))return;
    Raster source=image;
    const auto wanted=source.pixel(sx,sy);
    if(wanted==color)return;
    std::vector<std::uint8_t> visited(std::size_t(image.width)*image.height,0);
    auto match=[&](int x,int y){
        if(!image.contains(x,y)||(selection&&!selection->contains(x,y)))return false;
        if(visited[std::size_t(y)*image.width+x])return false;
        const auto p=source.pixel(x,y);
        return std::abs(int(p.r)-wanted.r)<=tolerance*257&&std::abs(int(p.g)-wanted.g)<=tolerance*257&&
               std::abs(int(p.b)-wanted.b)<=tolerance*257&&std::abs(int(p.a)-wanted.a)<=tolerance*257;
    };
    std::deque<std::pair<int,int>> seeds;seeds.emplace_back(sx,sy);
    while(!seeds.empty()){
        if(stop.stop_requested())throw std::runtime_error("Fill cancelled");
        auto [x,y]=seeds.front();seeds.pop_front();if(!match(x,y))continue;
        int left=x,right=x;while(match(left-1,y))--left;while(match(right+1,y))++right;
        bool up=false,down=false;
        for(int px=left;px<=right;++px){
            visited[std::size_t(y)*image.width+px]=1;image.set_pixel(px,y,color);
            bool next=match(px,y-1);if(next&&!up)seeds.emplace_back(px,y-1);up=next;
            next=match(px,y+1);if(next&&!down)seeds.emplace_back(px,y+1);down=next;
        }
    }
}
void fill_rectangle(Raster& image,Rect rect,Pixel color,const std::optional<RasterSelection>& selection){
    // Clip in the wide domain before narrowing; caller rectangles may be
    // off-page and the active mask may contain holes inside its stored bounds.
    const auto left=std::clamp<std::int64_t>(rect.x,0,image.width),top=std::clamp<std::int64_t>(rect.y,0,image.height);
    const auto right=std::clamp<std::int64_t>(std::int64_t(rect.x)+std::max(0,rect.width),left,image.width);
    const auto bottom=std::clamp<std::int64_t>(std::int64_t(rect.y)+std::max(0,rect.height),top,image.height);
    auto work=Rect{int(left),int(top),int(right-left),int(bottom-top)};
    if(selection)work=selection->extent(work);
    for(int y=work.y;y<work.y+work.height;++y)
        for(int x=work.x;x<work.x+work.width;++x)
            if(!selection||selection->contains(x,y))image.set_pixel(x,y,color);
}
Raster transform_raster(const Raster& source,int turns,bool mx,bool my){
    turns=((turns%4)+4)%4;
    Raster output(turns%2?source.height:source.width,turns%2?source.width:source.height,source.depth,source.resolution);
    for(const auto& [pos,bytes]:source.tiles)
        for(int y=pos.y*256;y<std::min(source.height,(pos.y+1)*256);++y)
            for(int x=pos.x*256;x<std::min(source.width,(pos.x+1)*256);++x){
                auto p=source.pixel(x,y);if(!p.a&&p==Pixel{})continue;
                int xx=mx?source.width-1-x:x,yy=my?source.height-1-y:y,nx=xx,ny=yy;
                if(turns==1){nx=source.height-1-yy;ny=xx;}
                if(turns==2){nx=source.width-1-xx;ny=source.height-1-yy;}
                if(turns==3){nx=yy;ny=source.width-1-xx;}
                output.set_pixel(nx,ny,p);
            }
    return output;
}
void map_pixels(Raster& image,const std::function<Pixel(Pixel)>& mapper,const std::optional<RasterSelection>& selection){
    auto source=image;
    for(const auto& [pos,bytes]:source.tiles)
        for(int y=pos.y*256;y<std::min(image.height,(pos.y+1)*256);++y)
            for(int x=pos.x*256;x<std::min(image.width,(pos.x+1)*256);++x){
                if(selection&&!selection->contains(x,y))continue;
                image.set_pixel(x,y,mapper(source.pixel(x,y)));
            }
}
}
