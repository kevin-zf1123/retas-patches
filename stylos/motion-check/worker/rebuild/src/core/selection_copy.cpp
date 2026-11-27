#include "selection_copy.hpp"
#include "retas/raster_stroke_pixels.hpp"
#include "modal_document_guard.hpp"
#include "vector_indication.hpp"
#include "selection_vector.hpp"
#include "selection_extension.hpp"
#include "retas/text_tool.hpp"
#include "text_font_backend_internal.hpp"
#include "retas/layer_plane.hpp"
#include "retas/vanishing_point_move.hpp"
#include "retas/vector_ruler_render.hpp"
#include "retas/vector_shape_render.hpp"
#include <algorithm>
#include <cmath>

namespace retas::selection_copy_detail {
namespace {
struct Budget {
    std::uint64_t& bytes;std::uint64_t& work;std::stop_token stop;
    Budget(std::uint64_t& b,std::uint64_t& w,std::stop_token s):bytes(b),work(w),stop(s){if(b>modal_detail::max_bytes||w>modal_detail::max_work)throw std::length_error("COPY ledger already exhausted");}
    void allocate(std::uint64_t n){modal_detail::poll(stop);modal_detail::charge(bytes,n,modal_detail::max_bytes,"COPY storage exceeds512MiB");}
    void visit(std::uint64_t n){modal_detail::poll(stop);modal_detail::charge(work,n,modal_detail::max_work,"COPY work exceeds128Mi");}
};
bool text_owner(const Layer& owner){return owner.family==0x40000800u||owner.family==0x48040800u;}
bool geometry_owner(const Layer& owner){return selection_detail::vector_move_owner(owner)||selection_detail::extension_move_owner(owner);}
bool query_family(std::uint32_t f) noexcept{
    switch(f){case 0x11000800:case 0x11001000:case 0x21200000:case 0x18004000:case 0x40020000:case 0x10008000:case 0x10000100:return true;default:return false;}
}
void owner_header(const Layer& owner){
    if(owner.family==0x40020000u){if(owner.opaque)throw std::invalid_argument("COPY vanishing owner is opaque");for(const auto& p:owner.planes){if(p.family!=0x40000080u||p.opaque||p.raster)throw std::invalid_argument("COPY vanishing child is invalid");(void)decode_vanishing_point_parameters(p.specific);}return;}
    if(geometry_owner(owner)){if(owner.opaque)throw std::invalid_argument("COPY vector owner is opaque");return;}
    if(text_owner(owner)){if(owner.opaque||owner.planes.empty())throw std::invalid_argument("COPY Text owner is invalid");for(const auto& p:owner.planes){if(p.family!=0x40000004u||p.opaque||p.raster)throw std::invalid_argument("COPY Text child is invalid");(void)decode_text_parameters(p.specific);}return;}
    if((owner.family!=0x10000100&&owner.family!=0x10008000&&owner.family!=0x18004000&&owner.family!=0x11000800&&owner.family!=0x11001000)||owner.opaque||!owner.specific.empty()||!owner.suffix.empty()||!owner.plane_suffix.empty()||!owner.opaque_record.empty()||!owner.common.suffix.empty())
        throw std::invalid_argument("COPY requires a canonical ordinary raster owner");
    if(owner.common.width<=0||owner.common.height<=0||owner.common.width>100000||owner.common.height>100000||!std::isfinite(owner.common.resolution)||owner.common.resolution<=0)
        throw std::invalid_argument("COPY owner geometry is invalid");
}
void child_header(const Layer& owner,const Plane& plane,double dpi,bool factory){
    const bool family=plane.family==0x10000001||((owner.family==0x11000800||owner.family==0x11001000)
        &&(plane.family==0x12100001||plane.family==(owner.family==0x11000800?0x11110002u:0x11110001u)));
    if(!family||plane.opaque||!plane.raster||!raster_stroke_surface_supported(*plane.raster)||!plane.suffix.empty()||!plane.opaque_record.empty()||!plane.common.suffix.empty())throw std::invalid_argument("COPY requires a canonical ordinary raster child");
    const auto& r=*plane.raster;
    if(owner.common.resolution!=dpi||!std::isfinite(plane.common.resolution)||plane.common.resolution<=0||r.resolution!=plane.common.resolution
       ||r.width<=0||r.height<=0||r.width>100000||r.height>100000||std::uint64_t(r.width)*r.height>268435456
       ||plane.common.width!=r.width||plane.common.height!=r.height||plane.common.depth!=r.depth
       ||(factory&&(r.width!=owner.common.width||r.height!=owner.common.height)))throw std::invalid_argument("COPY child factory geometry does not match its offscreen");
    for(const auto& [key,tile]:r.tiles)if(key.x<0||key.y<0||std::int64_t(key.x)*256>=r.width||std::int64_t(key.y)*256>=r.height||!tile||tile->size()!=r.tile_bytes())throw std::invalid_argument("COPY raster tile is invalid");
    // The ordinary specific section is the archived offscreen. Unknown
    // following family records are outside this factory-clone certificate.
    if(!plane.specific.empty()){Reader reader(plane.specific);(void)reader.section();if(reader.remaining())throw std::invalid_argument("COPY does not admit private raster suffix records");}
}
CommonElement clone_common(const CommonElement& source,bool full,bool owner){
    auto c=source;c.runtime_identity=next_element_identity();c.field_14=-1;c.suffix.clear();
    if(!full)c.position={Length{0,-1,0},Length{0,-1,0}};
    if(owner&&!full)c.opacity_mode=1;return c;
}
void normalize(std::vector<Layer>& layers,Budget& budget){
    for(std::size_t i=0;i<layers.size();++i){budget.visit(1);auto& owner=layers[i];owner.common.flags[2]=i==0;owner.current_plane=0;
        for(std::size_t p=0;p<owner.planes.size();++p){budget.visit(1);owner.planes[p].common.flags[2]=p==0;}}
}
int coordinate(std::int64_t value){if(value<INT32_MIN||value>INT32_MAX)throw std::invalid_argument("COPY coordinates exceed signed32");return int(value);}
const Plane* companion(const Layer& owner,const Plane& primary,const RegionSelection& selection,const SelectionCopyOptions& options,Budget& budget){
    if(owner.family!=0x11000800||primary.family!=0x12100001||!options.copy_paint_and_line)return nullptr;
    budget.visit(owner.planes.size());const auto found=std::ranges::find(owner.planes,0x11110002u,&Plane::family);
    if(found==owner.planes.end()||found->common.flags[2]||!found->common.flags[1])return nullptr;
    budget.visit(found->raster?found->raster->tiles.size():0);child_header(owner,*found,owner.common.resolution,true);
    for(auto value:{std::int64_t(selection.bounds.x)+selection.origin[0]-owner.common.x,std::int64_t(selection.bounds.y)+selection.origin[1]-owner.common.y,
        std::int64_t(selection.bounds.x)+selection.origin[0]-owner.common.x+std::max(0,selection.bounds.width),std::int64_t(selection.bounds.y)+selection.origin[1]-owner.common.y+std::max(0,selection.bounds.height)})(void)coordinate(value);
    return &*found;
}
bool intersects(Rect a,const Layer& l){return std::int64_t(a.x)<std::int64_t(l.common.x)+std::max(0,l.common.width)&&std::int64_t(l.common.x)<std::int64_t(a.x)+a.width&&std::int64_t(a.y)<std::int64_t(l.common.y)+std::max(0,l.common.height)&&std::int64_t(l.common.y)<std::int64_t(a.y)+a.height&&a.width>0&&a.height>0;}
}
bool query(const DocumentData& data,const RegionSelection& selection,std::uint32_t flags,bool cutting) noexcept{
    for(const auto& owner:data.layers){
        if(!owner.common.flags[1]||!owner.common.flags[2]||(cutting&&!owner.common.runtime_operable))continue;
        if(owner.family==0x40000800||owner.family==0x48040800){if(!owner.planes.empty()&&owner.planes[0].common.flags[1])return true;continue;}
        if(!query_family(owner.family))continue;
        //66E480 rechecks owner+2C in its one-owner branch, not child+2C.
        for(const auto& plane:owner.planes)if(plane.common.flags[1]&&(((flags&1u)||(plane.family&0x10000000u))?selection.active():owner.common.flags[3]!=0))return true;
    }return false;
}
std::shared_ptr<const SelectionCopyPacket> selected(DocumentData& data,const RegionSelection& selection,const SelectionCopyOptions& options,bool& active,std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop){
    Budget budget{bytes,work,stop};budget.visit(data.layers.size());
    const auto carrier=selection_carrier_size(selection,data.base.width(),data.base.height());
    const Rect frame{coordinate(std::int64_t(selection.bounds.x)+selection.origin[0]),coordinate(std::int64_t(selection.bounds.y)+selection.origin[1]),std::max(0,selection.bounds.width),std::max(0,selection.bounds.height)};
    (void)coordinate(std::int64_t(frame.x)+frame.width);(void)coordinate(std::int64_t(frame.y)+frame.height);
    const bool one=std::count_if(data.layers.begin(),data.layers.end(),[](const Layer& l){return l.common.flags[2]!=0;})==1;
    budget.allocate(data.layers.size()*sizeof(FloatingSource));std::vector<FloatingSource> targets;targets.reserve(data.layers.size());
    for(std::size_t i=0;i<data.layers.size();++i){budget.visit(1);const auto& owner=data.layers[i];
        if(!owner.common.flags[2]||!owner.common.flags[1]||!owner.common.runtime_operable||owner.family==0x40000200||owner.family==0x40001000
            ||(options.current_plane_only&&int(i)!=data.current_layer))continue;
        if(selection.active()&&!intersects(frame,owner)&&((selection.vector_flags&1u)||!(owner.family&0x20000000u)))continue;
        if(owner.family==0x40020000u)continue; //native factory shell has no successful worker/frame
        owner_header(owner);if(!selection.active()&&!geometry_owner(owner)&&!text_owner(owner))continue;
        FloatingSource target{i,{}};budget.allocate(owner.planes.size()*sizeof(std::size_t));target.planes.reserve(owner.planes.size());
        if(text_owner(owner)){if(options.current_plane_only)continue;for(std::size_t p=0;p<owner.planes.size();++p)target.planes.push_back(p);targets.push_back(std::move(target));continue;}
        for(std::size_t p=0;p<owner.planes.size();++p){budget.visit(1);const auto& plane=owner.planes[p];if(!plane.common.flags[1]||(one&&!plane.common.flags[2])||(options.current_plane_only&&int(p)!=owner.current_plane))continue;
            if(geometry_owner(owner)){
                const bool paint=owner.family==0x21200000u;
                if(owner.family==0x20800000u)continue;
                if(paint&&((selection.vector_flags&1u)?!selection.active():!owner.common.flags[3]))continue;
                if(paint&&((plane.family==0x21110001u||plane.family==0x21120001u)?!(selection.vector_flags&0x100u):!(selection.vector_flags&0x200u)))continue;
                if((owner.family==0x22100000u||owner.family==0x24400000u)&&((plane.family!=0x21210001u&&plane.family!=0x21220001u)||((selection.vector_flags&1u)?!selection.active():!plane.common.flags[3])))continue;
                if(selection_detail::extension_move_owner(owner)&&!plane.common.flags[3])continue;
                target.planes.push_back(p);continue;
            }
            budget.visit(plane.raster?plane.raster->tiles.size():0);child_header(owner,plane,data.base.dpi,true);(void)companion(owner,plane,selection,options,budget);target.planes.push_back(p);}
        if(target.planes.empty()&&geometry_owner(owner))continue;
        if(target.planes.empty())throw std::invalid_argument("COPY rejects the native empty selected-child shell");
        targets.push_back(std::move(target));
    }
    budget.allocate(sizeof(SelectionCopyPacket)+sizeof(std::vector<Layer>));auto packet=std::make_shared<SelectionCopyPacket>();
    auto layers=std::make_shared<std::vector<Layer>>();budget.allocate(targets.size()*sizeof(Layer));layers->reserve(targets.size());
    Rect output_bounds{};const auto include=[&](Rect r){if(r.width<=0||r.height<=0)return;if(output_bounds.width<=0||output_bounds.height<=0){output_bounds=r;return;}const auto l=std::min(output_bounds.x,r.x),t=std::min(output_bounds.y,r.y);const auto right=std::max(std::int64_t(output_bounds.x)+output_bounds.width,std::int64_t(r.x)+r.width),bottom=std::max(std::int64_t(output_bounds.y)+output_bounds.height,std::int64_t(r.y)+r.height);output_bounds={l,t,coordinate(right-l),coordinate(bottom-t)};};
    std::vector<FloatingSource> exported_targets;exported_targets.reserve(targets.size());
    for(const auto& target:targets){budget.visit(1);const auto& source=data.layers[target.layer];budget.allocate(source.common.name.size()+target.planes.size()*sizeof(Plane));
        if(geometry_owner(source)||text_owner(source)){
            Layer owner;bool captured=true;
            if(text_owner(source)){budget.allocate(source.specific.size()+source.suffix.size()+source.plane_suffix.size());budget.visit(source.specific.size());owner=source;for(auto& p:owner.planes){budget.allocate(p.specific.size()+p.suffix.size());budget.visit(p.specific.size());p.runtime_text_cache.reset();}include(frame);for(const auto& p:source.planes){if(decode_text_parameters(p.specific).text.empty())continue;if(!options.text_fonts)throw std::invalid_argument("Text COPY requires an explicit font backend");resolution_detail::Budget font_budget{bytes,work,stop};std::uint64_t pixels{};const auto masks=text_font_detail::export_masks(*options.text_fonts,source,p,source.common.resolution,font_budget,pixels);include({masks.glyph.origin[0],masks.glyph.origin[1],masks.glyph.raster.width,masks.glyph.raster.height});if(masks.leader)include({masks.leader->origin[0],masks.leader->origin[1],masks.leader->raster.width,masks.leader->raster.height});bytes=font_budget.bytes;work=font_budget.work;}}
            else{auto remainder=source;const auto initial_bytes=bytes,initial_work=work;std::uint64_t remaining_bytes=modal_detail::max_bytes-bytes,remaining_work=modal_detail::max_work-work;
                if(selection_detail::extension_move_owner(source)){
                    owner=selection_detail::capture_extension_move(remainder,source,target.planes,selection,data.base.dpi,remaining_bytes,remaining_work,captured);
                    VectorBounds raw{};const auto merge=[&](VectorBounds v){if(v.left>=v.right||v.top>=v.bottom)return;if(raw.left>=raw.right||raw.top>=raw.bottom)raw=v;else{raw.left=std::min(raw.left,v.left);raw.top=std::min(raw.top,v.top);raw.right=std::max(raw.right,v.right);raw.bottom=std::max(raw.bottom,v.bottom);}};
                    budget.allocate(owner.specific.size()*16);budget.visit(owner.specific.size()*8);
                    if(owner.family==0x40004000u){for(const auto& p:decode_vector_ruler_geometries(owner))for(const auto& c:p.commands)merge(c.gate);}else for(const auto& p:decode_vector_shape_geometries(owner))for(const auto& c:p.commands)merge(c.gate);
                    if(raw.left<raw.right&&raw.top<raw.bottom)include({coordinate(std::int64_t(source.common.x)+raw.left/16),coordinate(std::int64_t(source.common.y)+raw.top/16),raw.right/16-raw.left/16,raw.bottom/16-raw.top/16});
                }
                else{Rect bounds;VectorBounds damage;owner=selection_detail::capture_vector_move(remainder,source,target.planes,selection,data.base.dpi,remaining_bytes,remaining_work,captured,bounds,damage,true);include(bounds);}
                const auto extra_bytes=bytes-initial_bytes,extra_work=work-initial_work;bytes=modal_detail::max_bytes-remaining_bytes;work=modal_detail::max_work-remaining_work;budget.allocate(extra_bytes);budget.visit(extra_work);
            }
            if(!captured)continue;owner.common=clone_common(source.common,text_owner(source),true);
            for(std::size_t p=0;p<owner.planes.size();++p)owner.planes[p].common=clone_common(source.planes.at(target.planes.at(p)).common,text_owner(source),false);
            owner.runtime_vector_high_water={};layers->push_back(std::move(owner));exported_targets.push_back(target);continue;
        }
        include(frame);Layer owner;owner.family=source.family;owner.common=clone_common(source.common,false,true);owner.planes.reserve(target.planes.size());
        for(auto index:target.planes){budget.visit(1);const auto& source_plane=source.planes[index];budget.allocate(source_plane.common.name.size());
            Plane plane;plane.family=source_plane.family;plane.common=clone_common(source_plane.common,false,false);
            const auto* line=companion(source,source_plane,selection,options,budget);
            plane.raster=raster_payload(source,source_plane,selection,carrier,bytes,work,stop,line);plane.pixels_changed=true;owner.planes.push_back(std::move(plane));}
        layers->push_back(std::move(owner));exported_targets.push_back(target);
    }
    auto sources=std::make_shared<std::vector<SelectionCopyPacket::Source>>();budget.allocate(sizeof(*sources)+targets.size()*sizeof(SelectionCopyPacket::Source));sources->reserve(targets.size());
    for(const auto& target:exported_targets){const auto& owner=data.layers[target.layer];SelectionCopyPacket::Source source;source.owner=owner.common.runtime_identity;budget.allocate(target.planes.size()*sizeof(std::uint64_t));source.planes.reserve(target.planes.size());for(auto p:target.planes)source.planes.push_back(owner.planes[p].common.runtime_identity);sources->push_back(std::move(source));}
    normalize(*layers,budget);if(!layers->empty()){packet->layers=std::move(layers);packet->sources=std::move(sources);packet->tight_bounds=output_bounds;packet->capture_frame=frame.width>0&&frame.height>0?frame:output_bounds;packet->source_is_current_document=true;packet->current_plane_only=options.current_plane_only;}
    //672300(cut=0) has precisely one615390 tail, even with no payload.
    const bool mask=selection.state==SelectionLayerState::Mask||selection.state==SelectionLayerState::VectorIndicated;
    const auto dpi=mask&&!(selection.vector_flags&1u)?selection_carrier_dpi(selection,data.base.dpi):0;
    active=vector_indication_detail::refresh_marks(data,active,selection,selection.vector_flags,dpi,bytes,work,stop);
    modal_detail::poll(stop);return packet;
}
std::shared_ptr<const SelectionCopyPacket> live(const FloatingSelection& source,bool current,std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop){
    Budget budget{bytes,work,stop};budget.visit(1);if(!source.layers||source.layers->empty())throw std::invalid_argument("COPY live holder is empty");
    budget.allocate(sizeof(SelectionCopyPacket)+sizeof(std::vector<Layer>)+source.layers->size()*sizeof(Layer));
    auto packet=std::make_shared<SelectionCopyPacket>();auto layers=std::make_shared<std::vector<Layer>>();layers->reserve(source.layers->size());
    for(const auto& source_owner:*source.layers){budget.visit(1);owner_header(source_owner);if(source_owner.planes.empty())throw std::invalid_argument("COPY live owner has no children");
        if(geometry_owner(source_owner)||text_owner(source_owner)||source_owner.family==0x40020000u){
            budget.allocate(source_owner.specific.size()*3+source_owner.planes.size()*sizeof(Plane));budget.visit(source_owner.specific.size());
            Layer owner=source_owner;owner.runtime_vector_face_colors.reset();owner.common=clone_common(source_owner.common,true,true);owner.runtime_vector_high_water={};
            for(auto& p:owner.planes){budget.allocate(p.specific.size()*3);budget.visit(p.specific.size());p.common=clone_common(p.common,true,false);p.runtime_text_cache.reset();}
            layers->push_back(std::move(owner));continue;
        }
        budget.allocate(source_owner.common.name.size()+source_owner.planes.size()*sizeof(Plane));Layer owner;owner.family=source_owner.family;owner.common=clone_common(source_owner.common,true,true);owner.planes.reserve(source_owner.planes.size());
        for(const auto& source_plane:source_owner.planes){budget.visit(1+(source_plane.raster?source_plane.raster->tiles.size():0));child_header(source_owner,source_plane,source_owner.common.resolution,false);budget.allocate(source_plane.common.name.size());
            Plane plane;plane.family=source_plane.family;plane.common=clone_common(source_plane.common,true,false);const auto& r=*source_plane.raster;budget.allocate(r.tiles.size()*96ull);
            plane.raster.emplace(r.width,r.height,r.depth,r.resolution);plane.raster->color_order=r.color_order;
            for(const auto& [key,tile]:r.tiles){budget.allocate(tile->size());budget.visit(tile->size()+1);plane.raster->tiles.emplace(key,std::make_shared<Bytes>(*tile));}
            plane.pixels_changed=true;owner.planes.push_back(std::move(plane));}
        layers->push_back(std::move(owner));
    }
    normalize(*layers,budget);packet->layers=std::move(layers);packet->tight_bounds=source.tight_bounds;packet->capture_frame=source.capture_frame;packet->source_is_current_document=current;
    modal_detail::poll(stop);return packet;
}
}
