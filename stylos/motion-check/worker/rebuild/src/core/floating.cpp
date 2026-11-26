#include "native_raster_copy.hpp"
#include "retas/floating.hpp"
#include "retas/fill.hpp"
#include "raster_layer_expansion.hpp"
#include "retas/numeric.hpp"
#include "selection_raster.hpp"
#include "selection_vector.hpp"
#include "selection_extension.hpp"
#include "selection_clipboard_target.hpp"
#include "vector_indication.hpp"
#include "modal_document_guard.hpp"
#include "floating_capture.hpp"
#include "selection_copy.hpp"
#include "text_font_backend_internal.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace retas {
using raster_copy_detail::low_copy_pixel;
using raster_copy_detail::writeback_pixel;
namespace {
struct Area {
    std::int64_t left{},top{},right{},bottom{};
    bool empty() const {return left>=right||top>=bottom;}
};
Area unite(Area,Area);
int coordinate(std::int64_t);
Area intersect(Area a,Area b){return {std::max(a.left,b.left),std::max(a.top,b.top),std::min(a.right,b.right),std::min(a.bottom,b.bottom)};}
Area area(Rect r){return {r.x,r.y,std::int64_t(r.x)+std::max(0,r.width),std::int64_t(r.y)+std::max(0,r.height)};}
std::uint64_t capture_visits(Area request,const Raster& raster,bool rectangle){
    if(request.empty())return 0;const auto area=std::uint64_t(request.right-request.left)*std::uint64_t(request.bottom-request.top);if(rectangle)return area;
    // Optimize examines the complete logical portions of touched256 blocks,
    // including pixels outside a narrow request within those blocks.
    const auto left=request.left/256*256,top=request.top/256*256,right=std::min<std::int64_t>(raster.width,(request.right+255)/256*256),bottom=std::min<std::int64_t>(raster.height,(request.bottom+255)/256*256);
    return area+2*std::uint64_t(right-left)*std::uint64_t(bottom-top);
}
struct Budget {
    std::uint64_t operations=128ull*1024*1024,bytes=256ull*1024*1024;
    std::stop_token stop;bool transform{},capture_precharged{};
    void poll() const {if(stop.stop_requested())throw std::runtime_error("Floating transform writeback cancelled");}
    void work(std::uint64_t amount){poll();if(capture_precharged)return;if(amount>operations){if(transform)throw std::length_error("Transform writeback work exceeds aggregate budget");throw std::invalid_argument("浮动选区计算量过大。");}operations-=amount;}
    void allocate(std::uint64_t amount){poll();if(amount>bytes){if(transform)throw std::length_error("Transform writeback storage exceeds aggregate budget");throw std::invalid_argument("浮动选区图块过多。");}bytes-=amount;}
};
void validate(const Layer& layer,const Plane& plane,double dpi,bool transform=false){
    if(layer.opaque||plane.opaque||(layer.family&0xf0000000u)!=0x10000000u||(plane.family&0xf0000000u)!=0x10000000u
        ||!plane.raster||!raster_stroke_surface_supported(*plane.raster))throw std::invalid_argument("此浮动选区目标类型尚未接入。");
    const auto& raster=*plane.raster;
    if(transform&&(!raster.can_edit()||plane.common.resolution!=dpi))throw std::invalid_argument("Transform capture requires same-DPI32/64 raster planes");
    if(!std::isfinite(dpi)||dpi<=0||layer.common.resolution!=dpi||!std::isfinite(plane.common.resolution)||plane.common.resolution<=0||raster.resolution!=plane.common.resolution)
        throw std::invalid_argument("不同分辨率的浮动选区尚未接入。");
    if(raster.width<=0||raster.height<=0||raster.width>100000||raster.height>100000
        ||std::uint64_t(raster.width)*raster.height>268435456)throw std::invalid_argument("浮动选区图像尺寸无效。");
    for(const auto& [coordinate,bytes]:raster.tiles)
        if(coordinate.x<0||coordinate.y<0||std::int64_t(coordinate.x)*256>=raster.width||std::int64_t(coordinate.y)*256>=raster.height
            ||!bytes||bytes->size()!=raster.tile_bytes())throw std::invalid_argument("浮动选区源图块无效。");
}
void optimize_block(Raster& raster,TileCoord coordinate,Budget& budget){
    if(!raster.tiles.contains(coordinate))return;
    const int left=coordinate.x*256,top=coordinate.y*256;
    const int right=std::min(left+256,raster.width),bottom=std::min(top+256,raster.height);
    budget.work(std::uint64_t(right-left)*(bottom-top));
    if(raster.depth==2&&right-left==256&&bottom-top==256){
        for(auto byte:*raster.tiles.at(coordinate))if(byte&0xaa)return;
        raster.tiles.erase(coordinate);return;
    }
    for(int y=top;y<bottom;++y)for(int x=left;x<right;++x){
        if(raster.depth==24?selection_detail::raw_pixel(raster,x,y)!=0:!selection_detail::transparent(raster,x,y,false))return;
    }
    // Native OptimizeBlock ignores latent pixels beyond the logical edge.
    raster.tiles.erase(coordinate);
}
int scaled_coordinate(double value){if(!std::isfinite(value)||value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())throw std::invalid_argument("Floating DPI mapping exceeds supported limits");return int(value);}
Raster copy_storage_map(const Raster& source,Budget& budget){
    budget.allocate(source.tiles.size()*96ull);Raster result(source.width,source.height,source.depth,source.resolution);result.color_order=source.color_order;
    for(const auto& [key,tile]:source.tiles){budget.work(1);result.tiles.emplace(key,tile);}return result;
}
Raster transfer_native_raster(const Raster& source,Raster& remainder,const Layer& owner,const Plane& plane,
    const RegionSelection& selection,Budget& budget,bool cut){
    if(selection.state==SelectionLayerState::FullPlane){auto payload=cut?source:copy_storage_map(source,budget);if(cut)remainder.tiles.clear();return payload;}
    const auto dpi_scale=plane.common.resolution/owner.common.resolution;
    const auto frame=area(selection.bounds);const auto dx=std::int64_t(selection.origin[0])-owner.common.x,dy=std::int64_t(selection.origin[1])-owner.common.y;
    const Area raw{scaled_coordinate((frame.left+dx)*dpi_scale),scaled_coordinate((frame.top+dy)*dpi_scale),
        scaled_coordinate((frame.right+dx)*dpi_scale),scaled_coordinate((frame.bottom+dy)*dpi_scale)};
    const auto request=intersect(raw,{0,0,source.width,source.height});Raster payload(source.width,source.height,source.depth,source.resolution);payload.color_order=source.color_order;
    if(request.empty())return payload;
    const bool rectangle=selection.state==SelectionLayerState::Rectangle;
    const bool copy_all=raw.left==0&&raw.top==0&&raw.right==source.width&&raw.bottom==source.height;if(copy_all)payload=cut?source:copy_storage_map(source,budget);
    if(!cut&&!copy_all)budget.allocate(source.tiles.size()*96ull);
    const auto factor=std::trunc(dpi_scale);const bool transfer=dpi_scale>=1;
    for(int ty=int(request.top/256);ty<=int((request.bottom-1)/256);++ty)for(int tx=int(request.left/256);tx<=int((request.right-1)/256);++tx){
        const TileCoord tile{tx,ty};const auto region=intersect(request,{std::int64_t(tx)*256,std::int64_t(ty)*256,std::int64_t(tx+1)*256,std::int64_t(ty+1)*256});
        const bool present=source.tiles.contains(tile);if(!present)continue;
        budget.allocate(source.tile_bytes()*(cut?2:1));budget.work(std::uint64_t(region.right-region.left)*(region.bottom-region.top));
        for(auto y=region.top;y<region.bottom;++y)for(auto x=region.left;x<region.right;++x){
            auto value=selection_detail::raw_pixel(source,int(x),int(y));
            const auto sx=transfer?std::int64_t(scaled_coordinate(double(x)/factor))-dx:0,sy=transfer?std::int64_t(scaled_coordinate(double(y)/factor))-dy:0;
            const bool physical=transfer&&selection.mask&&sx>=0&&sy>=0&&sx<selection.mask->width()&&sy<selection.mask->height();
            const bool selected=rectangle||(physical&&selection.mask->test(int(sx),int(sy)));
            if(!rectangle&&physical&&!selected)value=selection_detail::clear_word(source.depth);
            if(source.depth==32&&plane.common.opacity_mode==5&&(value&0xffffff)==0xffffff)value&=0xffffff;
            if(!copy_all||value!=selection_detail::raw_pixel(source,int(x),int(y)))selection_detail::store_raw_pixel(payload,int(x),int(y),value);
            if(cut&&selected)selection_detail::store_raw_pixel(remainder,int(x),int(y),selection_detail::clear_word(source.depth));
        }
        if(cut&&rectangle&&region.left==std::int64_t(tx)*256&&region.top==std::int64_t(ty)*256&&region.right==std::int64_t(tx+1)*256&&region.bottom==std::int64_t(ty+1)*256)
            remainder.tiles.erase(tile);
        if(!rectangle){optimize_block(payload,tile,budget);if(cut)optimize_block(remainder,tile,budget);}
    }
    return payload;
}
Raster transfer(const Raster& source,Raster& remainder,const Layer& owner,const Plane& plane,
                const RegionSelection& selection,std::array<int,2> carrier,Budget& budget,bool cut=true){
    if(source.depth<32||plane.common.resolution!=owner.common.resolution)return transfer_native_raster(source,remainder,owner,plane,selection,budget,cut);
    if(selection.state==SelectionLayerState::FullPlane){auto payload=cut?source:copy_storage_map(source,budget);if(cut)remainder.tiles.clear();return payload;}
    Raster payload(source.width,source.height,source.depth,source.resolution);payload.color_order=source.color_order;
    const auto frame=area(selection.bounds);
    const auto dx=std::int64_t(selection.origin[0])-owner.common.x,dy=std::int64_t(selection.origin[1])-owner.common.y;
    const Area raw{frame.left+dx,frame.top+dy,frame.right+dx,frame.bottom+dy};
    const auto request=intersect(raw,{0,0,source.width,source.height});
    if(request.empty())return payload;
    const bool rectangle=selection.state==SelectionLayerState::Rectangle;
    const bool copy_all=raw.left==0&&raw.top==0&&raw.right==source.width&&raw.bottom==source.height;
    if(copy_all)payload=cut?source:copy_storage_map(source,budget); // Equality before clipping.
    if(!cut&&!copy_all)budget.allocate(std::uint64_t((request.right-1)/256-request.left/256+1)*((request.bottom-1)/256-request.top/256+1)*96ull);
    for(int ty=int(request.top/256);ty<=int((request.bottom-1)/256);++ty)
        for(int tx=int(request.left/256);tx<=int((request.right-1)/256);++tx){
        const TileCoord tile{tx,ty};const std::int64_t left=std::int64_t(tx)*256,top=std::int64_t(ty)*256;
        const auto clipped=intersect(request,{left,top,left+256,top+256});
        const bool present=source.tiles.contains(tile);
        const bool full=clipped.left==left&&clipped.top==top&&clipped.right==left+256&&clipped.bottom==top+256;
        const bool copy=copy_all?present:(present||source.depth==64||!full);
        budget.work(std::uint64_t(clipped.right-clipped.left)*(clipped.bottom-clipped.top));
        if(copy&&!copy_all){budget.allocate(source.tile_bytes());payload.set_pixel(int(clipped.left),int(clipped.top),{});}
        if(cut&&rectangle&&full)remainder.tiles.erase(tile);
        bool copied_source{},copied_payload=!copy_all;
        for(auto y=clipped.top;y<clipped.bottom;++y)for(auto x=clipped.left;x<clipped.right;++x){
            const auto sx=x-dx,sy=y-dy;
            const bool mask_visits=sx>=0&&sy>=0&&sx<carrier[0]&&sy<carrier[1];
            const bool selected=rectangle||(mask_visits&&selection.mask&&sx<selection.mask->width()&&sy<selection.mask->height()&&selection.mask->test(int(sx),int(sy)));
            if(copy){
                auto color=source.pixel(int(x),int(y));
                if(source.depth==32&&plane.common.opacity_mode==5&&color.r==65535&&color.g==65535&&color.b==65535)color.a=0;
                // A missing mask block takes CAreaMask's ClrRect fast path;
                // an allocated zero block takes Set1BitColor(Transmit).
                // Both use Initialize transparent white; physical visits differ.
                if(!rectangle&&mask_visits&&!selected){
                    color=Pixel{};
                }
                if(!copy_all||color!=source.pixel(int(x),int(y))){
                    if(!copied_payload){budget.allocate(source.tile_bytes());copied_payload=true;}
                    payload.set_pixel(int(x),int(y),color);
                }
            }
            if(cut&&present&&selected&&!(rectangle&&full)){
                if(!copied_source){budget.allocate(source.tile_bytes());copied_source=true;}
                // Native ClrRect mode1 ignores its zero argument and uses the
                // Initialize clear colour: transparent white in32/64bit.
                remainder.set_pixel(int(x),int(y),Pixel{});
            }
        }
        // CAreaMask optimizes every request-touched destination tile, even
        // when the carrier clips away all pixel visits or no bit matched.
        if(!rectangle){optimize_block(payload,tile,budget);if(cut)optimize_block(remainder,tile,budget);}
    }
    return payload;
}
}
std::shared_ptr<const FloatingSelection> floating_history_snapshot(const FloatingSelection& live){
    if(!live.active())return {};
    auto snapshot=std::make_shared<FloatingSelection>(live);snapshot->capture_frame={};snapshot->same_document_source=false;return snapshot;
}
namespace {
FloatingCapture capture(const DocumentData& data,const RegionSelection& selection,Budget& budget,bool live=false,std::vector<FloatingSource>* history=nullptr,bool current_only=false){
    if(!selection.active()&&!live)return {data,{},false};
    const auto carrier=selection_carrier_size(selection,data.base.width(),data.base.height());
    const auto frame=area(selection.bounds);
    const Area document_frame{frame.left+selection.origin[0],frame.top+selection.origin[1],frame.right+selection.origin[0],frame.bottom+selection.origin[1]};
    for(auto value:{document_frame.left,document_frame.top,document_frame.right,document_frame.bottom})
        if(value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())throw std::invalid_argument("浮动选区坐标超出支持范围。");
    const bool one_layer=std::count_if(data.layers.begin(),data.layers.end(),[](const Layer& layer){return layer.common.flags[2]!=0;})==1;
    std::vector<FloatingSource> targets;
    std::vector<FloatingPaintSourceHistory> paint_sources;
    for(std::size_t i=0;i<data.layers.size();++i){
        const auto& layer=data.layers[i];if(!layer.common.flags[1]||!layer.common.flags[2]||!layer.common.runtime_operable)continue;
        if(layer.family==0x40000200||layer.family==0x40001000||layer.family==0x40000800u||layer.family==0x48040800u||layer.family==0x40020000u||(current_only&&int(i)!=data.current_layer))continue;
        const Area bounds{layer.common.x,layer.common.y,std::int64_t(layer.common.x)+std::max(0,layer.common.width),std::int64_t(layer.common.y)+std::max(0,layer.common.height)};
        if(selection.active()&&intersect(document_frame,bounds).empty()
            &&((selection.vector_flags&1u)||!(layer.family&0x20000000u)))continue;
        if(history){budget.allocate(sizeof(FloatingSource));history->push_back({i,{}});}
        if(!budget.transform&&layer.family==0x21200000){
            if((selection.vector_flags&1u)?!selection.active():!layer.common.flags[3])continue;
            //672813 sets A8 before the category test. 673750 binds the
            //Paint source even when no child ran or its worker returned0.
            budget.allocate(sizeof(FloatingPaintSourceHistory));budget.work(1);
            paint_sources.push_back({i,selection.state==SelectionLayerState::FullPlane?std::nullopt:std::optional<VectorBounds>{VectorBounds{}}});
            if(!(selection.vector_flags&0x300u))continue;
        }
        FloatingSource target{i,{}};
        for(std::size_t p=0;p<layer.planes.size();++p){
            const auto& plane=layer.planes[p];if(!plane.common.flags[1]||(one_layer&&!plane.common.flags[2])||(current_only&&int(p)!=layer.current_plane))continue;
            if(layer.family==0x21200000&&((plane.family==0x21110001||plane.family==0x21120001)?!(selection.vector_flags&0x100u):!(selection.vector_flags&0x200u)))continue;
            //208 does not enter the owner-level Paint special case. Its
            //Paint/Pure children fail the ordinary Vert RTTI gate; other
            //Select helper child profiles retain explicit modern admission.
            if(layer.family==0x20800000&&!budget.transform&&(plane.family==0x21110001||plane.family==0x21120001||plane.family==0x22100001||plane.family==0x22100002))continue;
            if((selection_detail::vector_move_owner(layer)||selection_detail::extension_move_owner(layer))&&!budget.transform){
                //672300's ordinary high-nibble2 path dynamic-casts to real
                //Vert-Line. Native Pure-Line siblings fail that RTTI gate.
                if((layer.family==0x22100000||layer.family==0x24400000)&&(plane.family==0x21110001||plane.family==0x21120001))continue;
                if((layer.family==0x22100000||layer.family==0x24400000)
                    &&((selection.vector_flags&1u)?!selection.active():!plane.common.flags[3]))continue;
                if(plane.opaque||plane.raster||(plane.family!=0x21210001&&plane.family!=0x21220001&&plane.family!=0x22100001&&plane.family!=0x22100002&&plane.family!=0x21110001&&plane.family!=0x21120001&&plane.family!=0x40000001&&plane.family!=0x40000002))throw std::invalid_argument("Unsupported vector floating child");
            }else{if(!selection.active())continue;validate(layer,plane,data.base.dpi,budget.transform);}if(selection_detail::extension_move_owner(layer)&&!plane.common.flags[3])continue;target.planes.push_back(p);
        }
        // Paint's owner worker still converts selection geometry with an
        // empty child map. In particular, bitmap Face capture can change
        // source topology/H and context+78 without transferring any child.
        if(history){budget.allocate(target.planes.size()*sizeof(std::size_t));history->back().planes=target.planes;}
        if(!target.planes.empty()||(!budget.transform&&layer.family==0x21200000))targets.push_back(std::move(target));
    }
    if(budget.transform){
        // Precharge all request visits and both Optimize passes before the
        // remainder copy or transfer loops. Missing physical blocks still
        // incur the native carrier/request walk. This upper bound deliberately
        // includes skipped Optimize visits; ordinary capture keeps its ledger.
        for(const auto& target:targets)for(const auto index:target.planes){const auto& owner=data.layers[target.layer];const auto& raster=*owner.planes[index].raster;const auto request=intersect({document_frame.left-owner.common.x,document_frame.top-owner.common.y,document_frame.right-owner.common.x,document_frame.bottom-owner.common.y},{0,0,raster.width,raster.height});if(!request.empty()&&selection.state!=SelectionLayerState::FullPlane)budget.work(capture_visits(request,raster,selection.state==SelectionLayerState::Rectangle));}
        std::uint64_t metadata=sizeof(data)+data.preview.size()+data.trailing.size()+data.layer_suffix.size()+data.base.suffix.size();for(const auto& l:data.layers){metadata+=sizeof(l)+l.common.name.size()+l.common.suffix.size()+l.specific.size()+l.suffix.size()+l.plane_suffix.size()+l.opaque_record.size();for(const auto& p:l.planes)metadata+=sizeof(p)+p.common.name.size()+p.common.suffix.size()+p.specific.size()+p.suffix.size()+p.opaque_record.size()+(p.raster?p.raster->tiles.size()*128:0);}budget.allocate(metadata);budget.capture_precharged=true;
    }
    FloatingCapture result{data,{},!paint_sources.empty()};result.paint_source_history=std::move(paint_sources);if(targets.empty())return result;
    auto layers=std::make_shared<std::vector<Layer>>();layers->reserve(targets.size());
    std::vector<FloatingSource> captured_targets;captured_targets.reserve(targets.size());
    auto captured_frame=area(selection.document_bounds());
    std::size_t paint_source{};
    for(const auto& target:targets){
        const auto& source=data.layers[target.layer];auto layer=source;layer.planes.clear();layer.current_plane=0;
        bool captured{};
        if(selection_detail::extension_move_owner(source)){layer=selection_detail::capture_extension_move(result.remainder.layers[target.layer],source,target.planes,selection,data.base.dpi,budget.bytes,budget.operations,captured);if(captured){result.source_history=true;layers->push_back(std::move(layer));captured_targets.push_back(target);}continue;}
        if(selection_detail::vector_move_owner(source)){
            Rect bounds;VectorBounds history_damage;layer=selection_detail::capture_vector_move(result.remainder.layers[target.layer],source,target.planes,selection,data.base.dpi,budget.bytes,budget.operations,captured,bounds,history_damage);
            while(paint_source<result.paint_source_history.size()&&result.paint_source_history[paint_source].layer<target.layer)++paint_source;
            if(paint_source<result.paint_source_history.size()&&result.paint_source_history[paint_source].layer==target.layer){auto& binding=result.paint_source_history[paint_source];if(binding.raw_damage)binding.raw_damage=history_damage;}
            if(captured){if(!area(bounds).empty())captured_frame=unite(captured_frame,area(bounds));result.source_history=true;layers->push_back(std::move(layer));captured_targets.push_back(target);}continue;
        }
        for(const auto index:target.planes){
            auto plane=source.planes[index];auto& remainder=result.remainder.layers[target.layer].planes[index];
            plane.raster=transfer(*source.planes[index].raster,*remainder.raster,source,source.planes[index],selection,carrier,budget);
            plane.pixels_changed=true;remainder.pixels_changed=true;result.source_history=true;
            if(int(index)==source.current_plane)layer.current_plane=int(layer.planes.size());
            layer.planes.push_back(std::move(plane));
        }
        layers->push_back(std::move(layer));captured_targets.push_back(target);
    }
    if(layers->empty())return result;
    auto floating=std::make_shared<FloatingSelection>();floating->kind=FloatingKind::General;floating->layers=std::move(layers);
    floating->sources=std::make_shared<const std::vector<FloatingSource>>(std::move(captured_targets));floating->same_document_source=true;
    floating->capture_frame=selection.document_bounds();floating->tight_bounds={coordinate(captured_frame.left),coordinate(captured_frame.top),coordinate(captured_frame.right-captured_frame.left),coordinate(captured_frame.bottom-captured_frame.top)};
    // sub_61BE70 classifies no-carrier single PaintMono payloads separately.
    const auto& payload=*floating->layers;
    if(payload.size()==1&&payload[0].family==0x11000800&&payload[0].planes.size()<=2
        &&std::none_of(payload[0].planes.begin(),payload[0].planes.end(),[](const Plane& plane){return plane.family==0x10000001;})
        &&std::any_of(data.layers.begin(),data.layers.end(),[](const Layer& layer){return !layer.opaque&&layer.family==0x11000800;}))
        floating->kind=FloatingKind::PaintMono;
    result.floating=std::move(floating);return result;
}
}
namespace {
void charge_capture_source(const DocumentData& data,Budget& budget){
    // Raster tiles and original-file witnesses are immutable/COW-shared by
    // these copies. Transfers charge newly allocated backing separately.
    std::uint64_t metadata{};modal_detail::data_charge(data,metadata,0,false);budget.allocate(metadata*3);budget.work(metadata);
}
bool refresh_capture_marks(DocumentData& data,bool active,const RegionSelection& selection,Budget& budget){
    std::uint64_t storage=512ull*1024*1024-budget.bytes,work=128ull*1024*1024-budget.operations;
    const bool mask=selection.state==SelectionLayerState::Mask||selection.state==SelectionLayerState::VectorIndicated;
    const double dpi=mask&&!(selection.vector_flags&1u)?selection_carrier_dpi(selection,data.base.dpi):0;
    const bool result=vector_indication_detail::refresh_marks(data,active,selection,selection.vector_flags,dpi,storage,work,budget.stop);
    budget.bytes=512ull*1024*1024-storage;budget.operations=128ull*1024*1024-work;return result;
}
FloatingCapture live_capture(const DocumentData& data,const RegionSelection& selection,bool active,Budget& budget){
    auto result=capture(data,selection,budget,true);
    result.indication_active=refresh_capture_marks(result.remainder,active,selection,budget);
    result.indication_redraw=active||result.indication_active;return result;
}
}
FloatingCapture prepare_live_floating_capture(const DocumentData& data,const RegionSelection& selection,bool active,std::stop_token stop){
    Budget budget;budget.stop=stop;budget.poll();charge_capture_source(data,budget);return live_capture(data,selection,active,budget);
}
namespace floating_detail {
Content capture_content(const DocumentData& data,const RegionSelection& selection,std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop,bool current_only){
    if(bytes>modal_detail::max_bytes||work>modal_detail::max_work)throw std::length_error("Floating capture ledger already exhausted");
    Budget budget{modal_detail::max_work-work,modal_detail::max_bytes-bytes,stop};budget.poll();charge_capture_source(data,budget);
    Content result;result.capture=capture(data,selection,budget,true,&result.sources,current_only);budget.poll();bytes=modal_detail::max_bytes-budget.bytes;work=modal_detail::max_work-budget.operations;return result;
}
void finish_capture(FloatingCapture& result,const RegionSelection& selection,bool active,std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop){
    if(bytes>modal_detail::max_bytes||work>modal_detail::max_work)throw std::length_error("Floating capture ledger already exhausted");
    Budget budget{modal_detail::max_work-work,modal_detail::max_bytes-bytes,stop};
    result.indication_active=refresh_capture_marks(result.remainder,active,selection,budget);result.indication_redraw=active||result.indication_active;
    budget.poll();bytes=modal_detail::max_bytes-budget.bytes;work=modal_detail::max_work-budget.operations;
}
}
FloatingCapture prepare_floating_capture(const DocumentData& data,const RegionSelection& selection){
    if(!selection.active())return {data,{},false};
    Budget budget;charge_capture_source(data,budget);auto marked=data;
    const bool active=selection.active()?refresh_capture_marks(marked,false,selection,budget):false;
    return live_capture(marked,selection,active,budget);
}
namespace {
void writable_copy_tile(Raster& target,TileCoord key,int x,int y,Budget& budget){
    const auto found=target.tiles.find(key);
    if(found==target.tiles.end()||found->second.use_count()!=1){
        budget.allocate(target.tile_bytes()+(found==target.tiles.end()?96:0));
        budget.work(target.tile_bytes());
        selection_detail::store_raw_pixel(target,x,y,selection_detail::raw_pixel(target,x,y));
    }
}
void materialize_low_plane(const Raster&,Raster&,std::int64_t,std::int64_t,Budget&);
Raster paint_copy_payload(const Layer& owner,const Plane& primary,const Plane& companion,const RegionSelection& selection,std::array<int,2> carrier,Budget& budget){
    if(!primary.raster||!companion.raster)throw std::invalid_argument("Paint COPY fold requires decoded primary and companion");
    const auto dx=std::int64_t(selection.origin[0])-owner.common.x,dy=std::int64_t(selection.origin[1])-owner.common.y;
    const auto frame=area(selection.bounds);const auto scale=primary.common.resolution/owner.common.resolution;const Area raw{scaled_coordinate((frame.left+dx)*scale),scaled_coordinate((frame.top+dy)*scale),scaled_coordinate((frame.right+dx)*scale),scaled_coordinate((frame.bottom+dy)*scale)};
    for(auto value:{raw.left,raw.top,raw.right,raw.bottom})(void)coordinate(value);
    auto rectangle=selection;if(rectangle.state==SelectionLayerState::Mask||rectangle.state==SelectionLayerState::VectorIndicated)rectangle.state=SelectionLayerState::Rectangle;
    Plane raw_plane;raw_plane.common.resolution=primary.common.resolution;raw_plane.common.opacity_mode=1;Raster unused;
    auto payload=transfer(*primary.raster,unused,owner,raw_plane,rectangle,carrier,budget,false);
    const auto& source=*companion.raster;
    if(payload.depth<32){
        //5EAC70 owns a fresh destination-format temporary. Restrict its
        //three-pass result to the exact projected ROI before copying back.
        Raster cropped(source.width,source.height,source.depth,source.resolution);cropped.color_order=source.color_order;
        const auto request=intersect(raw,{0,0,source.width,source.height});
        for(const auto& [key,bytes]:source.tiles){(void)bytes;const auto region=intersect(request,{key.x*256ll,key.y*256ll,(key.x+1)*256ll,(key.y+1)*256ll});if(region.empty())continue;
            budget.work(std::uint64_t(region.right-region.left)*(region.bottom-region.top));budget.allocate(source.tile_bytes()+96);
            for(auto y=region.top;y<region.bottom;++y){budget.poll();for(auto x=region.left;x<region.right;++x)selection_detail::store_raw_pixel(cropped,int(x),int(y),selection_detail::raw_pixel(source,int(x),int(y)));}
        }
        auto folded=copy_storage_map(payload,budget);materialize_low_plane(cropped,folded,0,0,budget);
        const auto output=intersect(raw,{0,0,payload.width,payload.height});
        if(!output.empty()){budget.work(std::uint64_t(output.right-output.left)*(output.bottom-output.top));for(auto y=output.top;y<output.bottom;++y){budget.poll();for(auto x=output.left;x<output.right;++x){const TileCoord key{int(x)/256,int(y)/256};if(!folded.tiles.contains(key))continue;writable_copy_tile(payload,key,int(x),int(y),budget);selection_detail::store_raw_pixel(payload,int(x),int(y),selection_detail::raw_pixel(folded,int(x),int(y)));}}}
    }else{
        //CommonCopy3 clips a negative destination without advancing its
        //source cursor. Source and destination can therefore have different
        //coordinates even though the call supplied equal origins.
        const auto output=intersect(raw,{0,0,payload.width,payload.height});
        if(!output.empty()){budget.work(std::uint64_t(output.right-output.left)*(output.bottom-output.top));
            for(auto y=output.top;y<output.bottom;++y){budget.poll();for(auto x=output.left;x<output.right;++x){
                const auto sx=raw.left+x-output.left,sy=raw.top+y-output.top;
                if(sx<0||sy<0||sx>=source.width||sy>=source.height||!source.tiles.contains({int(sx)/256,int(sy)/256}))continue;
                const TileCoord key{int(x)/256,int(y)/256};writable_copy_tile(payload,key,int(x),int(y),budget);if(source.depth<=8)continue;
                const auto sample=raster_stroke_surface_pixel(source,int(sx),int(sy));if(!(sample.a>>8)||((sample.r>>8)==255&&(sample.g>>8)==255&&(sample.b>>8)==255))continue;
                payload.set_pixel(int(x),int(y),writeback_pixel(payload.pixel(int(x),int(y)),sample,payload.depth,false));
            }}
        }
    }
    if(selection.state==SelectionLayerState::FullPlane)return payload;
    const bool keyed=payload.depth==32&&primary.common.opacity_mode==5,masked=selection.state==SelectionLayerState::Mask||selection.state==SelectionLayerState::VectorIndicated;
    const auto request=intersect(raw,{0,0,payload.width,payload.height});
    if(request.empty()||(!keyed&&!masked))return payload;
    for(int ty=int(request.top/256);ty<=int((request.bottom-1)/256);++ty)for(int tx=int(request.left/256);tx<=int((request.right-1)/256);++tx){budget.poll();
        const TileCoord key{tx,ty};if(!payload.tiles.contains(key))continue;
        const auto region=intersect(request,{std::int64_t(tx)*256,std::int64_t(ty)*256,std::int64_t(tx+1)*256,std::int64_t(ty+1)*256});
        budget.work(std::uint64_t(region.right-region.left)*(region.bottom-region.top)*(unsigned(keyed)+unsigned(masked)));
        writable_copy_tile(payload,key,int(region.left),int(region.top),budget);
        for(auto y=region.top;y<region.bottom;++y){budget.poll();for(auto x=region.left;x<region.right;++x){
            auto value=selection_detail::raw_pixel(payload,int(x),int(y));if(keyed&&(value&0xffffffu)==0xffffffu)value&=0xffffffu;
            const auto factor=std::trunc(scale);const bool transfer=scale>=1;
            const auto sx=transfer?std::int64_t(scaled_coordinate(double(x)/factor))-dx:0,sy=transfer?std::int64_t(scaled_coordinate(double(y)/factor))-dy:0;
            const bool visits=transfer&&sx>=0&&sy>=0&&sx<carrier[0]&&sy<carrier[1];
            if(masked&&visits&&(!selection.mask||sx>=selection.mask->width()||sy>=selection.mask->height()||!selection.mask->test(int(sx),int(sy))))value=selection_detail::clear_word(payload.depth);
            selection_detail::store_raw_pixel(payload,int(x),int(y),value);
        }}
        if(masked)optimize_block(payload,key,budget);
    }
    return payload;
}
}
Raster selection_copy_detail::raster_payload(const Layer& owner,const Plane& plane,const RegionSelection& selection,std::array<int,2> carrier,std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop,const Plane* companion){
    if(bytes>modal_detail::max_bytes||work>modal_detail::max_work)throw std::length_error("COPY ledger already exhausted");
    Budget budget{modal_detail::max_work-work,modal_detail::max_bytes-bytes,stop};budget.poll();Raster unused;
    auto result=companion?paint_copy_payload(owner,plane,*companion,selection,carrier,budget):transfer(*plane.raster,unused,owner,plane,selection,carrier,budget,false);
    for(auto& [key,tile]:result.tiles){(void)key;budget.allocate(tile->size());budget.work(tile->size());tile=std::make_shared<Bytes>(*tile);}
    budget.poll();bytes=modal_detail::max_bytes-budget.bytes;work=modal_detail::max_work-budget.operations;return result;
}
void admit_transform_floating_capture_work(const DocumentData& data,const RegionSelection& selection,std::uint64_t remaining){
    if(!selection.active()||selection.state==SelectionLayerState::FullPlane)return;
    const auto carrier=selection_carrier_size(selection,data.base.width(),data.base.height());(void)carrier;
    const auto bounds=area(selection.bounds);const Area frame{bounds.left+selection.origin[0],bounds.top+selection.origin[1],bounds.right+selection.origin[0],bounds.bottom+selection.origin[1]};
    const bool one_layer=std::count_if(data.layers.begin(),data.layers.end(),[](const Layer& l){return l.common.flags[2]!=0;})==1;
    for(const auto& owner:data.layers){if(!owner.common.flags[1]||!owner.common.flags[2]||!owner.common.runtime_operable||owner.family==0x40000200||owner.family==0x40001000)continue;const Area owner_bounds{owner.common.x,owner.common.y,std::int64_t(owner.common.x)+std::max(0,owner.common.width),std::int64_t(owner.common.y)+std::max(0,owner.common.height)};if(intersect(frame,owner_bounds).empty()&&!(owner.family&0x20000000u))continue;
        for(const auto& plane:owner.planes){if(!plane.common.flags[1]||(one_layer&&!plane.common.flags[2]))continue;validate(owner,plane,data.base.dpi,true);const auto& raster=*plane.raster;const auto request=intersect({frame.left-owner.common.x,frame.top-owner.common.y,frame.right-owner.common.x,frame.bottom-owner.common.y},{0,0,raster.width,raster.height});if(request.empty())continue;const auto work=capture_visits(request,raster,selection.state==SelectionLayerState::Rectangle);if(work>remaining)throw std::length_error("Transform capture work exceeds aggregate budget");remaining-=work;}}
}
FloatingCapture prepare_transform_floating_capture(const DocumentData& data,const RegionSelection& selection,std::uint64_t& work_remaining,std::uint64_t& bytes_remaining,std::stop_token stop){Budget budget{work_remaining,bytes_remaining,stop,true};budget.poll();auto result=capture(data,selection,budget);budget.poll();work_remaining=budget.operations;bytes_remaining=budget.bytes;return result;}

