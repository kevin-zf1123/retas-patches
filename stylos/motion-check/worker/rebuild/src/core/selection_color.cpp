#include "retas/selection_color.hpp"
#include "selection_color_command.hpp"
#include "retas/document.hpp"
#include "retas/fill.hpp"
#include "selection_color_math.hpp"
#include "selection_raster.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <stdexcept>

namespace retas {
namespace {
bool command_ok(SelectionColorCommand command){return command>=SelectionColorCommand::Color&&command<=SelectionColorCommand::ColorSpace;}
void cancelled(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Color selection cancelled");}
struct Budget {
    std::uint64_t remaining{128ull*1024*1024};
    void spend(std::uint64_t value){if(value>remaining)throw std::length_error("Color selection exceeds work budget");remaining-=value;}
};
std::uint64_t packed(Pixel p,int depth){
    if(depth==32)return std::uint64_t(p.b>>8)|(std::uint64_t(p.g>>8)<<8)|(std::uint64_t(p.r>>8)<<16)|(std::uint64_t(p.a>>8)<<24);
    return std::uint64_t(p.b)|(std::uint64_t(p.g)<<16)|(std::uint64_t(p.r)<<32)|(std::uint64_t(p.a)<<48);
}
unsigned gray(Pixel p,int depth){
    const unsigned r=depth==32?p.r>>8:p.r,g=depth==32?p.g>>8:p.g,b=depth==32?p.b>>8:p.b;
    return (4897*r+9611*g+1876*b)>>14;
}
bool equal(std::uint64_t value,Pixel reference,int depth,int mode){
    if(depth==1)return value==unsigned(reference.a!=0);
    if(depth==8)return value==255-unsigned(reference.r>>8);
    if(depth!=32&&depth!=64)return false;
    if(mode==4)return (value>>(depth==32?24:48))==gray(reference,depth);
    if(value==packed(reference,depth))return true;
    return depth==32&&mode==5&&(!reference.a||(packed(reference,32)&0xffffff)==0xffffff)&&(value&0xffffff)==0xffffff;
}
}
bool can_select_raster_color(const DocumentData& data,SelectionColorCommand command) noexcept{
    if(!command_ok(command)||data.current_layer<0||std::size_t(data.current_layer)>=data.layers.size())return false;
    const auto& owner=data.layers[std::size_t(data.current_layer)];const auto* plane=data.active_plane();
    return owner.common.flags[1]&&!(owner.family&0x40000000u)&&plane&&plane->common.flags[1]
        &&(plane->family&0xf0000000u)==0x10000000u&&(command!=SelectionColorCommand::ColorSpace||plane->common.opacity_mode!=4);
}
static RegionSelection raster_color_impl(const DocumentData& data,const RegionSelection& current,SelectionColorCommand command,Pixel reference,bool additive_space,std::stop_token stop,bool admitted,selection_color_detail::BitmapAction* action){
    if(!command_ok(command))throw std::invalid_argument("Invalid color selection command");
    cancelled(stop);if(!admitted&&!can_select_raster_color(data,command))return current;
    const auto& owner=data.layers[std::size_t(data.current_layer)];const auto& plane=*data.active_plane();
    if(owner.opaque||plane.opaque||!plane.raster||!raster_stroke_surface_supported(*plane.raster))throw std::invalid_argument("Color selection requires a canonical raster plane");
    const auto& raster=*plane.raster;
    if(raster.width<=0||raster.height<=0||raster.width>100000||raster.height>100000||plane.common.depth!=raster.depth
        ||!std::isfinite(owner.common.resolution)||owner.common.resolution<=0||!std::isfinite(plane.common.resolution)||plane.common.resolution<=0
        ||raster.resolution!=plane.common.resolution)throw std::invalid_argument("Invalid color selection raster metadata");
    for(const auto& [tile,bytes]:raster.tiles)if(tile.x<0||tile.y<0||std::int64_t(tile.x)*256>=raster.width||std::int64_t(tile.y)*256>=raster.height
        ||!bytes||bytes->size()!=raster.tile_bytes())throw std::invalid_argument("Invalid color selection raster tile");
    const bool space=command==SelectionColorCommand::ColorSpace;
    // 677A50 still admits history but never enters the worker below32 bits.
    if(space&&raster.depth<32)return current;
    const int width=data.base.width(),height=data.base.height();const auto size=selection_carrier_size(current,width,height);
    const auto left=std::max<std::int64_t>(0,-std::int64_t(owner.common.x)),top=std::max<std::int64_t>(0,-std::int64_t(owner.common.y));
    const auto right=std::min<std::int64_t>(raster.width,std::int64_t(width)-owner.common.x),bottom=std::min<std::int64_t>(raster.height,std::int64_t(height)-owner.common.y);
    Budget budget;if(left<right&&top<bottom)budget.spend(std::uint64_t(right-left)*(bottom-top));
    BinaryMask incoming(size[0],size[1]);
    const auto dx=std::int64_t(owner.common.x)-current.origin[0],dy=std::int64_t(owner.common.y)-current.origin[1];
    const bool positive=command!=SelectionColorCommand::NotColor;
    const bool missing_equal=!reference.a||(raster.depth==32&&plane.common.opacity_mode==5&&(packed(reference,32)&0xffffff)==0xffffff);
    if(left<right&&top<bottom)for(auto y=top;y<bottom;++y){
        cancelled(stop);const auto my=y+dy;if(my<0||my>=size[1])continue;
        for(auto x=left;x<right;++x){
            const auto mx=x+dx;if(mx<0||mx>=size[0])continue;
            const bool present=raster.tiles.contains({int(x)/256,int(y)/256});
            const bool hit=space?(present&&selection_color_detail::outside_space(raster.pixel(int(x),int(y)),raster.depth)):
                (positive==(present?equal(selection_detail::raw_pixel(raster,int(x),int(y)),reference,raster.depth,plane.common.opacity_mode):missing_equal));
            if(hit)incoming.set(int(mx),int(my));
        }
    }
    // The write lock survives GetExtentRect's nested read lock. Its scan
    // materializes the entire scratch carrier; only tight-result Copy/OR
    // reaches the live selection. Account and retain that native topology.
    budget.spend(std::uint64_t(size[0])*size[1]);
    const auto extent=incoming.extent({0,0,size[0],size[1]});
    const auto combine=command==SelectionColorCommand::ColorAdd||(space&&additive_space)?SelectionCombine::Add:SelectionCombine::Replace;
    RegionSelection result;
    if(extent.width<=0||extent.height<=0){result=combine==SelectionCombine::Add?current:cleared_selection({},size);if(action&&combine!=SelectionCombine::Add)*action=selection_color_detail::BitmapAction::Clear;}
    else{
        if(action)*action=selection_color_detail::apply_action(current,combine==SelectionCombine::Add,width,height,extent);
        for(int ty=0;ty*256<size[1];++ty)for(int tx=0;tx*256<size[0];++tx)incoming.materialize_block(tx,ty);
        budget.spend(std::uint64_t(extent.width)*extent.height);
        result=combine_selection_mask(current,combine,width,height,extent,incoming);
    }
    result.vector_flags=current.vector_flags;result.carrier_dpi=current.carrier_dpi;cancelled(stop);return result;
}
RegionSelection select_raster_color(const DocumentData& data,const RegionSelection& current,SelectionColorCommand command,Pixel reference,bool additive_space,std::stop_token stop){
    return raster_color_impl(data,current,command,reference,additive_space,stop,false,nullptr);
}
selection_color_detail::BitmapSelection selection_color_detail::raster_after_admission(const DocumentData& data,const RegionSelection& current,SelectionColorCommand command,Pixel reference,bool additive_space,std::stop_token stop){
    BitmapSelection result;result.selection=raster_color_impl(data,current,command,reference,additive_space,stop,true,&result.action);return result;
}

}
