#include "retas/document.hpp"
#include "retas/fill.hpp"
#include "retas/layer_plane.hpp"
#include "selection_layer_bridge.hpp"
#include "selection_mask_source.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace retas {
namespace {
constexpr std::uint64_t max_bytes=512ull*1024*1024,max_work=128ull*1024*1024;
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("选区转图层已取消。");}
void charge(std::uint64_t& used,std::uint64_t amount,std::uint64_t limit){
    if(amount>limit-used)throw std::length_error("选区转图层资源超出支持范围。");used+=amount;
}
void integer(std::int64_t value){if(value<INT32_MIN||value>INT32_MAX)throw std::invalid_argument("选区载体坐标超出支持范围。");}
Rect clipped(Rect r,int width,int height){
    const auto left=std::clamp<std::int64_t>(r.x,0,width),top=std::clamp<std::int64_t>(r.y,0,height);
    const auto right=std::clamp<std::int64_t>(std::int64_t(r.x)+r.width,left,width),bottom=std::clamp<std::int64_t>(std::int64_t(r.y)+r.height,top,height);
    return {int(left),int(top),int(right-left),int(bottom-top)};
}
}
bool Document::selection_to_layer(const std::string& label,std::stop_token stop){
    require_no_floating_transform();
    if(!region_selection.active())return false;
    poll(stop);
    const auto selected=region_selection;
    if(selected.state<SelectionLayerState::FullPlane||selected.state>SelectionLayerState::VectorIndicated)
        throw std::invalid_argument("选区状态无效。");
    const auto generation=state_generation_,selection_generation=region_selection_generation_;
    const auto mask_generation=selected.mask?selected.mask->mutation_generation():0;
    const auto layer_count=data.layers.size();
    if(layer_count>=std::size_t(std::numeric_limits<int>::max()))throw std::length_error("图层数量超出支持范围。");
    const int page_width=data.base.width(),page_height=data.base.height();const double page_dpi=data.base.dpi;
    const auto size=selection_carrier_size(selected,page_width,page_height);
    const int width=size[0],height=size[1];const auto dpi=selection_carrier_dpi(selected,page_dpi);
    if(selected.bounds.width<0||selected.bounds.height<0)throw std::invalid_argument("选区边界尺寸无效。");
    integer(std::int64_t(selected.bounds.x)+selected.bounds.width);integer(std::int64_t(selected.bounds.y)+selected.bounds.height);
    integer(std::int64_t(selected.origin[0])+width);integer(std::int64_t(selected.origin[1])+height);
    if(!selected.mask){
        if(selected.state==SelectionLayerState::Mask||selected.state==SelectionLayerState::VectorIndicated)
            throw std::invalid_argument("选区转图层需要实际位图载体。");
        //615790 clears origin and fills page coverage. Other lazy state1
        //snapshots cannot recover the native surface from point-hit semantics.
        if(selected.state==SelectionLayerState::FullPlane&&(selected.origin!=std::array<int,2>{}||selected.bounds!=Rect{0,0,page_width,page_height}))
            throw std::invalid_argument("未物化的全选载体不是原版完整页面状态。");
    }
    const auto fill=clipped(selected.bounds,width,height);
    const int columns=(width+255)/256,rows=(height+255)/256;
    const auto present=[&](int x,int y){return selected.mask?selected.mask->block_present(x,y):
        fill.width>0&&fill.height>0&&x*256<fill.x+fill.width&&(x+1)*256>fill.x&&y*256<fill.y+fill.height&&(y+1)*256>fill.y;};
    std::uint64_t bytes=sizeof(Layer)+sizeof(Plane),work{};
    charge(bytes,(layer_count+1)*2*sizeof(Layer),max_bytes);
    charge(bytes,label.size()+256,max_bytes);
    if(selected.mask)charge(bytes,((std::uint64_t(width)+31)/32)*height*4+std::uint64_t(columns)*rows,max_bytes);
    //Close physical backing/metadata and all visits before any output tile.
    charge(work,2*std::uint64_t(columns)*rows,max_work);
    for(int y=0;y<rows;++y){poll(stop);for(int x=0;x<columns;++x)if(present(x,y)){
        charge(bytes,8192+128,max_bytes);
        charge(work,8192+std::uint64_t(std::min(256,width-x*256))*std::min(256,height-y*256),max_work);
    }}
    LayerCreateOptions options;options.family=0x10008000;options.color=0;
    auto layer=make_layer(data.base,options);
    layer.common.width=width;layer.common.height=height;layer.common.depth=1;layer.common.resolution=dpi;
    layer.common.x=selected.origin[0];layer.common.y=selected.origin[1];layer.common.position={Length{0,-1,0},Length{0,-1,0}};
    auto& plane=layer.planes[0];plane.common.width=width;plane.common.height=height;plane.common.depth=1;plane.common.resolution=dpi;
    plane.raster.emplace(width,height,1,dpi);
    for(int ty=0;ty<rows;++ty){poll(stop);for(int tx=0;tx<columns;++tx){
        if(!present(tx,ty))continue;
        auto tile=std::make_shared<Bytes>(8192,0);
        const int left=tx*256,top=ty*256,right=std::min(width,left+256),bottom=std::min(height,top+256);
        for(int y=top;y<bottom;++y){poll(stop);for(int x=left;x<right;++x){
            const bool bit=selected.mask?selected.mask->test(x,y):fill.contains(x,y);
            if(bit)(*tile)[std::size_t(y-top)*32+(x-left)/8]|=std::uint8_t(0x80u>>((x-left)%8));
        }}
        plane.raster->tiles.emplace(TileCoord{tx,ty},std::move(tile));
    }}
    const auto validate=[&]{poll(stop);
    if(state_generation_!=generation||region_selection_generation_!=selection_generation||region_selection!=selected||data.layers.size()!=layer_count
       ||data.base.width()!=page_width||data.base.height()!=page_height||data.base.dpi!=page_dpi
       ||(selected.mask&&selected.mask->mutation_generation()!=mask_generation))
        throw std::logic_error("选区转图层准备期间源已改变。");
    };
    validate();
    //43B110(mode0)->447B60->60DF80(-1) appends. The existing structural
    //item preserves live selection/floating; Undo has no old-current snapshot.
    insert_layer_checked(layer_count,std::move(layer),label,validate);return true;
}
RegionSelection selection_detail::from_selection_layer(const Layer& layer,const RegionSelection& live,int page_width,int page_height,double page_dpi,
    std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop){
    poll(stop);
    if(layer.opaque||layer.family!=0x10008000||layer.planes.size()!=1)throw std::invalid_argument("图层转选区需要单平面的可解码选区层。");
    const auto& child=layer.planes[0];
    if(child.opaque||child.family!=0x10000001||!child.raster)throw std::invalid_argument("选区层需要第0个二值光栅平面。");
    const auto& source=*child.raster;
    if(layer.common.depth!=1||child.common.depth!=1||source.depth!=1||source.color_order!=0
       ||child.common.width!=source.width||child.common.height!=source.height||child.common.resolution!=source.resolution
       ||!std::isfinite(source.resolution)||source.resolution<=0)throw std::invalid_argument("选区层位图元数据不受支持。");
    RegionSelection certificate;certificate.carrier_size={source.width,source.height};(void)selection_carrier_size(certificate,page_width,page_height);
    const auto size=selection_carrier_size(live,page_width,page_height);const double dpi=selection_carrier_dpi(live,page_dpi);
    const auto words=[](int w,int h){return ((std::uint64_t(w)+31)/32)*h*4+std::uint64_t((w+255)/256)*((h+255)/256);};
    charge(bytes,words(source.width,source.height),max_bytes);
    charge(work,words(source.width,source.height),max_work);
    charge(work,std::uint64_t(source.tiles.size())*3*65536,max_work);
    for(const auto& [key,tile]:source.tiles){poll(stop);
        if(key.x<0||key.y<0||std::int64_t(key.x)*256>=source.width||std::int64_t(key.y)*256>=source.height||!tile||tile->size()!=8192)
            throw std::invalid_argument("选区层物理块无效。");
        //BinaryMask models logical bits/presence, not nonzero physical tails.
        const int w=std::min(256,source.width-key.x*256),h=std::min(256,source.height-key.y*256);
        if(w<256||h<256)for(int y=0;y<256;++y){poll(stop);for(int x=0;x<256;++x)
            if((x>=w||y>=h)&&((*tile)[y*32+x/8]&(0x80u>>(x%8))))throw std::invalid_argument("选区层非零物理padding不受支持。");}
    }
    //6156B0(1) clears backing/origin but retains the live offscreen WH/DPI.
    //614E00->61FD70 uses the *live* size at source rawXY, then AllocateRect.
    const auto ox=std::int64_t(layer.common.x),oy=std::int64_t(layer.common.y);
    integer(ox+size[0]);integer(oy+size[1]);
    auto il=std::max<std::int64_t>(0,ox),it=std::max<std::int64_t>(0,oy);
    auto ir=std::min<std::int64_t>(page_width,ox+size[0]),ib=std::min<std::int64_t>(page_height,oy+size[1]);
    if(il>=ir||it>=ib)il=it=ir=ib=0;
    int xpad{},ypad{},width=size[0],height=size[1];
    if(il!=0||it!=0||ir!=page_width||ib!=page_height){
        const auto bump=[](std::int64_t v){return v>0?v+1:v;};
        const auto px=(bump(il)+255)/256*256,py=(bump(it)+255)/256*256;
        const auto w=std::int64_t(width)+px+bump(page_width-ir),h=std::int64_t(height)+py+bump(page_height-ib);
        integer(px);integer(py);integer(w);integer(h);xpad=int(px);ypad=int(py);width=int(w);height=int(h);
    }
    integer(ox-xpad);integer(oy-ypad);integer(ox-xpad+width);integer(oy-ypad+height);
    auto result=cleared_selection({int(ox-xpad),int(oy-ypad)},{width,height});result.bounds={xpad,ypad,0,0};result.carrier_dpi=dpi;result.vector_flags=live.vector_flags;
    (void)selection_carrier_size(result,page_width,page_height);
    charge(bytes,2*words(width,height),max_bytes);charge(work,2*words(width,height),max_work);
    auto incoming=std::make_shared<BinaryMask>(source.width,source.height);
    for(const auto& [key,tile]:source.tiles){poll(stop);incoming->materialize_block(key.x,key.y);
        const int right=std::min(source.width,(key.x+1)*256),bottom=std::min(source.height,(key.y+1)*256);
        for(int y=key.y*256;y<bottom;++y){poll(stop);for(int x=key.x*256;x<right;++x)
            if((*tile)[std::size_t(y%256)*32+(x%256)/8]&(0x80u>>(x%8)))incoming->set(x,y);}
    }
    const auto extent=incoming->extent({0,0,source.width,source.height});
    if(extent.width<=0||extent.height<=0)return result;
    charge(work,std::uint64_t(std::min(width,source.width))*std::min(height,source.height),max_work);
    auto output=std::make_shared<BinaryMask>(width,height);
    //6759C0 passes the SAME source extent coordinates to both cursors. Do not
    //add AllocateRect's xpad/ypad, page-clip or renormalize the resulting bounds.
    copy_mask_rectangle(*output,*incoming,extent,stop);
    result.state=SelectionLayerState::Mask;result.bounds=extent;result.mask=std::move(output);return result;
}
}

