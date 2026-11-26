#include "page_geometry.hpp"
#include "vector_document_geometry.hpp"
#include "modal_document_guard.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <cmath>

namespace retas::page_geometry_detail {
namespace {
using numeric_detail::binary64;
// RCLength::operator= copies the value unconditionally, but an unassigned
// unit or nonpositive reference retains the destination's corresponding lane.
void copy_length(Length& destination,const Length& source){destination.value=source.value;if(source.unit!=-2)destination.unit=source.unit;if(source.reference>0)destination.reference=source.reference;}
int integer(std::int64_t n){if(n<INT32_MIN||n>INT32_MAX)throw std::overflow_error("Page transform coordinate overflow");return int(n);}
int pixels(const Length& length,double dpi){
    double value{};switch(length.unit){case 0:if(length.reference==0)return 0;value=binary64(binary64(length.value/length.reference)*dpi);break;case 1:value=binary64(binary64(dpi*length.value)*0.3937007874015748);break;case 2:value=binary64(binary64(dpi*length.value)*0.03937007874015748);break;case 3:value=binary64(dpi*length.value);break;case 5:value=binary64(binary64(dpi*length.value)*0.01388888888888889);break;case 6:value=binary64(binary64(dpi*length.value)*0.00984251968503937);break;default:return 0;}
    const auto result=legacy_rt_double_to_int(value);if(!result)throw std::invalid_argument("Page origin exceeds numeric domain");return *result;
}
void validate(const Plane& plane){
    if(plane.opaque||!plane.raster)throw std::invalid_argument("Page transform needs decoded raster storage");const auto& r=*plane.raster;
    if(r.width<1||r.height<1||r.width>100000||r.height>100000||std::int64_t(r.width)*r.height>268435456||!std::isfinite(r.resolution)||r.resolution<=0||r.width!=plane.common.width||r.height!=plane.common.height||r.depth!=plane.common.depth||r.resolution!=plane.common.resolution)throw std::invalid_argument("Invalid page raster metadata");
    if(!(r.depth==1||r.depth==2||r.depth==8||r.depth==16||r.depth==24||r.depth==32||r.depth==64)||r.color_order!=(r.depth==24?4:r.depth>=32?3:0))throw std::invalid_argument("Unsupported page raster storage");
    for(const auto& [key,tile]:r.tiles)if(key.x<0||key.y<0||std::int64_t(key.x)*256>=r.width||std::int64_t(key.y)*256>=r.height||!tile||tile->size()!=r.tile_bytes())throw std::invalid_argument("Invalid page raster tile");
}
Bytes empty(const Raster& image){Bytes out(image.tile_bytes());if(image.depth==24)std::ranges::fill(out,255);else if(image.depth>=32)for(std::size_t i=0;i<out.size();i+=image.depth/8)std::fill_n(out.begin()+i,image.depth==32?3:6,255);return out;}
Raster raster(const Raster& source,int kind,std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop){
    const bool rotate=kind>=30;Raster out(rotate?source.height:source.width,rotate?source.width:source.height,source.depth,source.resolution);
    modal_detail::charge(bytes,out.tile_bytes()+source.tiles.size()*sizeof(TileCoord),modal_detail::max_bytes,"Page raster storage limit");const auto initial=empty(out);
    std::vector<TileCoord> tiles;tiles.reserve(source.tiles.size());for(const auto& [key,value]:source.tiles){(void)value;tiles.push_back(key);}std::ranges::sort(tiles,[](auto a,auto b){return a.y==b.y?a.x<b.x:a.y<b.y;});
    for(const auto tile:tiles){modal_detail::poll(stop);const auto& raw=*source.tiles.at(tile);const int left=tile.x*256,top=tile.y*256,right=std::min(left+256,source.width),bottom=std::min(top+256,source.height);
        modal_detail::charge(work,std::uint64_t(right-left)*(bottom-top),modal_detail::max_work,"Page transform exceeds128Mi samples");
        for(int y=top;y<bottom;++y){modal_detail::poll(stop);for(int x=left;x<right;++x){int dx=x,dy=y;switch(kind){case 28:dx=source.width-1-x;break;case 29:dy=source.height-1-y;break;case 30:dx=y;dy=source.width-1-x;break;case 31:dx=source.height-1-y;dy=x;break;}
            const TileCoord key{dx/256,dy/256};auto at=out.tiles.find(key);if(at==out.tiles.end()){modal_detail::charge(bytes,out.tile_bytes()+128,modal_detail::max_bytes,"Page transform exceeds512MiB");at=out.tiles.emplace(key,std::make_shared<Bytes>(initial)).first;}auto& target=*at->second;
            const auto sp=std::size_t(y%256)*256+x%256,dp=std::size_t(dy%256)*256+dx%256;
            if(source.depth<8){const unsigned mask=(1u<<source.depth)-1;const auto shift=8-source.depth-(sp*source.depth)%8,dshift=8-source.depth-(dp*source.depth)%8;unsigned value=(raw[sp*source.depth/8]>>shift)&mask;if(source.depth==2&&value==1)value=0;auto& value_out=target[dp*source.depth/8];value_out=std::uint8_t((value_out&~(mask<<dshift))|(value<<dshift));}
            else{const auto stride=source.depth/8;std::copy_n(raw.begin()+sp*stride,stride,target.begin()+dp*stride);}
        }}
    }
    return out;
}
}
std::int32_t inverse(std::int32_t kind){if(kind==28||kind==29)return kind;if(kind==30)return 31;if(kind==31)return 30;throw std::invalid_argument("Page transform kind must be28..31");}
void apply(DocumentData& data,std::int32_t kind,std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop){
    (void)inverse(kind);modal_detail::poll(stop);if(data.unrestored_external_content)throw std::invalid_argument("Page transform cannot consume unrestored external content");
    const int width=data.base.width(),height=data.base.height();const bool rotate=kind>=30;
    for(auto& owner:data.layers){modal_detail::poll(stop);if(owner.opaque)throw std::invalid_argument("Page transform cannot consume an opaque owner");
        const int x=owner.common.x,y=owner.common.y,w=owner.common.width,h=owner.common.height;
        if(w<0||h<0||!std::isfinite(owner.common.resolution)||owner.common.resolution<=0)throw std::invalid_argument("Invalid page owner geometry");
        const bool vector=vector_geometry_detail::owner(owner.family);if(vector)vector_geometry_detail::page_transform(owner,kind,bytes,work,stop);
        for(auto& plane:owner.planes){modal_detail::poll(stop);if(plane.raster){validate(plane);plane.raster=raster(*plane.raster,kind,bytes,work,stop);plane.common.width=plane.raster->width;plane.common.height=plane.raster->height;plane.pixels_changed=true;plane.raster_modified=true;}
            else if(plane.opaque)throw std::invalid_argument("Page transform cannot consume an opaque child");
            else if(!vector&&plane.family!=0x40000004u&&plane.family!=0x40000010u&&plane.family!=0x40001008u&&plane.family!=0x40002008u&&plane.family!=0x40000080u)throw std::invalid_argument("Unsupported nonraster page transform child");
        }
        int nx=x,ny=y;if(kind==28)nx=integer(std::int64_t(x)+2*(std::int64_t(width/2)-(std::int64_t(x)+w/2)));
        else if(kind==29)ny=integer(std::int64_t(y)+2*(std::int64_t(height/2)-(std::int64_t(y)+h/2)));
        else if(kind==30){nx=y;ny=integer(std::int64_t(width)-x-w);}else{nx=integer(std::int64_t(height)-y-h);ny=x;}
        if(owner.family==0x40000800u||owner.family==0x48040800u){nx=x;ny=y;}
        if(owner.family==0x40001000u)for(std::size_t axis=0;axis<2;++axis){const auto delta=axis?std::int64_t(ny)-y:std::int64_t(nx)-x;data.base.lengths[2+axis]={double(integer(std::int64_t(pixels(data.base.lengths[2+axis],data.base.dpi))+delta)),0,data.base.dpi};}
        owner.common.x=nx;owner.common.y=ny;if(rotate){owner.common.width=h;owner.common.height=w;}
    }
    if(rotate){const auto old_width=data.base.lengths[0],old_height=data.base.lengths[1];copy_length(data.base.lengths[0],old_height);copy_length(data.base.lengths[1],old_width);if(data.base.frame_mode==0){copy_length(data.base.lengths[4],old_height);copy_length(data.base.lengths[5],old_width);data.base.lengths[2]={double(height/2),0,data.base.dpi};data.base.lengths[3]={double(width/2),0,data.base.dpi};}}
    modal_detail::poll(stop);
}
}