namespace {
int coordinate(std::int64_t value){
    if(value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())
        throw std::invalid_argument("浮动图像写回坐标超出支持范围。");
    return int(value);
}
Area unite(Area a,Area b){
    if(a.empty())return b;if(b.empty())return a;
    return {std::min(a.left,b.left),std::min(a.top,b.top),std::max(a.right,b.right),std::max(a.bottom,b.bottom)};
}
Area block_extent(const Raster& raster){
    Area result;
    for(const auto& [tile,bytes]:raster.tiles){
        const auto x=std::int64_t(tile.x)*256,y=std::int64_t(tile.y)*256;
        result=unite(result,{x,y,std::min<std::int64_t>(x+256,raster.width),std::min<std::int64_t>(y+256,raster.height)});
    }
    return result;
}
// Same-DPI CommonCopy3 samples even 64-bit sources through Get32BitColorFast.
// Keep these native mixers distinct from the generic straight-alpha adapter.
void materialize_low_plane(const Raster& payload,Raster& destination,std::int64_t dx,std::int64_t dy,Budget& budget){
    Raster temp(destination.width,destination.height,destination.depth,destination.resolution);temp.color_order=destination.color_order;
    //5EAC70's three passes: Copy payload into fresh temporary, CommonCopy
    //the old target behind it, then Copy the complete temporary into target.
    //Both passes visit physical source blocks, including allocated-zero ones.
    if(destination.depth!=payload.depth){
        // CommonCopy(mode1) converts via Get32/Set32 and visits missing source
        // blocks as well. A missing source invokes the destination clear
        // setter; backed transparent samples retain their latent RGB.
        const auto roi=intersect({0,0,payload.width,payload.height},{-dx,-dy,std::int64_t(temp.width)-dx,std::int64_t(temp.height)-dy});
        if(!roi.empty())budget.work(std::uint64_t(roi.right-roi.left)*(roi.bottom-roi.top));
        for(auto y=roi.top;!roi.empty()&&y<roi.bottom;++y){budget.poll();for(auto x=roi.left;x<roi.right;++x){
            const int tx=coordinate(x+dx),ty=coordinate(y+dy);const TileCoord key{tx/256,ty/256};
            if(!temp.tiles.contains(key))budget.allocate(temp.tile_bytes());
            std::uint64_t value=selection_detail::clear_word(temp.depth);
            if(payload.tiles.contains({int(x)/256,int(y)/256})){
                const auto p=raster_stroke_surface_pixel(payload,int(x),int(y));
                const unsigned r=p.r>>8,g=p.g>>8,b=p.b>>8,a=p.a>>8,l=(r+4*g+2*b)/7,k=((255-l)*(a+1))>>8;
                value=temp.depth==1?std::uint64_t(k>127):temp.depth==2?std::uint64_t(a<=127?0:l<127?3:2):
                    temp.depth==8?std::uint64_t(k):temp.depth==16?std::uint64_t(a)|(std::uint64_t(l)<<8):
                    payload.depth==8?std::uint64_t(255-a)*0x010101ull:std::uint64_t(b)|(std::uint64_t(g)<<8)|(std::uint64_t(r)<<16);
            }
            selection_detail::store_raw_pixel(temp,tx,ty,value);
        }
            //Only the missing-source mode1 clear loop advances its terminal
            //cursor. Backed Get32/Set32 guards the last pixel before NextX.
            const auto next_x=roi.right+dx,next_y=y+dy;
            if(dx%256&&next_x%256==0&&next_x>=0&&next_x<temp.width
                &&!payload.tiles.contains({int(roi.right-1)/256,int(y)/256})){
                const TileCoord key{int(next_x)/256,int(next_y)/256};if(!temp.tiles.contains(key))budget.allocate(temp.tile_bytes());
                selection_detail::store_raw_pixel(temp,int(next_x),int(next_y),selection_detail::clear_word(temp.depth));
            }
        }
    }else for(const auto& [tile,bytes]:payload.tiles){
        const auto roi=intersect({std::int64_t(tile.x)*256,std::int64_t(tile.y)*256,std::int64_t(tile.x+1)*256,std::int64_t(tile.y+1)*256},
            {std::max<std::int64_t>(0,-dx),std::max<std::int64_t>(0,-dy),std::min<std::int64_t>(payload.width,std::int64_t(temp.width)-dx),std::min<std::int64_t>(payload.height,std::int64_t(temp.height)-dy)});
        if(roi.empty())continue;budget.work(std::uint64_t(roi.right-roi.left)*(roi.bottom-roi.top));
        for(auto y=roi.top;y<roi.bottom;++y){budget.poll();for(auto x=roi.left;x<roi.right;++x){const int tx=coordinate(x+dx),ty=coordinate(y+dy);const TileCoord key{tx/256,ty/256};
            if(!temp.tiles.contains(key))budget.allocate(temp.tile_bytes());selection_detail::store_raw_pixel(temp,tx,ty,selection_detail::raw_pixel(payload,int(x),int(y)));}}
    }
    for(const auto& [tile,bytes]:destination.tiles){
        const auto roi=intersect({std::int64_t(tile.x)*256,std::int64_t(tile.y)*256,std::int64_t(tile.x+1)*256,std::int64_t(tile.y+1)*256},{0,0,temp.width,temp.height});
        if(roi.empty())continue;budget.work(std::uint64_t(roi.right-roi.left)*(roi.bottom-roi.top));
        if(!temp.tiles.contains(tile))budget.allocate(temp.tile_bytes());
        for(auto y=roi.top;y<roi.bottom;++y){budget.poll();for(auto x=roi.left;x<roi.right;++x){const auto output=low_copy_pixel(selection_detail::raw_pixel(temp,int(x),int(y)),selection_detail::raw_pixel(destination,int(x),int(y)),temp.depth);
            selection_detail::store_raw_pixel(temp,int(x),int(y),output);}}
    }
    destination=std::move(temp);
}

}
void validate_local_floating(const DocumentData& data,const FloatingSelection& floating){
    if(floating.target){validate_targeted_floating(data,floating);return;}
    if(!floating.active()||!floating.same_document_source||floating.target||!floating.layers||!floating.sources||floating.layers->size()!=floating.sources->size())
        throw std::invalid_argument("此浮动图像来源或目标绑定尚未接入写回。");
    // Validate the complete identity map before preparing any destination.
    for(std::size_t i=0;i<floating.layers->size();++i){
        const auto& source=(*floating.layers)[i];const auto& mapping=(*floating.sources)[i];
        if(mapping.layer>=data.layers.size()||source.family!=data.layers[mapping.layer].family||source.planes.size()!=mapping.planes.size())
            throw std::invalid_argument("浮动图像的原始图层已被替换。");
        if(selection_detail::extension_move_owner(source)){std::uint64_t bytes=256ull*1024*1024,work=128ull*1024*1024;selection_detail::validate_extension_move(source,data.base.dpi,bytes,work);selection_detail::validate_extension_move(data.layers[mapping.layer],data.base.dpi,bytes,work);}
        else if(selection_detail::vector_move_owner(source)){std::uint64_t bytes=256ull*1024*1024,work=128ull*1024*1024;selection_detail::validate_vector_move(source,data.base.dpi,bytes,work);selection_detail::validate_vector_move(data.layers[mapping.layer],data.base.dpi,bytes,work);}
        for(std::size_t p=0;p<source.planes.size();++p){
            const auto target=mapping.planes[p];const auto& owner=data.layers[mapping.layer];
            if(target>=owner.planes.size()||source.planes[p].family!=owner.planes[target].family)
                throw std::invalid_argument("浮动图像的原始平面已被替换。");
            if(!selection_detail::vector_move_owner(source)&&!selection_detail::extension_move_owner(source)){validate(source,source.planes[p],data.base.dpi);validate(owner,owner.planes[target],data.base.dpi);}
        }
    }
}
void validate_targeted_floating(const DocumentData& data,const FloatingSelection& floating){
    if(!floating.active()||!floating.target||!floating.layers||floating.layers->empty()||floating.same_document_source||floating.sources
        ||(floating.kind!=FloatingKind::General&&floating.kind!=FloatingKind::TargetedPaintMono))throw std::invalid_argument("Invalid explicit floating binding");
    const auto binding=*floating.target;if(binding.layer>=data.layers.size()||binding.plane>=data.layers[binding.layer].planes.size())throw std::invalid_argument("Floating target was removed");
    const auto& owner=data.layers[binding.layer];const auto& target=owner.planes[binding.plane];
    if(!owner.common.flags[1]||!target.common.flags[1]||owner.opaque||target.opaque)throw std::invalid_argument("Floating target is hidden or opaque");
    std::uint64_t bytes=256ull*1024*1024,work=128ull*1024*1024;
    if(target.raster)validate(owner,target,data.base.dpi);
    else if(selection_detail::extension_move_owner(owner))selection_detail::validate_extension_move(owner,data.base.dpi,bytes,work);
    else if(selection_detail::vector_move_owner(owner)&&target.family!=0x24100001)selection_detail::validate_vector_move(owner,data.base.dpi,bytes,work);
    else if(target.family!=0x24100001)throw std::invalid_argument("Unsupported explicit target family");
    for(const auto& source:*floating.layers){
        if(selection_detail::extension_move_owner(source))selection_detail::validate_extension_move(source,data.base.dpi,bytes,work);
        else if(selection_detail::vector_move_owner(source))selection_detail::validate_vector_move(source,data.base.dpi,bytes,work);
        for(const auto& plane:source.planes){if(plane.raster)validate(source,plane,data.base.dpi);
            if(!floating_target_family_compatible(target.family,plane.family))throw std::invalid_argument("Explicit floating source family is incompatible with its target");}
    }
}
bool floating_target_family_compatible(std::uint32_t target,std::uint32_t source) noexcept{
    const auto cluster_a=[](auto f){return f==0x11210001||f==0x11220001||f==0x11240001;};
    const auto cluster_b=[](auto f){return f==0x21110001||f==0x21120001;};const auto cluster_c=[](auto f){return f==0x21210001||f==0x21220001;};
    if(cluster_a(target))return cluster_a(source);if(cluster_b(target))return cluster_b(source);if(cluster_c(target))return cluster_c(source);
    return target!=0x22100001||source==target;
}
namespace {
FloatingMaterialization materialize(const DocumentData& data,const FloatingSelection& floating,Budget& budget){
    if(!floating.active())return {data,false};
    validate_local_floating(data,floating);
    if(budget.transform){std::uint64_t metadata=sizeof(data)+data.preview.size()+data.trailing.size()+data.layer_suffix.size()+data.base.suffix.size();for(const auto& l:data.layers){metadata+=sizeof(l)+l.common.name.size()+l.common.suffix.size()+l.specific.size()+l.suffix.size()+l.plane_suffix.size()+l.opaque_record.size();for(const auto& p:l.planes)metadata+=sizeof(p)+p.common.name.size()+p.common.suffix.size()+p.specific.size()+p.suffix.size()+p.opaque_record.size()+(p.raster?p.raster->tiles.size()*128:0);}budget.allocate(metadata);}
    FloatingMaterialization result{data,false};
    for(std::size_t i=0;i<floating.layers->size();++i){
        const auto& source=(*floating.layers)[i];FloatingSource bound;
        if(floating.target){bound.layer=floating.target->layer;bound.planes.assign(source.planes.size(),floating.target->plane);}
        const auto& mapping=floating.target?bound:(*floating.sources)[i];
        auto& owner=result.data.layers[mapping.layer];Area extent;
        if(floating.target){
            const auto target_index=floating.target->plane;const auto& target=owner.planes[target_index];
            if(target.family==0x40000001||target.family==0x40000002||target.family==0x24100001)continue; //native fixed-zero +5C/+60
            if(!target.raster){result.content_history|=selection_detail::materialize_vector_target(owner,target_index,source,floating.delta,data.base.dpi,budget.bytes,budget.operations);continue;}
            if(source.family==0x40000800u||source.family==0x48040800u){
                Layer carrier=source;carrier.family=0x10000100u;carrier.planes.clear();carrier.common.x=carrier.common.y=0;
                const auto width=std::max<std::int64_t>(data.base.width(),std::int64_t(floating.tight_bounds.x)+floating.tight_bounds.width),height=std::max<std::int64_t>(data.base.height(),std::int64_t(floating.tight_bounds.y)+floating.tight_bounds.height);
                if(width<=0||height<=0||width>100000||height>100000)throw std::invalid_argument("Text clipboard raster geometry exceeds bounds");
                budget.allocate(std::uint64_t((width+255)/256)*((height+255)/256)*256*256*4);budget.work(std::uint64_t(width)*height*source.planes.size());
                Raster glyphs(int(width),int(height),32,data.base.dpi);
                for(std::size_t i=source.planes.size();i-->0;){const auto& p=source.planes[i];if(!p.common.flags[1])continue;const auto* masks=text_font_detail::image_masks(source,p,data.base.dpi);if(!masks)throw std::invalid_argument("Text clipboard lowering requires a prepared font consumer");
                    resolution_detail::Budget b{512ull*1024*1024-budget.bytes,128ull*1024*1024-budget.operations,{}};auto view=p.runtime_text_cache->image_geometry?text_font_detail::ordinary_view_masks(source,p,data.base.dpi,b):*masks;
                    budget.bytes=512ull*1024*1024-b.bytes;budget.operations=128ull*1024*1024-b.work;text_font_detail::paint_display_masks(glyphs,view,source.common.opacity,p.runtime_text_cache->image_geometry?std::array<int,2>{}:std::array<int,2>{source.common.x,source.common.y});}
                Plane image;image.family=0x10000001u;image.common=source.planes.front().common;image.common.width=carrier.common.width=int(width);image.common.height=carrier.common.height=int(height);image.common.depth=carrier.common.depth=32;image.common.resolution=data.base.dpi;image.common.opacity_mode=1;image.raster=std::move(glyphs);carrier.planes.push_back(std::move(image));
                FloatingSelection converted=floating;converted.layers=std::make_shared<const std::vector<Layer>>(std::vector{std::move(carrier)});
                auto written=materialize(result.data,converted,budget);result.data=std::move(written.data);result.content_history|=written.content_history;continue;
            }
            const bool line_source=std::ranges::any_of(source.planes,[](const auto& p){return p.family==0x11210001||p.family==0x11220001||p.family==0x11240001;});
            if(selection_detail::vector_move_owner(source)||selection_detail::extension_move_owner(source)||(line_source&&(target.family&0x00f00000u)!=0x00200000u)){
                const auto render_owner=owner;const auto render_target=target;
                for(std::size_t child=0;child<source.planes.size();++child){const auto& p=source.planes[child];Raster tmp=p.raster?selection_detail::clipboard_line_raster(source,p,render_owner,render_target,floating.delta,budget.bytes,budget.operations):selection_detail::clipboard_vector_raster(source,child,render_owner,render_target,floating.delta,budget.bytes,budget.operations);
                    Layer carrier=render_owner;carrier.family=0x10000100;Plane incoming=render_target;incoming.common.opacity_mode=1;incoming.common.depth=tmp.depth;incoming.raster=std::move(tmp);carrier.planes={std::move(incoming)};
                    FloatingSelection converted;converted.kind=FloatingKind::General;converted.target=floating.target;converted.layers=std::make_shared<const std::vector<Layer>>(std::vector{std::move(carrier)});converted.capture_frame=converted.tight_bounds={0,0,render_target.raster->width,render_target.raster->height};
                    const int old_mode=result.data.layers[mapping.layer].planes[target_index].common.opacity_mode;result.data.layers[mapping.layer].planes[target_index].common.opacity_mode=1;
                    auto written=materialize(result.data,converted,budget);written.data.layers[mapping.layer].planes[target_index].common.opacity_mode=old_mode;result.data=std::move(written.data);result.content_history|=written.content_history;
                }continue;
            }
        }
        if(selection_detail::extension_move_owner(source)){selection_detail::materialize_extension_move(owner,source,mapping.planes,floating.delta,data.base.dpi,budget.bytes,budget.operations);result.content_history=true;continue;}
        if(selection_detail::vector_move_owner(source)){
            selection_detail::materialize_vector_move(owner,source,mapping.planes,floating.delta,data.base.dpi,budget.bytes,budget.operations);
            result.content_history=true;continue;
        }
        for(const auto& plane:source.planes)extent=unite(extent,block_extent(*plane.raster));
        if(extent.empty())continue;
        //620200 unions raw cached boxes before61F9A0 converts the union with
        //the LAST visited raster child's DPI. Placement below remains in raw
        //integer owner coordinates, independently of this extent conversion.
        const auto extent_scale=source.common.resolution/source.planes.back().common.resolution;
        extent={scaled_coordinate(extent.left*extent_scale),scaled_coordinate(extent.top*extent_scale),
            scaled_coordinate(extent.right*extent_scale),scaled_coordinate(extent.bottom*extent_scale)};
        const auto source_x=std::int64_t(source.common.x)+floating.delta[0],source_y=std::int64_t(source.common.y)+floating.delta[1];
        const Area incoming{extent.left+source_x-owner.common.x,extent.top+source_y-owner.common.y,
            extent.right+source_x-owner.common.x,extent.bottom+source_y-owner.common.y};
        const auto bounds=unite(incoming,{0,0,owner.common.width,owner.common.height});
        if(!floating.target&&(incoming.left<0||incoming.top<0||incoming.right>=owner.common.width||incoming.bottom>=owner.common.height)){
            if(budget.transform){std::uint64_t used_bytes=512ull*1024*1024-budget.bytes,used_work=128ull*1024*1024-budget.operations;
                layer_move_detail::expand_raster_layer(owner,{coordinate(bounds.left),coordinate(bounds.top),coordinate(bounds.right-bounds.left),coordinate(bounds.bottom-bounds.top)},used_bytes,used_work,budget.stop);
                budget.bytes=512ull*1024*1024-used_bytes;budget.operations=128ull*1024*1024-used_work;
            }else expand_raster_layer(owner,{coordinate(bounds.left),coordinate(bounds.top),coordinate(bounds.right-bounds.left),coordinate(bounds.bottom-bounds.top)});
            result.content_history=true;
        }
        const auto base_dx=source_x-owner.common.x,base_dy=source_y-owner.common.y;
        for(std::size_t p=0;p<source.planes.size();++p){
            const auto& plane=source.planes[p];const auto& raster=*plane.raster;auto& target=owner.planes[mapping.planes[p]];
            const auto roi=block_extent(raster);if(roi.empty())continue;
            const auto factor=floating.target&&(target.family&0x00f00000u)==0x00200000u?std::trunc(target.common.resolution/owner.common.resolution):1;const auto dx=scaled_coordinate(base_dx*factor),dy=scaled_coordinate(base_dy*factor);
            result.content_history=true;target.pixels_changed=true;
            const bool alpha_copy=target.common.opacity_mode==4,white_skip=!alpha_copy&&plane.common.opacity_mode==5&&target.raster->depth>=32;
            if(target.raster->depth<32&&!alpha_copy){
                materialize_low_plane(raster,*target.raster,dx,dy,budget);continue;}
            // Traverse only backed source blocks, including transparent blocks
            // for geometry/history. Missing samples have zero source alpha.
            for(const auto& [tile,bytes]:raster.tiles){
                const auto region=intersect({std::int64_t(tile.x)*256,std::int64_t(tile.y)*256,std::int64_t(tile.x+1)*256,std::int64_t(tile.y+1)*256},{0,0,raster.width,raster.height});
                budget.work(std::uint64_t(region.right-region.left)*(region.bottom-region.top));
                for(auto y=region.top;y<region.bottom;++y){budget.poll();
                    //Unaligned CommonCopy3 advances with NextCursorX, including
                    //the terminal cursor. A final boundary may allocate the
                    //next in-range block without sampling another source pixel.
                    const auto next_x=region.right+dx,next_y=y+dy;
                    if(std::max<std::int64_t>(0,region.left+dx)<std::min<std::int64_t>(target.raster->width,next_x)
                        &&dx%256&&next_x%256==0&&next_x>=0&&next_x<target.raster->width&&next_y>=0&&next_y<target.raster->height){
                        const auto key=TileCoord{int(next_x)/256,int(next_y)/256};const auto found=target.raster->tiles.find(key);
                        if(found==target.raster->tiles.end()||found->second.use_count()!=1){budget.allocate(target.raster->tile_bytes());selection_detail::store_raw_pixel(*target.raster,int(next_x),int(next_y),selection_detail::raw_pixel(*target.raster,int(next_x),int(next_y)));}
                    }
                    for(auto x=region.left;x<region.right;++x){
                    const auto tx=coordinate(x+dx),ty=coordinate(y+dy);if(!target.raster->contains(tx,ty))continue;
                    const auto key=TileCoord{tx/256,ty/256};auto found=target.raster->tiles.find(key);
                    if(found==target.raster->tiles.end()||found->second.use_count()!=1){budget.allocate(target.raster->tile_bytes());
                        selection_detail::store_raw_pixel(*target.raster,tx,ty,selection_detail::raw_pixel(*target.raster,tx,ty));}
                    //Native write-locked MoveCursor precedes the source alpha
                    //and white-key gates, so a skipped setter can own a block.
                    auto sample=raster_stroke_surface_pixel(raster,int(x),int(y));
                    if(raster.depth==2&&selection_detail::raw_pixel(raster,int(x),int(y))!=3)continue;
                    if(!(sample.a>>8))continue;
                    //CommonCopy3 low-source Get8 path normalizes using its
                    //zero tint packet. The visit remains, but all channels,
                    //including AlphaCopy's input alpha, normalize to zero.
                    if(raster.depth<=8)sample={0,0,0,0};
                    if(white_skip&&(sample.r>>8)==255&&(sample.g>>8)==255&&(sample.b>>8)==255)continue;
                    if(target.raster->depth<32){
                        // CommonCopy3/AlphaCopy dispatches by destination depth:
                        // Do8→Set8 for1/2/8; base Do24→Set24 for16/24.
                        const auto a=sample.a>>8;
                        const auto value=target.raster->depth==1?std::uint64_t(a>127):target.raster->depth==2?std::uint64_t(a>=127?3:0):
                            target.raster->depth==8?std::uint64_t(a):target.raster->depth==16?
                            (std::uint64_t(((sample.r>>8)+4*(sample.g>>8)+2*(sample.b>>8))/7)<<8)|255:
                            std::uint64_t(sample.b>>8)|(std::uint64_t(sample.g>>8)<<8)|(std::uint64_t(sample.r>>8)<<16);
                        selection_detail::store_raw_pixel(*target.raster,tx,ty,value);
                    }else{
                        const auto output=writeback_pixel(target.raster->pixel(tx,ty),sample,target.raster->depth,alpha_copy);
                        target.raster->set_pixel(tx,ty,output);
                    }
                }}
            }
        }
    }
    return result;
}
}
FloatingMaterialization prepare_floating_materialization(const DocumentData& data,const FloatingSelection& floating){Budget budget;return materialize(data,floating,budget);}
FloatingMaterialization prepare_transform_floating_materialization(const DocumentData& data,const FloatingSelection& floating,std::uint64_t& work_remaining,std::uint64_t& bytes_remaining,std::stop_token stop){Budget budget{work_remaining,bytes_remaining,stop,true};auto result=materialize(data,floating,budget);budget.poll();work_remaining=budget.operations;bytes_remaining=budget.bytes;return result;}

}