namespace retas::selection_detail {
RegionSelection combine_selection_layer(const Layer& layer,const RegionSelection& live,bool subtract,int page_width,int page_height,double page_dpi,std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop,bool* propagate){
    if(propagate)*propagate=false;
    poll(stop);if(layer.family!=0x10008000u||layer.opaque||layer.planes.size()!=1)throw std::invalid_argument("选择合并需要可解码的单平面选区层。");
    const auto& plane=layer.planes[0];if(plane.family!=0x10000001u||plane.opaque||!plane.raster||plane.raster->depth!=1||plane.raster->color_order!=0)throw std::invalid_argument("选择合并需要二值源。");
    const auto& source=*plane.raster;if(source.width!=plane.common.width||source.height!=plane.common.height||source.resolution!=plane.common.resolution||!std::isfinite(source.resolution)||source.resolution<=0)throw std::invalid_argument("选择合并源元数据无效。");
    RegionSelection certificate;certificate.carrier_size={source.width,source.height};(void)selection_carrier_size(certificate,page_width,page_height);
    const auto size=selection_carrier_size(live,page_width,page_height);const auto dpi=selection_carrier_dpi(live,page_dpi);
    charge(bytes,2*(((std::uint64_t(size[0])+31)/32)*size[1]*4+std::uint64_t((size[0]+255)/256)*((size[1]+255)/256))+((std::uint64_t(source.width)+31)/32)*source.height*4+std::uint64_t((source.width+255)/256)*((source.height+255)/256),max_bytes);
    charge(work,std::uint64_t(source.width)*source.height,max_work);
    charge(work,std::uint64_t(source.tiles.size())*65536+std::uint64_t(size[0])*size[1],max_work);
    BinaryMask incoming(source.width,source.height);
    for(const auto& [key,tile]:source.tiles){poll(stop);if(key.x<0||key.y<0||key.x*256ll>=source.width||key.y*256ll>=source.height||!tile||tile->size()!=8192)throw std::invalid_argument("选择合并源块无效。");incoming.materialize_block(key.x,key.y);
        const int width=std::min(256,source.width-key.x*256),height=std::min(256,source.height-key.y*256);
        if(width<256||height<256)for(int y=0;y<256;++y){poll(stop);for(int x=0;x<256;++x)if((x>=width||y>=height)&&((*tile)[y*32+x/8]&(0x80u>>(x%8))))throw std::invalid_argument("选择合并源非零padding不受支持。");}
        for(int y=key.y*256;y<std::min((key.y+1)*256,source.height);++y){poll(stop);for(int x=key.x*256;x<std::min((key.x+1)*256,source.width);++x)if((*tile)[std::size_t(y%256)*32+x%256/8]&(0x80u>>(x%8)))incoming.set(x,y);}
    }
    auto result=live;auto output=live.mask?std::make_shared<BinaryMask>(*live.mask):std::make_shared<BinaryMask>(size[0],size[1]);
    if(output->width()!=size[0]||output->height()!=size[1])throw std::invalid_argument("选择合并目标载体无效。");
    if(!live.mask&&live.active())for(int y=0;y<size[1];++y){poll(stop);for(int x=0;x<size[0];++x)if(live.bounds.contains(x,y))output->set(x,y);}
    const auto extent=incoming.extent({0,0,source.width,source.height});if(!subtract&&(extent.width<=0||extent.height<=0))return result;
    const double ratio=dpi/source.resolution;const bool admitted=ratio>=1&&std::isfinite(ratio)&&ratio<=100000;const int factor=admitted?int(ratio):0;
    const auto dx=std::int64_t(layer.common.x)-live.origin[0],dy=std::int64_t(layer.common.y)-live.origin[1];bool processed{};
    if(admitted){
        const auto tx=dx*factor,ty=dy*factor;
        const auto left=subtract?std::int64_t(live.bounds.x)-dx:extent.x,top=subtract?std::int64_t(live.bounds.y)-dy:extent.y;
        const auto right=left+(subtract?live.bounds.width:extent.width),bottom=top+(subtract?live.bounds.height:extent.height);
        const auto l=std::max({std::int64_t(0),left,-tx/factor}),t=std::max({std::int64_t(0),top,-ty/factor});
        const auto r=std::min({std::int64_t(source.width),right,(size[0]-tx)/factor}),b=std::min({std::int64_t(source.height),bottom,(size[1]-ty)/factor});
        if(l<r&&t<b){charge(work,std::uint64_t(r-l)*std::uint64_t(b-t)*factor*factor,max_work);
            for(auto sy=t;sy<b;++sy){poll(stop);for(auto sx=l;sx<r;++sx){if(!incoming.block_present(int(sx)/256,int(sy)/256))continue;
                for(int j=0;j<factor;++j)for(int i=0;i<factor;++i){const auto x=sx*factor+tx+i,y=sy*factor+ty+j;if(x<0||y<0||x>=size[0]||y>=size[1])continue;
                    if(subtract&&!output->block_present(int(x)/256,int(y)/256))continue;processed=true;output->materialize_block(int(x)/256,int(y)/256);
                    if(incoming.test(int(sx),int(sy)))output->set(int(x),int(y),!subtract);
                }
            }}
            if(subtract)output->optimize_blocks({int(std::max<std::int64_t>(0,l*factor+tx)),int(std::max<std::int64_t>(0,t*factor+ty)),int((r-l)*factor),int((b-t)*factor)});
        }
    }
    result.mask=std::move(output);
    if(!subtract){const auto l=std::min<std::int64_t>(live.bounds.x,extent.x+dx),t=std::min<std::int64_t>(live.bounds.y,extent.y+dy),r=std::max<std::int64_t>(std::int64_t(live.bounds.x)+live.bounds.width,extent.x+dx+extent.width),b=std::max<std::int64_t>(std::int64_t(live.bounds.y)+live.bounds.height,extent.y+dy+extent.height);integer(l);integer(t);integer(r);integer(b);integer(r-l);integer(b-t);result.bounds={int(l),int(t),int(r-l),int(b-t)};result.state=SelectionLayerState::Mask;result.vector_cache.reset();result.vector_cache_origin={};if(propagate)*propagate=true;}
    else if(processed){const auto remaining=result.mask->extent({0,0,size[0],size[1]});if(remaining.width>0&&remaining.height>0){result.bounds=remaining;result.state=SelectionLayerState::Mask;result.vector_cache.reset();result.vector_cache_origin={};if(propagate)*propagate=true;}}
    return result;
}
}
