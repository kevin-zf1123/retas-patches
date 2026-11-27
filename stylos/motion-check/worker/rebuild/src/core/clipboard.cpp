#include "retas/clipboard.hpp"
#include "retas/raster_stroke_pixels.hpp"
#include "selection_vector.hpp"
#include "selection_extension.hpp"
#include "retas/vector_face_archive.hpp"
#include "retas/preview.hpp"
#include "retas/text_tool.hpp"
#include "retas/vanishing_point_move.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace retas {
ConvertPasteCandidates convert_paste_candidates(const DocumentData& data,const SelectionCopyPacket& holder,bool current_only){
    ConvertPasteCandidates result;if(current_only||holder.current_plane_only||!holder.layers||holder.layers->empty()||data.current_layer<0||std::size_t(data.current_layer)>=data.layers.size())return result;
    result.layers={0x10000100,0x18004000};const auto family=data.layers[std::size_t(data.current_layer)].family;
    bool base=family==0x11000800||family==0x11001000||family==0x18004000||family==0x21200000;
    bool ras=family==0x12000200||family==0x14002000,add=family==0x22100000||family==0x24400000,vert=add,line=family==0x21200000,face=line;
    for(const auto& owner:*holder.layers)for(const auto& plane:owner.planes){const auto f=plane.family;
        const bool r=f==0x11210001||f==0x11220001||f==0x11240001;ras&=r;add&=r;vert&=f==0x21210001||f==0x21220001;line&=f==0x21110001||f==0x21120001;face&=f==0x22100001;}
    if(base)result.planes.push_back(0x10000001);if(ras){result.planes.push_back(0x11210001);result.planes.push_back(0x11220001);}if(add)result.planes.push_back(0x11240001);
    if(vert){result.planes.push_back(0x21210001);result.planes.push_back(0x21220001);}if(line){result.planes.push_back(0x21110001);result.planes.push_back(0x21120001);}if(face)result.planes.push_back(0x22100001);return result;
}
namespace {
int coordinate(std::int64_t value){
    if(value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())
        throw std::invalid_argument("粘贴坐标超出支持范围。");
    return int(value);
}
void dimensions(int width,int height){
    if(width<=0||height<=0||width>100000||height>100000
        ||std::uint64_t(width)*height>128ull*1024*1024)
        throw std::invalid_argument("粘贴图像尺寸超出支持范围。");
}
std::array<int,2> page(const DocumentData& target){
    if(!std::isfinite(target.base.dpi)||target.base.dpi<=0)
        throw std::invalid_argument("粘贴目标分辨率无效。");
    const auto width=target.base.width(),height=target.base.height();dimensions(width,height);return {width,height};
}
Rect translated(Rect frame,std::int64_t dx,std::int64_t dy,bool allow_empty=false){
    if(frame.width<0||frame.height<0||(!allow_empty&&(!frame.width||!frame.height)))
        throw std::invalid_argument("粘贴来源范围无效。");
    if(frame.width&&frame.height)dimensions(frame.width,frame.height);
    const auto x=std::int64_t(frame.x)+dx,y=std::int64_t(frame.y)+dy;
    coordinate(x+frame.width);coordinate(y+frame.height);
    return {coordinate(x),coordinate(y),frame.width,frame.height};
}
void validate_helper_storage(std::array<int,2> size,Rect capture){
    // Foreign helper storage is page union capture, independent of both the
    // document selection carrier and the later live floating translation.
    const auto left=std::min<std::int64_t>(0,capture.x),top=std::min<std::int64_t>(0,capture.y);
    const auto right=std::max<std::int64_t>(size[0],std::int64_t(capture.x)+capture.width);
    const auto bottom=std::max<std::int64_t>(size[1],std::int64_t(capture.y)+capture.height);
    dimensions(coordinate(right-left),coordinate(bottom-top));
}
void validate_payload(const DocumentData& target,const FloatingSelection& floating,bool normalized=true){
    if((floating.kind!=FloatingKind::General&&floating.kind!=FloatingKind::PaintMono)||floating.target||!floating.layers||floating.layers->empty())
        throw std::invalid_argument("此粘贴载荷类型或显式目标尚未接入。");
    translated(floating.capture_frame,0,0);translated(floating.tight_bounds,0,0,true);
    std::uint64_t tiles_bytes{},vector_bytes=256ull*1024*1024,vector_work=128ull*1024*1024;
    for(const auto& layer:*floating.layers){
        if(layer.family==0x40000800u||layer.family==0x48040800u||layer.family==0x40020000u){
            if(layer.opaque||layer.planes.empty()||!std::isfinite(layer.common.resolution)||layer.common.resolution<=0)throw std::invalid_argument("Clipboard parameter owner is invalid");
            translated({layer.common.x,layer.common.y,layer.common.width,layer.common.height},0,0);
            for(const auto& plane:layer.planes){if(plane.opaque||plane.raster)throw std::invalid_argument("Clipboard parameter child is invalid");if(layer.family==0x40020000u){if(plane.family!=0x40000080u)throw std::invalid_argument("Clipboard vanishing family is invalid");(void)decode_vanishing_point_parameters(plane.specific);}else{if(plane.family!=0x40000004u)throw std::invalid_argument("Clipboard Text family is invalid");(void)decode_text_parameters(plane.specific);}}
            continue;
        }
        if(selection_detail::extension_move_owner(layer)){if(normalized&&layer.common.resolution!=target.base.dpi)throw std::invalid_argument("Foreign extension owner DPI was not normalized");translated({layer.common.x,layer.common.y,layer.common.width,layer.common.height},0,0);selection_detail::validate_extension_move(layer,layer.common.resolution,vector_bytes,vector_work);continue;}
        if(selection_detail::vector_move_owner(layer)){
            if(normalized&&layer.common.resolution!=target.base.dpi)throw std::invalid_argument("Foreign vector owner DPI was not normalized");
            translated({layer.common.x,layer.common.y,layer.common.width,layer.common.height},0,0);
            selection_detail::validate_vector_move(layer,layer.common.resolution,vector_bytes,vector_work);continue;
        }
        if(layer.opaque||(layer.family&0xf0000000u)!=0x10000000u||layer.planes.empty())
            throw std::invalid_argument("粘贴仅支持普通光栅图层的标准模式。");
        if(!std::isfinite(layer.common.resolution)||layer.common.resolution<=0||(normalized&&layer.common.resolution!=target.base.dpi))throw std::invalid_argument("粘贴图层分辨率无效。");
        translated({layer.common.x,layer.common.y,layer.common.width,layer.common.height},0,0);
        for(const auto& plane:layer.planes){
            if(plane.opaque||(plane.family&0xf0000000u)!=0x10000000u
                ||!plane.raster||!raster_stroke_surface_supported(*plane.raster))
                throw std::invalid_argument("粘贴仅支持 canonical 光栅平面的标准模式。");
            const auto& raster=*plane.raster;dimensions(raster.width,raster.height);
            if(!std::isfinite(plane.common.resolution)||plane.common.resolution<=0||raster.resolution!=plane.common.resolution)
                throw std::invalid_argument("粘贴平面分辨率无效。");
            const auto tile_size=raster.tile_bytes();
            for(const auto& [tile,bytes]:raster.tiles){
                if(tile.x<0||tile.y<0||std::int64_t(tile.x)*256>=raster.width||std::int64_t(tile.y)*256>=raster.height
                    ||!bytes||bytes->size()!=tile_size)throw std::invalid_argument("粘贴来源光栅图块无效。");
                if(tile_size>256ull*1024*1024-tiles_bytes)throw std::invalid_argument("粘贴载荷图块过多。");
                tiles_bytes+=tile_size;
            }
        }
    }
}
void translate_layers(std::vector<Layer>& layers,std::int64_t dx,std::int64_t dy){
    for(auto& layer:layers){
        const auto frame=translated({layer.common.x,layer.common.y,layer.common.width,layer.common.height},dx,dy);
        layer.common.x=frame.x;layer.common.y=frame.y;
        // Child-plane origins are metadata, not the owner-space raster offset.
    }
}
}
PlainPasteImport prepare_plain_paste(const DocumentData& target,const FloatingSelection& exported,bool shrink_selection){
    return prepare_plain_paste(target,exported,shrink_selection,{});
}
PlainPasteImport prepare_plain_paste(const DocumentData& target,const SelectionCopyPacket& packet,bool shrink){
    return prepare_plain_paste(target,packet,shrink,{});
}
PlainPasteImport prepare_plain_paste(const DocumentData& target,const SelectionCopyPacket& packet,bool shrink,const RegionSelection& carrier){
    FloatingSelection exported;exported.kind=FloatingKind::General;exported.layers=packet.layers;
    exported.tight_bounds=packet.tight_bounds;exported.capture_frame=packet.capture_frame;
    return prepare_plain_paste(target,exported,shrink,carrier);
}
PlainPasteImport prepare_plain_paste(const DocumentData& target,const FloatingSelection& exported,bool shrink_selection,const RegionSelection& carrier){
    const auto size=page(target);validate_payload(target,exported,false);
    const auto frame=exported.capture_frame;
    const bool inside=frame.x>=0&&frame.y>=0&&std::int64_t(frame.x)+frame.width<=size[0]
        &&std::int64_t(frame.y)+frame.height<=size[1];
    const std::int64_t dx=inside?0:-std::int64_t(frame.x),dy=inside?0:-std::int64_t(frame.y);
    const auto capture=translated(exported.capture_frame,dx,dy);validate_helper_storage(size,capture);
    auto floating=std::make_shared<FloatingSelection>(exported);
    auto layers=std::make_shared<std::vector<Layer>>(*exported.layers);
    for(auto& layer:*layers)layer.runtime_vector_face_colors.reset(); //61CB50 typed owner clone
    for(auto& layer:*layers)for(auto& plane:layer.planes)if(plane.raster)for(auto& [key,tile]:plane.raster->tiles){(void)key;tile=std::make_shared<Bytes>(*tile);}
    translate_layers(*layers,dx,dy);
    //61CE10→61CB50 retags DPI using the FIRST source owner's resolution.
    //Origins, dimensions and physical pixels are cloned without resampling.
    const auto source_dpi=exported.layers->front().common.resolution;
    for(auto& layer:*layers){layer.common.resolution=target.base.dpi;for(auto& plane:layer.planes){
        const auto dpi=plane.common.resolution/source_dpi*target.base.dpi;
        if(!std::isfinite(dpi)||dpi<=0)throw std::invalid_argument("粘贴分辨率比例超出支持范围。");
        plane.common.resolution=dpi;if(plane.raster)plane.raster->resolution=dpi;
    }
        //61CB50 retags native Paint212 Border resolution; raw geometry and
        //material records are cloned without a matrix transformation.
        if(layer.family==0x21200000){auto a=decode_vector_paint_archive(layer);a.border.common.resolution=target.base.dpi*16;store_vector_paint_archive(layer,a);}
    }
    floating->kind=FloatingKind::General;
    if(layers->size()==1&&layers->front().family==0x11000800&&layers->front().planes.size()<=2
        &&std::ranges::none_of(layers->front().planes,[](const Plane& p){return p.family==0x10000001;})
        &&std::ranges::any_of(target.layers,[](const Layer& l){return l.family==0x11000800;}))floating->kind=FloatingKind::PaintMono;
    floating->layers=std::move(layers);floating->sources.reset();floating->same_document_source=false;floating->delta={};
    floating->capture_frame=capture;
    floating->tight_bounds=translated(exported.tight_bounds,dx,dy,true);
    // First-owner retag can put other native16 owners outside the supported
    // consumer profile. Reject the normalized candidate before publication,
    // including public paste calls without an optional preview validator.
    validate_payload(target,*floating);
    // The page/capture union sizes the foreign preview helper, not the
    // document's selection surface. sub_615830 fills the existing carrier
    // with clipping while retaining the complete requested source frame.
    auto selection=rectangle_selection(floating->capture_frame,{},selection_carrier_size(carrier,size[0],size[1]));selection.vector_flags=carrier.vector_flags;selection.carrier_dpi=carrier.carrier_dpi;
    if(shrink_selection){selection=prepare_paste_content_selection(target,*floating,carrier);if(!selection.active())floating.reset();}
    return {std::move(floating),std::move(selection)};
}
PlainPasteImport prepare_targeted_paste(const DocumentData& target,const FloatingSelection& exported,FloatingTarget binding){
    return prepare_targeted_paste(target,exported,binding,false);
}
PlainPasteImport prepare_targeted_paste(const DocumentData& target,const FloatingSelection& exported,FloatingTarget binding,bool shrink_selection,const RegionSelection& carrier){
    auto imported=prepare_plain_paste(target,exported,false,carrier);auto floating=std::make_shared<FloatingSelection>(*imported.floating);floating->target=binding;floating->kind=FloatingKind::General;
    if(binding.layer<target.layers.size()&&target.layers[binding.layer].family==0x11000800&&binding.plane<target.layers[binding.layer].planes.size()
        &&target.layers[binding.layer].planes[binding.plane].family==0x12100001)floating->kind=FloatingKind::TargetedPaintMono;
    validate_targeted_floating(target,*floating);
    if(shrink_selection){imported.selection=prepare_paste_content_selection(target,*floating,carrier);if(!imported.selection.active())floating.reset();}
    imported.floating=std::move(floating);return imported;
}
void validate_foreign_floating(const DocumentData& target,const FloatingSelection& floating){
    const auto size=page(target);
    if(floating.same_document_source||floating.sources)
        throw std::invalid_argument("外部粘贴不能绑定本地来源索引。");
    validate_payload(target,floating);validate_helper_storage(size,floating.capture_frame);
    translated(floating.capture_frame,floating.delta[0],floating.delta[1]);
    translated(floating.tight_bounds,floating.delta[0],floating.delta[1],true);
    for(const auto& layer:*floating.layers)
        translated({layer.common.x,layer.common.y,layer.common.width,layer.common.height},floating.delta[0],floating.delta[1]);
}
std::vector<Layer> prepare_pasted_layers(const DocumentData& target,const FloatingSelection& floating){
    validate_foreign_floating(target,floating);
    auto layers=*floating.layers;translate_layers(layers,floating.delta[0],floating.delta[1]);
    return layers;
}
}
