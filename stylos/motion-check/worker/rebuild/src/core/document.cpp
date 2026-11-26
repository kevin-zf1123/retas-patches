#include "vector_direct_context.hpp"
#include "../io/vector_face_projection.hpp"
#include "retas/document.hpp"
#include "../io/native_vector_canonical.hpp"
#include "retas/adjust_plane.hpp"
#include "adjust_render.hpp"
#include "retas/layer_plane.hpp"
#include "retas/floating.hpp"
#include "selection_source.hpp"
#include "selection_layer_bridge.hpp"
#include "selection_extension.hpp"
#include "retas/clipboard.hpp"
#include "retas/fill.hpp"
#include "retas/filter_tools.hpp"
#include "filter_dust.hpp"
#include "retas/selection_color.hpp"
#include "selection_color_command.hpp"
#include "retas/vector_indication.hpp"
#include "retas/numeric.hpp"
#include "retas/legacy_text.hpp"
#include "retas/text_tool.hpp"
#include "retas/vector_color_metadata.hpp"
#include "retas/vector_render.hpp"
#include "retas/vector_face_archive.hpp"
#include "retas/vector_extension_archive.hpp"
#include "retas/vector_ruler_render.hpp"
#include "retas/vector_shape_render.hpp"
#include "vector_stored_line.hpp"
#include "vector_line_output.hpp"
#include "text_font_backend_internal.hpp"
#include "vector_stored_render_costs.hpp"
#include "paint_owner_render.hpp"
#include "selection_vector_render.hpp"
#include "../io/vector_line_projection.hpp"
#include "native_storage.hpp"
#include "modal_document_guard.hpp"
#include "raster_history.hpp"
#include "vector_indication.hpp"
#include "vector_paint_history.hpp"
#include "floating_capture.hpp"
#include "selection_copy.hpp"
#include "page_geometry.hpp"
#include <algorithm>
#include <bit>
#include <cassert>
#include <cmath>
#include <limits>
#include <system_error>
#include <type_traits>
#include <tuple>

namespace retas {
namespace {
void inherit_selection_carrier(RegionSelection& selected,const RegionSelection& live){
    if(selected.carrier_dpi==0)selected.carrier_dpi=live.carrier_dpi;
    if(selected.carrier_size==std::array<int,2>{}&&!selected.mask){
        selected.carrier_size=live.carrier_size;
        if(selected.carrier_size==std::array<int,2>{}&&live.mask)
            selected.carrier_size={live.mask->width(),live.mask->height()};
    }
}
RegionSelection selection_history_snapshot(const DocumentData& data,const RegionSelection& selection){
    //44A420 stores only the state for an empty selection. Active snapshots
    //own the offscreen dimensions/DPI, bitmap, bounds and owner origin.
    if(!selection.active())return cleared_selection();
    auto saved=selection;
    saved.carrier_size=selection_carrier_size(selection,data.base.width(),data.base.height());
    saved.carrier_dpi=selection_carrier_dpi(selection,data.base.dpi);
    saved.vector_cache.reset();saved.vector_cache_origin={};saved.vector_flags=0;
    return saved;
}
RegionSelection restore_selection_history(const DocumentData& data,const RegionSelection& saved,
    const RegionSelection& live,std::uint32_t flags){
    auto restored=saved.active()?saved:cleared_selection({},live.carrier_size);
    if(saved.active()){
        const auto size=selection_carrier_size(saved,data.base.width(),data.base.height());
        //6159B0 recreates an offscreen only when its dimensions differ.
        //CopyAll transfers backing without assigning the live offscreen DPI.
        restored.carrier_dpi=size==selection_carrier_size(live,data.base.width(),data.base.height())
            ?live.carrier_dpi:selection_carrier_dpi(saved,data.base.dpi);
        restored.carrier_size=size;
        if(restored.state==SelectionLayerState::VectorIndicated)restored.state=SelectionLayerState::Mask;
    }else restored.carrier_dpi=live.carrier_dpi;
    restored.vector_cache.reset();restored.vector_cache_origin={};restored.vector_flags=flags;
    return restored;
}
struct SelectionIndicationWitness {
    std::uint64_t generation,selection_generation,floating_generation;
    RegionSelection selection;
    std::optional<std::uint32_t> application_flags;
    bool active;
    std::shared_ptr<const FloatingSelection> floating;
    explicit SelectionIndicationWitness(const Document& d):generation(d.state_generation()),selection_generation(d.region_selection_generation()),
        floating_generation(d.floating_selection_generation()),selection(d.region_selection),application_flags(d.application_vector_selection_flags),
        active(d.vector_indication_active),floating(d.floating_selection()){}
    std::uint32_t flags()const{return application_flags.value_or(selection.vector_flags);}
    bool matches(const Document& d)const{
        return generation==d.state_generation()&&selection_generation==d.region_selection_generation()
            &&floating_generation==d.floating_selection_generation()&&selection==d.region_selection
            &&application_flags==d.application_vector_selection_flags&&active==d.vector_indication_active&&floating==d.floating_selection();
    }
};
std::map<std::uint64_t,std::size_t> raster_scope_planes(const Layer& owner,std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop){
    std::map<std::uint64_t,std::size_t> result;
    modal_detail::charge(bytes,owner.planes.size()*64ull,modal_detail::max_bytes,"Raster scope identity metadata exceeds limit");
    for(std::size_t i=0;i<owner.planes.size();++i){modal_detail::poll(stop);
        modal_detail::charge(work,4+4*std::bit_width(result.size()+1),modal_detail::max_work,"Raster scope identity work exceeds limit");
        if(!result.emplace(owner.planes[i].common.runtime_identity,i).second)throw std::invalid_argument("光栅浮动plane标识不唯一。");
    }return result;
}
bool needs_selection_indications(bool active,const RegionSelection& selection,std::uint32_t flags){
    return active||(selection.active()&&!(flags&1u)&&(flags&0x700u));
}
std::optional<DocumentData> indication_source(const DocumentData& data,bool needed,bool include_shared_backing=true){
    if(!needed)return {};
    std::uint64_t metadata{};modal_detail::data_charge(data,metadata,0,include_shared_backing);
    if(metadata>512ull*1024*1024/3)throw std::length_error("Selection indication source exceeds storage limit");
    return data;
}
std::optional<VectorIndicationUpdate> prepare_selection_indications(const std::optional<DocumentData>& source,
    bool active,const RegionSelection& selected,std::uint32_t flags){
    if(!source)return {};
    const bool mask=selected.state==SelectionLayerState::Mask||selected.state==SelectionLayerState::VectorIndicated;
    const auto dpi=mask&&!(flags&1u)?selection_carrier_dpi(selected,source->base.dpi):0;
    return prepare_vector_indication_refresh(*source,active,selected,flags,dpi);
}
template<class Operation>auto with_paint_budget(raster_history_detail::Budget& budget,Operation operation){
    vector_paint_history_detail::Budget paint{budget.bytes,budget.work,budget.stop};
    try{auto result=operation(paint);budget.bytes=paint.bytes;budget.work=paint.work;return result;}
    catch(...){budget.bytes=paint.bytes;budget.work=paint.work;throw;}
}
using PaintManagerBinding=vector_paint_history_detail::DocumentBinding;
std::optional<std::size_t> paint_manager_owner(const PaintManagerBinding& binding,const DocumentData& data){
    raster_history_detail::Edit identity;identity.layout=binding.layout;
    if(identity.layout_matches(data))return binding.hint;
    std::optional<std::size_t> found;for(std::size_t i=0;i<data.layers.size();++i)if(data.layers[i].common.runtime_identity==binding.owner&&data.layers[i].family==binding.family){if(found)return {};found=i;}return found;
}
std::shared_ptr<const PaintManagerBinding> relocate_paint_manager(std::shared_ptr<const PaintManagerBinding> previous,const DocumentData& data,std::size_t hint,raster_history_detail::Budget& budget){
    budget.allocate(sizeof(PaintManagerBinding));auto result=std::make_shared<PaintManagerBinding>(*previous);result->hint=hint;result->layout=raster_history_detail::Edit::identity_layout(data,budget);return result;
}
bool ordinary_pure_history_owner(const Layer& owner){
    return (owner.family==0x22100000u||owner.family==0x24400000u)&&std::ranges::any_of(owner.planes,[](const Plane& p){return p.family==0x21110001u||p.family==0x21120001u;});
}
std::shared_ptr<const PaintManagerBinding> ensure_paint_manager(std::shared_ptr<const PaintManagerBinding> previous,const DocumentData& data,std::size_t index,bool force,raster_history_detail::Budget& budget,std::shared_ptr<const std::vector<std::uint64_t>> layout={},bool draw_mode=false,bool select_paint=false){
    const auto& owner=data.layers.at(index);const auto prior=previous?(layout&&previous->layout==layout?std::optional<std::size_t>{previous->hint}:paint_manager_owner(*previous,data)):std::nullopt;
    force=force||(previous&&(!prior||*prior!=index));
    auto manager=with_paint_budget(budget,[&](auto& paint){return vector_paint_history_detail::Manager::ensure(previous?previous->manager:nullptr,owner,paint,force,draw_mode,select_paint);});
    if(previous&&previous->manager==manager&&prior&&*prior==index&&previous->hint==index){raster_history_detail::Edit identity;identity.layout=previous->layout;if(identity.layout_matches(data))return previous;}
    budget.allocate(sizeof(PaintManagerBinding));auto result=std::make_shared<PaintManagerBinding>();result->manager=std::move(manager);result->owner=owner.common.runtime_identity;result->family=owner.family;result->hint=index;result->layout=layout?std::move(layout):raster_history_detail::Edit::identity_layout(data,budget);return result;
}
std::shared_ptr<const PaintManagerBinding> replace_paint_manager(std::shared_ptr<const PaintManagerBinding> previous,std::shared_ptr<const vector_paint_history_detail::Manager> manager,raster_history_detail::Budget& budget){
    budget.allocate(sizeof(PaintManagerBinding));auto result=std::make_shared<PaintManagerBinding>(*previous);result->manager=std::move(manager);return result;
}
void invalidate_select_paint_manager(std::shared_ptr<const PaintManagerBinding>& manager,const Layer& owner){
    // Ordinary208 producers use whole-owner history, so their writes cannot
    // reuse an Airbrush-specific typed baseline from before the operation.
    if(owner.family==0x20800000u&&manager&&manager->family==owner.family&&manager->owner==owner.common.runtime_identity)manager.reset();
}
std::shared_ptr<const PaintManagerBinding> prepare_paint_manager(std::shared_ptr<const PaintManagerBinding> previous,const DocumentData& data,bool force,raster_history_detail::Budget& budget,bool select_paint=false){
    if(!previous)return {};if(data.current_layer<0||std::size_t(data.current_layer)>=data.layers.size())return {};
    const auto& owner=data.layers[std::size_t(data.current_layer)];const bool draw=previous->manager->draw_baseline()&&(owner.family==0x21200000u||owner.family==0x20800000u||owner.family==0x40002000u||owner.family==0x22100000u||owner.family==0x24400000u||owner.family==0x20000200u);return ensure_paint_manager(previous,data,std::size_t(data.current_layer),force,budget,{},draw,select_paint);
}
std::shared_ptr<const Layer> capture_source_storage(const Layer& owner,raster_history_detail::Budget& budget){
    std::uint64_t bytes=sizeof(Layer)+vector_runtime_detail::face_colors_storage(owner)+owner.specific.size()+owner.suffix.size()+owner.plane_suffix.size()+owner.opaque_record.size()+owner.common.name.size()+owner.common.suffix.size();
    for(const auto& p:owner.planes)bytes+=sizeof(Plane)+p.specific.size()+p.suffix.size()+p.opaque_record.size()+p.common.name.size()+p.common.suffix.size()+adjust_render_detail::cache_storage(p)+(p.raster?p.raster->tiles.size()*128:0);
    budget.allocate(bytes);auto result=std::make_shared<Layer>(owner);
    for(auto& p:result->planes)if(p.raster)p.raster=raster_history_detail::copy_all(*p.raster,budget);return result;
}
bool propagates_plane_line_color(std::uint32_t family) noexcept{
    return ((family&0x200000u)&&(family&0x20000000u))||family==0x40000001u;
}
int pixels(const Length& l,double dpi) {
    using numeric_detail::binary64;
    if(!std::isfinite(dpi)||dpi<=0||!std::isfinite(l.value))throw std::invalid_argument("Invalid document dimensions");
    double v{};
    switch(l.unit){
        case 0:
            if(!std::isfinite(l.reference)||l.reference<=0)throw std::invalid_argument("Invalid document reference resolution");
            v=binary64(binary64(l.value/l.reference)*dpi);break;
        case 1:v=binary64(binary64(dpi*l.value)*0.3937007874015748);break;
        case 2:v=binary64(binary64(dpi*l.value)*0.03937007874015748);break;
        case 3:v=binary64(dpi*l.value);break;
        case 5:v=binary64(binary64(dpi*l.value)*0.01388888888888889);break;
        case 6:v=binary64(binary64(dpi*l.value)*0.00984251968503937);break;
        default:throw std::invalid_argument("Unsupported RCLength unit");
    }
    // RCLength::GetAsPixel uses rtwUtl rounding, including ties toward zero.
    // Reject negative lengths before the legacy negative-near-integer quirk
    // can turn them into a positive pixel count.
    if(!std::isfinite(v)||v<=0)throw std::invalid_argument("Invalid document dimensions");
    const auto result=legacy_rt_double_to_int(v);
    if(!result||*result<1||*result>100000)throw std::invalid_argument("Invalid document dimensions");
    return *result;
}
int line_render_scale(const Plane& plane,double document_dpi){
    if(!plane.raster||plane.raster->depth!=1||!std::isfinite(document_dpi)||document_dpi<=0
        ||!std::isfinite(plane.common.resolution))return 0;
    for(const int scale:{1,2,4,8})
        if(plane.common.resolution==numeric_detail::binary64(document_dpi*scale))return scale;
    return 0;
}
int line_render_width(const Raster& raster,int scale){
    // sub_64B310 advances x by two while x < right, including an odd final
    // pair. The 4x/8x kernels instead truncate both output dimensions.
    return raster.width/scale+(scale==2&&raster.width%2!=0);
}
bool line_missing_white(const Layer& layer,int scale){
    return scale==1&&layer.common.opacity_mode==2;
}
Pixel line_render_pixel(const Layer& layer,const Plane& plane,int x,int y,int scale,const adjust_render_detail::Tables* tables=nullptr){
    const auto& raster=*plane.raster;
    if(x<0||y<0||x>=line_render_width(raster,scale)||y>=raster.height/scale)return {};
    const int source_x=x*scale,source_y=y*scale;
    const auto tile=raster.tiles.find({source_x/256,source_y/256});
    // sub_63C4A0 sends absent blocks to RectFill64(white, alpha-composite)
    // in owner mode 2. A present block's zero bits still contribute nothing.
    if(tile==raster.tiles.end())return line_missing_white(layer,scale)?adjust_render_detail::general_rgb(Pixel::rgba8(255,255,255),tables):Pixel{};
    if(!tile->second)throw std::invalid_argument("Invalid raster tile");
    const auto& bytes=*tile->second;
    const auto offset=std::size_t(source_y%256)*32+(source_x%256)/8;
    const auto shift=8-scale-source_x%8;
    const auto mask=(1u<<scale)-1;
    unsigned count{};
    // Each 1/2/4/8-bit group fits in one byte and one 256-square tile. The 2x
    // final pair deliberately includes a preserved bit past an odd width.
    for(int row=0;row<scale;++row)count+=std::popcount((unsigned(bytes.at(offset+row*32))>>shift)&mask);
    // The 1x wrapper sub_63C4A0 forces full owner opacity in mode 2.
    const int opacity=scale==1&&layer.common.opacity_mode==2?100:std::clamp(layer.common.opacity,0,100);
    auto material_alpha=unsigned(*legacy_rt_mul_div(255,opacity,100));
    const auto color=plane.common.flags[4]?plane.common.color:0;
    // sub_63D1E0 / sub_63D5D0 OR the sideband alpha into owner alpha. The
    // 1x/8x wrappers replace it. These are distinct material contracts;
    // plane opacity and plane mode 5 do not alter one-bit source samples.
    if(scale==2||scale==4)material_alpha|=unsigned(color>>56);
    const auto alpha=((count*255/unsigned(scale*scale))*(material_alpha+1))>>8;
    if(!alpha)return {};
    // Native Color64 is AAAABBBBGGGGRRRR; RTCOLORREF64to32 takes high bytes.
    // DocumentData's composite API returns straight RGBA. Do not expose the
    // native renderer's complement-on-white cache bytes as straight colors.
    return Pixel::rgba8((color>>8)&255,(color>>24)&255,(color>>40)&255,alpha);
}
struct VectorOwnerRender {
    VectorRenderContext context;
    std::vector<vector_render_detail::StoredLinePlane> stored_planes;
};
bool paint_owner_family(std::uint32_t family){return family==0x21200000||family==0x20800000;}
bool zero_position(const CommonElement& common){
    for(const auto& p:common.position){
        // Native constructors leave an unassigned RCLength as (0,-1,0).
        if(p.value==0&&p.unit==-1&&p.reference==0)continue;
        if(p.value!=0||!std::isfinite(p.reference)||p.reference<=0)return false;
        switch(p.unit){case 0:case 1:case 2:case 3:case 5:case 6:break;default:return false;}
    }
    return true;
}
struct PaintOwnerRender {
    VectorRenderContext context;
    std::vector<VectorPaintPlaneGeometry> geometry;
    std::vector<std::vector<const VectorFaceGeometry*>> faces;
    std::vector<VectorFacePlaneView> views;
    std::vector<vector_render_detail::StoredPaintPlane> planes;
    std::uint64_t preparation_work{};
};
PaintOwnerRender prepare_paint_owner(const Layer& layer,double dpi,int depth,const VectorRenderContext* request,
                                    vector_render_detail::StoredLineCacheCosts* costs,std::stop_token stop={},int reference=0){
    modal_detail::poll(stop);
    if(costs&&(costs->storage>modal_detail::max_bytes||costs->work>modal_detail::max_work))
        throw std::length_error("Paint preceding resources exceed limits");
    if(!paint_owner_family(layer.family)||layer.opaque||(layer.common.depth!=32&&layer.common.depth!=64)
       ||(reference?(!std::isfinite(layer.common.resolution)||layer.common.resolution<=0):layer.common.resolution!=dpi)||layer.planes.size()>1024||layer.common.width<1||layer.common.height<1
       ||layer.common.width>100000||layer.common.height>100000)
        throw std::invalid_argument("Unsupported Paint cache owner");
    //63C4A0 uses the same normal terminal for owner1/4. Main preview
    //applies its stack stop separately; document image output keeps all layers.
    // The raw pool and active embedded images are prepared once per owner.
    // Bound source and derived work before publishing the complete batch.
    constexpr std::size_t work_limit=128U*1024U*1024U;
    std::size_t source_size=layer.specific.size();
    if(source_size>work_limit)throw std::length_error("Paint cache source exceeds limits");
    for(const auto& p:layer.planes){
        if(p.specific.size()>work_limit-source_size)throw std::length_error("Paint cache source exceeds limits");
        source_size+=p.specific.size();
    }
    if(source_size>work_limit)throw std::length_error("Paint cache source exceeds limits");
    PaintOwnerRender owner;owner.context=request?*request:VectorRenderContext{layer.common.width,layer.common.height,depth,dpi,16.0,{},std::nullopt,2};
    modal_detail::poll(stop);auto archive=decode_vector_paint_archive(layer);modal_detail::poll(stop);
    // 62FC10 walks only the ordinary +9C plane collection. The separately
    // archived +3E8 Border lists have no independent ordinary-cache paint
    // instruction; decode still validates all of their graph references.
    const auto w=std::uint64_t(owner.context.width),h=std::uint64_t(owner.context.height);
    const auto logical=((w+255)/256)*((h+255)/256)*256*256*std::uint64_t(depth/8)+((w+31)/32)*h*4+((w+255)/256)*((h+255)/256)+(costs?costs->storage:0);
    //References use one Line/Face program even for Face-only owners,
    //so complete query/budget admission precedes Texture decompression.
    bool mixed=reference||owner.context.oversample==1;
    for(const auto& p:layer.planes)if(p.family==0x21110001||p.family==0x21120001||p.family==0x21210001||p.family==0x21220001)
        mixed=mixed||!vector_line_plane_references(p).empty();
    // Empty Line lists still share the owner attachment graph with Face
    // and Border records. The mixed projector performs this check itself.
    if(!mixed)vector_archive_detail::validate_stored_line_attachments(archive.base);
    std::map<int,std::shared_ptr<const VectorShapeTextureRecipe>> recipes;
    owner.geometry=mixed?vector_render_detail::project_stored_paint_faces(archive,layer,recipes,logical):vector_archive_detail::project_native_paint_faces(archive,layer,logical);
    owner.faces.resize(layer.planes.size());
    std::uint64_t derived_work=source_size;
    const auto charge=[&](std::uint64_t n){if(n>work_limit-derived_work)return false;derived_work+=n;return true;};
    std::array<std::uint64_t,8> graph_counts{};
    const auto graph_charge=[&](std::size_t field,std::uint64_t n){
        if(n>100000-graph_counts[field])return false;graph_counts[field]+=n;return true;
    };
    bool checked_lines=false;
    for(std::size_t i=0;i<layer.planes.size();++i){
        modal_detail::poll(stop);const auto& p=layer.planes[i];const auto& c=p.common;
        //62FC10->646E20 does not read Paint child density or mode.
        //Child4 input projection belongs to the producer, not saved materials.
        const bool paint_child=p.family==0x22100001||p.family==0x22100002;
        const bool ordinary_metadata=paint_child?(c.opacity>=0&&c.opacity<=100&&c.opacity_mode>=1&&c.opacity_mode<=5)
            :(c.opacity==100&&c.opacity_mode==1);
        if(p.opaque||p.raster||(!reference&&owner.context.oversample!=1&&(c.x||c.y||!zero_position(c)
           ||!ordinary_metadata)))throw std::invalid_argument("Unsupported Paint cache child metadata");
        if(p.family==0x22100001||p.family==0x22100002){
            for(const auto& face:owner.geometry[i].faces){
                const auto& g=face.geometry;
                const auto stops=g.gradation?g.gradation->stops.size():0;
                if(!graph_charge(7,stops)||!charge(stops))throw std::length_error("Paint cache aggregate graph or work limit exceeded");
                std::uint64_t contours{},spans{},members{};
                for(const auto& model:g.models)spans+=model.spans.size();
                for(const auto& loop:g.loops)members+=loop.members.size();
                const auto contour=[&](std::uint32_t id){for(const auto& member:g.loops.at(id).members)
                    contours+=g.models.at(member.model).spans.size();};
                contour(g.outer_loop);for(auto hole:g.holes)contour(hole);
                // Apply the AA owner's aggregate graph caps incrementally
                // before retaining another plane's expanded graph copies.
                if(!graph_charge(0,1)||!graph_charge(1,g.vertices.size())||!graph_charge(2,g.models.size())
                   ||!graph_charge(3,g.loops.size())||!graph_charge(4,g.holes.size())
                   ||!graph_charge(5,spans)||!graph_charge(6,members))throw std::length_error("Paint cache aggregate graph or work limit exceeded");
                // Retain the conservative quadratic geometry-work reservation
                // across all planes, including hidden siblings, alongside
                // bounded structural projection and actual scanline costs.
                if(!charge(contours*contours+g.vertices.size()+g.models.size()+g.loops.size()+spans+members))throw std::length_error("Paint cache aggregate graph or work limit exceeded");
            }
        }else if(p.family==0x21110001||p.family==0x21120001||p.family==0x21210001||p.family==0x21220001){
            if(!checked_lines){(void)validated_vector_plane_line_references(layer,i);checked_lines=true;}
        }else throw std::invalid_argument("Unsupported Paint cache child metadata");
        const bool owner_metadata=c.width==layer.common.width&&c.height==layer.common.height
            &&c.depth==layer.common.depth&&std::isfinite(c.resolution)&&c.resolution>0;
        // 60D370/63B280 leave vector plane dimensions/depth at zero;
        // the paint/line factory receives ownerDPI*16. Appending native
        // geometry keeps zero depth; crop/resolution workers may derive
        // the ratio16 carrier dimensions while retaining vector storage.
        const bool native_metadata=c.depth==0&&std::isfinite(c.resolution)&&c.resolution>0
            &&((c.width==0&&c.height==0)||(std::int64_t(c.width)==std::int64_t(layer.common.width)*16
                 &&std::int64_t(c.height)==std::int64_t(layer.common.height)*16));
        if(!reference&&owner.context.oversample!=1&&!owner_metadata&&!native_metadata)throw std::invalid_argument("Unsupported Paint cache child metadata");
    }
    owner.views.reserve(layer.planes.size());
    for(std::size_t i=0;i<owner.geometry.size();++i){
        for(const auto& face:owner.geometry[i].faces)owner.faces[i].push_back(&face.geometry);
        owner.views.push_back({owner.faces[i]});
    }
    if(mixed){
        auto lines=vector_render_detail::project_stored_paint_lines(layer,archive.base);
        if(!charge(archive.faces.size()*(1+std::bit_width(archive.faces.size()))))throw std::length_error("Paint cache aggregate graph or work limit exceeded");
        std::map<int,int> material_by_face;for(const auto& r:archive.faces)material_by_face.emplace(r.index,r.value.material);
        owner.planes.resize(layer.planes.size());
        for(std::size_t i=0;i<owner.planes.size();++i){owner.planes[i].lines=std::move(lines[i]);
            owner.planes[i].faces=std::move(owner.faces[i]);owner.planes[i].visible=layer.planes[i].common.flags[1]!=0;
            for(const auto& f:owner.geometry[i].faces)
                owner.planes[i].texture_recipes.push_back((f.geometry.material_flags&15)==4?recipes.at(material_by_face.at(f.face_index)):nullptr);}
        owner.preparation_work=derived_work;
        if(costs){auto prior=*costs;if(derived_work>128ull*1024*1024-prior.work)throw std::length_error("Paint cumulative preparation work exceeds limits");prior.work+=derived_work;*costs=(reference==1?vector_render_detail::stored_paint_reference_costs:reference==2?vector_render_detail::stored_paint_direct_costs:vector_render_detail::stored_paint_planes_costs)(owner.planes,owner.context,prior,stop);}else if(!vector_render_detail::can_render_stored_paint_planes(owner.planes,owner.context,derived_work))throw std::invalid_argument("Unsupported Paint cache child metadata");
        return owner;
    }
    owner.preparation_work=derived_work;
    // Include hidden siblings in the whole-batch structural/resource
    // admission; visibility only changes publication after certification.
    if(costs){auto prior=*costs;if(derived_work>128ull*1024*1024-prior.work)throw std::length_error("Face cumulative preparation work exceeds limits");prior.work+=derived_work;*costs=vector_render_detail::stored_face_planes_costs(owner.views,owner.context,prior,stop);}else if(!can_render_vector_face_planes(owner.views,owner.context))throw std::invalid_argument("Unsupported Paint cache child metadata");
    for(std::size_t i=owner.views.size();i-->0;)if(!layer.planes[i].common.flags[1])owner.views.erase(owner.views.begin()+i);
    modal_detail::poll(stop);return owner;
}
std::optional<PaintOwnerRender> paint_owner_render(const Layer& layer,double dpi,int depth,const VectorRenderContext* request=nullptr,vector_render_detail::StoredLineCacheCosts* costs=nullptr){
    if(!paint_owner_family(layer.family)||layer.opaque||layer.common.opacity!=100||(layer.common.opacity_mode!=1&&layer.common.opacity_mode!=4)
       ||(layer.common.depth!=32&&layer.common.depth!=64)||layer.common.resolution!=dpi||!zero_position(layer.common)
       ||layer.planes.size()>1024||layer.common.width<1||layer.common.height<1||layer.common.width>100000||layer.common.height>100000)return {};
    try{return prepare_paint_owner(layer,dpi,depth,request,costs);}
    catch(const FormatError&){return {};}
    catch(const std::invalid_argument&){return {};}
    catch(const std::length_error&){return {};}
}
std::optional<VectorOwnerRender> vector_owner_render(const Layer& layer,double dpi,int depth){
    if(layer.opaque||layer.common.opacity!=100||layer.common.opacity_mode!=1
       ||(layer.common.depth!=32&&layer.common.depth!=64)
       ||layer.common.resolution!=dpi||layer.planes.empty()
       ||(layer.family!=0x22100000&&layer.family!=0x24400000&&layer.family!=0x20000200))return {};
    // Projection currently validates the shared owner graph for each plane.
    // Bound that repeated work before parsing attacker-controlled pool bytes.
    constexpr std::size_t projection_work=128U*1024U*1024U;
    if(layer.planes.size()>1024||layer.specific.size()>projection_work/layer.planes.size())return {};
    VectorOwnerRender owner;
    owner.context={layer.common.width,layer.common.height,depth,dpi,16.0,{},std::nullopt,2};
    try{
        // This RGBA output path rerenders saved LC materials directly. The
        // separate formal Raster8/Common.color display cache is in PreviewScene.
        // Current output sampling remains the rebuild's bounded normal2 policy.
        owner.stored_planes=vector_render_detail::decode_stored_line_planes(layer);
        if(!vector_render_detail::can_render_stored_line_planes(owner.stored_planes,owner.context))return {};
        std::uint64_t reservation{};for(auto& p:owner.stored_planes){reservation+=p.reserved_storage;p.reserved_storage=0;}
        for(std::size_t i=owner.stored_planes.size();i-->0;)if(!layer.planes[i].common.flags[1])owner.stored_planes.erase(owner.stored_planes.begin()+i);
        if(!owner.stored_planes.empty())owner.stored_planes.front().reserved_storage=reservation;
    }catch(const FormatError&){return {};}
    catch(const std::invalid_argument&){return {};}
    catch(const std::length_error&){return {};}
    return owner;
}
struct RulerOwnerRender {VectorRenderContext context;std::vector<VectorRulerPlaneGeometry> planes;};
std::optional<RulerOwnerRender> ruler_owner_render(const Layer& layer,double dpi,int depth,vector_render_detail::StoredLineCacheCosts* costs=nullptr){
    if(layer.family!=0x40004000||layer.opaque||layer.common.opacity!=100||layer.common.opacity_mode!=1
       ||(layer.common.depth!=32&&layer.common.depth!=64)||layer.common.resolution!=dpi||!zero_position(layer.common))return {};
    RulerOwnerRender owner;owner.context={layer.common.width,layer.common.height,depth,dpi,16.0,{},std::nullopt,2};
    try{
        owner.planes=decode_vector_ruler_geometries(layer);
        // Hidden siblings participate in complete graph/domain/storage checks.
        if(costs)*costs=vector_render_detail::ruler_output_costs(owner.planes,owner.context,*costs);else if(!can_render_vector_ruler_geometries(owner.planes,owner.context))return {};
        std::uint64_t reservation{};for(auto& p:owner.planes){reservation+=p.reserved_storage;p.reserved_storage=0;}
        for(std::size_t i=owner.planes.size();i-->0;)if(!layer.planes[i].common.flags[1])owner.planes.erase(owner.planes.begin()+i);
        if(!owner.planes.empty())owner.planes.front().reserved_storage=reservation;
    }catch(const FormatError&){return {};}catch(const std::invalid_argument&){return {};}catch(const std::length_error&){return {};}
    return owner;
}
struct ShapeOwnerRender {VectorRenderContext context;std::vector<VectorShapePlaneGeometry> planes;};
std::optional<ShapeOwnerRender> shape_owner_render(const Layer& layer,double dpi,int depth,vector_render_detail::StoredLineCacheCosts* costs=nullptr){
    if(layer.family!=0x40002000||layer.opaque||layer.common.opacity!=100||layer.common.opacity_mode!=1
       ||(layer.common.depth!=32&&layer.common.depth!=64)||layer.common.resolution!=dpi||!zero_position(layer.common))return {};
    ShapeOwnerRender owner;owner.context={layer.common.width,layer.common.height,depth,dpi,16.0,{},std::nullopt,2};
    try{
        owner.planes=decode_vector_shape_geometries(layer);
        if(costs)*costs=vector_render_detail::shape_output_costs(owner.planes,owner.context,*costs);else if(!can_render_vector_shape_geometries(owner.planes,owner.context))return {};
        std::uint64_t reservation{};for(auto& p:owner.planes){reservation+=p.reserved_storage;p.reserved_storage=0;}
        for(std::size_t i=owner.planes.size();i-->0;)if(!layer.planes[i].common.flags[1])owner.planes.erase(owner.planes.begin()+i);
        if(!owner.planes.empty())owner.planes.front().reserved_storage=reservation;
    }catch(const FormatError&){return {};}catch(const std::invalid_argument&){return {};}catch(const std::length_error&){return {};}
    return owner;
}
void composite_owner_raster(Raster& destination,const Raster& source,const Layer& layer,const adjust_render_detail::Tables* tables){
    for(const auto& [position,bytes]:source.tiles){
        for(int y=position.y*256;y<std::min(source.height,(position.y+1)*256);++y)
            for(int x=position.x*256;x<std::min(source.width,(position.x+1)*256);++x){
                const auto dx=std::int64_t(x)+layer.common.x,dy=std::int64_t(y)+layer.common.y;
                if(dx<0||dy<0||dx>=destination.width||dy>=destination.height)continue;
                const auto pixel=adjust_render_detail::general_rgb(source.pixel(x,y),tables);
                if(pixel.a)destination.blend_pixel(int(dx),int(dy),pixel);
            }
    }
}
}
int BaseInfo::width() const { return pixels(lengths[0],dpi); }
int BaseInfo::height() const { return pixels(lengths[1],dpi); }
Plane* DocumentData::active_plane() {
    if(current_layer<0||current_layer>=int(layers.size()))return nullptr;
    auto& layer=layers[current_layer];
    if(layer.current_plane<0||layer.current_plane>=int(layer.planes.size()))return nullptr;
    return &layer.planes[layer.current_plane];
}
const Plane* DocumentData::active_plane() const { return const_cast<DocumentData*>(this)->active_plane(); }
std::optional<Raster> vector_render_detail::paint_cache_for_print(const Layer& layer,const VectorRenderContext& context,
                                                                   StoredLineCacheCosts& costs,std::stop_token stop){
    // Print consumes the certified normal cache, independently of the wider
    // metadata domain of the direct image and reference materializers.
    for(const auto& plane:layer.planes){
        const auto& c=plane.common;
        const bool owner_metadata=c.width==layer.common.width&&c.height==layer.common.height
            &&c.depth==layer.common.depth&&c.resolution==layer.common.resolution;
        const bool native_metadata=c.depth==0&&c.resolution==layer.common.resolution*16
            &&((c.width==0&&c.height==0)||(std::int64_t(c.width)==std::int64_t(layer.common.width)*16
                &&std::int64_t(c.height)==std::int64_t(layer.common.height)*16));
        if(c.x||c.y||!zero_position(c)||c.opacity!=100
            ||(c.opacity_mode!=1&&!(plane.family==0x22100001u&&c.opacity_mode==4))
            ||!std::isfinite(c.resolution)||c.resolution<=0||(!owner_metadata&&!native_metadata))
            throw std::invalid_argument("Unsupported Paint print child metadata");
    }
    const auto preceding_work=costs.work;
    auto owner=prepare_paint_owner(layer,context.resolution,context.depth,&context,&costs,stop);
    if(!layer.runtime_vector_cache_present)return {}; //62E720 skips a null owner+100.
    // Cost admission and rendering each prepare the programs. Reserve a
    // conservative second allowance before the renderer allocates output.
    modal_detail::charge(costs.work,costs.work-preceding_work,modal_detail::max_work,"Paint print repeated preparation work exceeds128Mi");
    const int columns=(context.width+255)/256,rows=(context.height+255)/256;
    modal_detail::charge(costs.storage,std::uint64_t(columns)*rows*128,modal_detail::max_bytes,"Paint print block metadata exceeds512MiB");
    modal_detail::poll(stop);
    auto result=owner.planes.empty()?render_vector_face_planes(owner.views,owner.context,stop)
                                   :render_stored_paint_planes(owner.planes,owner.context,stop,owner.preparation_work);
    // Fresh full-page62E820 owns every block. Normal2's648FD0 receives
    // compose=1 and preserves initial white A0, including physical padding.
    // Dense pixel backing is already reserved by the owner cost plan.
    for(int y=0;y<rows;++y)for(int x=0;x<columns;++x){modal_detail::poll(stop);
        if(!result.tiles.contains({x,y})){
            modal_detail::charge(costs.work,65536,modal_detail::max_work,"Paint print blank initialization work exceeds128Mi");
            result.set_pixel(x*256,y*256,{});
        }
    }
    modal_detail::poll(stop);return result;
}
Raster vector_render_detail::paint_reference_redraw(const Layer& layer,const VectorRenderContext& context,StoredLineCacheCosts& costs,std::stop_token stop){
    if(context.depth!=32||(context.oversample!=1&&context.oversample!=2))throw std::invalid_argument("Paint reference requires target32/normal1-or-AA2");
    const auto preceding_work=costs.work;
    auto owner=prepare_paint_owner(layer,context.resolution,32,&context,&costs,stop,true);
    if(!layer.runtime_vector_cache_present)throw std::invalid_argument("Paint reference redraw requires a runtime cache carrier");
    modal_detail::charge(costs.work,costs.work-preceding_work,modal_detail::max_work,"Paint reference repeated preparation work exceeds128Mi");
    const int columns=(context.width+255)/256,rows=(context.height+255)/256;
    modal_detail::charge(costs.storage,std::uint64_t(columns)*rows*128,modal_detail::max_bytes,"Paint reference block metadata exceeds512MiB");
    //References use the joint program even for empty/Face-only input, retaining
    //all Face materials until complete query/resource admission closes.
    auto result=render_stored_paint_reference(owner.planes,owner.context,stop,owner.preparation_work);
    for(int y=0;y<rows;++y)for(int x=0;x<columns;++x){modal_detail::poll(stop);if(!result.tiles.contains({x,y})){
        modal_detail::charge(costs.work,65536,modal_detail::max_work,"Paint reference blank initialization work exceeds128Mi");
        result.set_pixel(x*256,y*256,{});
    }}
    modal_detail::poll(stop);return result;
}
Raster vector_render_detail::paint_direct_redraw(const Layer& layer,const VectorRenderContext& context,StoredLineCacheCosts& costs,std::stop_token stop){
    if((context.depth!=32&&context.depth!=64)||(context.oversample!=1&&context.oversample!=2&&context.oversample!=4))throw std::invalid_argument("Paint direct redraw requires target32/64 and normal1/AA2/weighted4");
    const auto preceding_work=costs.work;
    auto owner=prepare_paint_owner(layer,context.resolution,context.depth,&context,&costs,stop,2);
    if(!layer.runtime_vector_cache_present)throw std::invalid_argument("Paint direct redraw requires a runtime cache carrier");
    modal_detail::charge(costs.work,costs.work-preceding_work,modal_detail::max_work,"Paint direct repeated preparation work exceeds128Mi");
    const int columns=(context.width+255)/256,rows=(context.height+255)/256;
    modal_detail::charge(costs.storage,std::uint64_t(columns)*rows*128,modal_detail::max_bytes,"Paint direct block metadata exceeds512MiB");
    auto result=render_stored_paint_direct(owner.planes,owner.context,stop,owner.preparation_work);
    //62F4A0 locks every requested block before62FC10 draws its children.
    // All newly locked blank blocks have identical physical contents. Share
    // one backing until a caller writes through Raster's copy-on-write API.
    std::shared_ptr<Bytes> blank;
    for(int y=0;y<rows;++y)for(int x=0;x<columns;++x){
        modal_detail::poll(stop);const TileCoord tile{x,y};
        if(result.tiles.contains(tile))continue;
        if(!blank){
            modal_detail::charge(costs.work,65536,modal_detail::max_work,"Paint direct blank initialization work exceeds128Mi");
            result.set_pixel(x*256,y*256,{});blank=result.tiles.at(tile);
        }else result.tiles.emplace(tile,blank);
    }
    modal_detail::poll(stop);return result;
}
Raster selection_detail::owner_helper(const Layer& layer,double dpi,vector_render_detail::StoredLineCacheCosts* costs){
    const auto depth=layer.common.depth;
    if(const auto owner=ruler_owner_render(layer,dpi,depth,costs)){return render_vector_ruler_geometries(owner->planes,owner->context);}
    if(const auto owner=shape_owner_render(layer,dpi,depth,costs)){return render_vector_shape_geometries(owner->planes,owner->context);}
    if(const auto owner=paint_owner_render(layer,dpi,depth))
        return owner->planes.empty()?render_vector_face_planes(owner->views,owner->context):vector_render_detail::render_stored_paint_planes(owner->planes,owner->context,{},owner->preparation_work);
    if(const auto owner=vector_owner_render(layer,dpi,depth)){
        if(depth==32&&(layer.family==0x24400000||layer.family==0x22100000)&&std::all_of(layer.planes.begin(),layer.planes.end(),[](const Plane& p){return p.family==0x21210001||p.family==0x21220001;}))
            return vector_render_detail::render_stored_line_cache_planes(owner->stored_planes,owner->context);
        return vector_render_detail::render_stored_line_planes(owner->stored_planes,owner->context);
    }
    throw std::invalid_argument("Selection vector helper exceeds the supported owner-render contract");
}
Raster selection_detail::motion_owner_helper(const Layer& layer,int width,int height,double dpi,vector_render_detail::StoredLineCacheCosts& costs){
    if(layer.common.opacity<0||layer.common.opacity>100||layer.common.opacity_mode!=1||std::abs(std::int64_t(layer.common.x))>65536||std::abs(std::int64_t(layer.common.y))>65536)throw std::invalid_argument("Unsupported MotionCheck vector owner metadata");
    auto prepared=layer;prepared.common.opacity=100;
    const VectorRenderContext request{width,height,32,dpi,16.0,{-double(layer.common.x),-double(layer.common.y)},std::nullopt,2};
    if(const auto owner=paint_owner_render(prepared,dpi,32,&request,&costs)){return owner->planes.empty()?render_vector_face_planes(owner->views,owner->context):vector_render_detail::render_stored_paint_planes(owner->planes,owner->context,{},owner->preparation_work);}
    if(layer.family==0x20000200){if(layer.common.resolution!=dpi)throw std::invalid_argument("MotionCheck Temp owner requires same DPI");auto planes=vector_render_detail::decode_stored_line_planes(prepared);costs=vector_render_detail::stored_line_output_costs(planes,request,vector_render_detail::StoredLineOutputSampling::AA2,costs);for(std::size_t i=planes.size();i-->0;)if(!layer.planes[i].common.flags[1])planes.erase(planes.begin()+i);return vector_render_detail::render_stored_line_output(planes,request,vector_render_detail::StoredLineOutputSampling::AA2);}
    throw std::invalid_argument("Unsupported MotionCheck vector owner request");
}
DocumentData selection_detail::query_data(const DocumentData& data){
    const auto* plane=data.active_plane();if(!plane||(plane->family&0xf0000000u)!=0x20000000u)return data;
    auto result=data;auto& owner=result.layers.at(std::size_t(result.current_layer));
    const auto helper=owner_helper(owner,owner.common.resolution);
    Plane target;target.family=0x10000001;target.common=owner.common;target.common.opacity_mode=1;target.common.width=helper.width;target.common.height=helper.height;target.common.depth=helper.depth;
    target.raster=helper;owner.planes={std::move(target)};owner.current_plane=0;owner.family=0x10000000;owner.specific.clear();owner.suffix.clear();return result;
}
namespace {
bool direct_family(const Layer& l){return l.family==0x22100000||l.family==0x24400000||l.family==0x20000200||paint_owner_family(l.family);}
std::optional<Raster> direct_owner_image(const Layer& l,int width,int height,double dpi,int depth,std::optional<VectorBounds> clip={},std::stop_token stop={}){
    using namespace vector_render_detail;
    if(!direct_family(l)||l.opaque||l.common.opacity<0||l.common.opacity>100||(paint_owner_family(l.family)&&l.common.opacity!=100)||(l.common.opacity_mode!=1&&!(paint_owner_family(l.family)&&l.common.opacity_mode==4))||(paint_owner_family(l.family)&&!zero_position(l.common))||(l.common.depth!=32&&l.common.depth!=64))return {};
        auto context=direct_output_context(l,{0,0,width,height},dpi,depth,2);context.clip=clip;
        StoredLineCacheCosts costs;
        if(paint_owner_family(l.family)){
            auto owner=prepare_paint_owner(l,dpi,depth,&context,&costs,stop,2);
            modal_detail::charge(costs.work,costs.work,modal_detail::max_work,"Paint direct repeated preparation work exceeds128Mi");
            if(!l.runtime_vector_cache_present)throw std::invalid_argument("Direct vector redraw requires a runtime cache carrier");
            return render_stored_paint_direct(owner.planes,context,stop,owner.preparation_work);
        }
        if(l.planes.empty()||l.planes.size()>1024||l.specific.size()>128ull*1024*1024/l.planes.size())return {};
        auto planes=decode_stored_line_planes(l);(void)stored_line_direct_costs(planes,context,StoredLineOutputSampling::AA2,{},stop);
        if(!l.runtime_vector_cache_present)throw std::invalid_argument("Direct vector redraw requires a runtime cache carrier");
        std::uint64_t reserved{};for(auto& p:planes){reserved+=p.reserved_storage;p.reserved_storage=0;}
        for(std::size_t i=planes.size();i-->0;)if(!l.planes[i].common.flags[1])planes.erase(planes.begin()+i);
        if(!planes.empty())planes.front().reserved_storage=reserved;
        return render_stored_line_direct(planes,context,StoredLineOutputSampling::AA2,stop);
}
}
bool DocumentData::has_unrendered_content() const {
    if(unrestored_external_content)return true;
    if(vector_render_detail::requires_native_paint_image(*this)){
        try{return !vector_render_detail::render_native_line_document(*this,32,vector_render_detail::LineDocumentOutput::StandardImage);}
        catch(const FormatError&){return true;}catch(const std::invalid_argument&){return true;}catch(const std::length_error&){return true;}
    }
    if(text_font_detail::requires_native_image(*this)&&!vector_render_detail::supports_native_line_document(*this,vector_render_detail::LineDocumentOutput::StandardImage))return true;
    try{(void)adjust_render_detail::prepare(*this);}
    catch(const FormatError&){return true;}catch(const std::invalid_argument&){return true;}catch(const std::length_error&){return true;}
    for(const auto& l:layers){
        if(l.opaque)return true;
        if(l.family==0x40000200)continue;
        // Stylos Frame guides carry layout metadata and never supply image pixels.
        if(l.family==0x40001000&&std::ranges::all_of(l.planes,[](const Plane& p){
            return !p.opaque && p.family==0x40001008;
        }))continue;
        if(direct_family(l)&&(l.common.resolution!=base.dpi||l.common.width!=base.width()||l.common.height!=base.height())){try{if(!direct_owner_image(l,base.width(),base.height(),base.dpi,32))return true;}catch(const std::exception&){return true;}continue;}
        if(l.family==0x40002000){if(!shape_owner_render(l,l.common.resolution,32))return true;continue;}
        if(l.family==0x40004000){if(!ruler_owner_render(l,l.common.resolution,32))return true;continue;}
        if(paint_owner_family(l.family)){if(!paint_owner_render(l,base.dpi,32))return true;continue;}
        if(std::any_of(l.planes.begin(),l.planes.end(),[&](const auto& p){const auto* text=text_raster_cache(l,p,base.dpi);return !p.raster&&!text&&!text_font_detail::image_masks(l,p,base.dpi);})
           &&!vector_owner_render(l,base.dpi,32))return true;
    }
    return false;
}
Raster DocumentData::composite() const {return composite(32);}
Raster DocumentData::composite(int target_depth) const {return composite(target_depth,{});}
Raster DocumentData::composite(int target_depth,std::stop_token stop) const {return composite(target_depth,stop,false);}
Raster DocumentData::composite(int target_depth,std::stop_token stop,bool white_background) const {
    const auto poll=[&]{if(stop.stop_requested())throw std::runtime_error("Document composition cancelled");};poll();
    if(target_depth!=32&&target_depth!=64)throw std::invalid_argument("Composite output depth must be 32 or 64");
    if(text_font_detail::requires_native_image(*this)||vector_render_detail::requires_native_paint_image(*this)){
        if(auto image=vector_render_detail::render_native_line_document(*this,target_depth,white_background?vector_render_detail::LineDocumentOutput::StandardImageWhite:vector_render_detail::LineDocumentOutput::StandardImage,stop))return std::move(*image);
        throw std::logic_error("Image metadata requires a supported native whole-document image profile");
    }
    const auto remaps=adjust_render_detail::prepare(*this,stop);
    Raster result(base.width(),base.height(),target_depth,base.dpi);
    if(white_background){const auto tiles=std::uint64_t((result.width+255)/256)*((result.height+255)/256);if(tiles>512ull*1024*1024/result.tile_bytes())throw std::length_error("White image background exceeds512MiB storage");Pixel paper=Pixel::rgba8(255,255,255);for(auto i=layers.rbegin();i!=layers.rend();++i)if(i->common.flags[1]&&(i->family&0x04000000u)){const auto color=i->common.color;paper=Pixel::rgba8((color>>8)&255,(color>>24)&255,(color>>40)&255);break;}for(int y=0;y<result.height;++y){poll();for(int x=0;x<result.width;++x)result.set_pixel(x,y,paper);}}

    for(const auto& l:layers){
        poll();if(!l.common.flags[1]||l.family==0x40000200)continue;
        const auto* tables=remaps.empty()?nullptr:remaps[std::size_t(&l-layers.data())].get();
        if(direct_family(l)&&(l.common.resolution!=base.dpi||l.common.width!=base.width()||l.common.height!=base.height())){
            if(const auto image=direct_owner_image(l,base.width(),base.height(),base.dpi,target_depth,{},stop)){
                // Keep normal2 sampling and the existing straight-over stack;
                // apply native typed owner density once to the page helper.
                const auto density=vector_render_detail::output_terminal::density(l.common.opacity,target_depth);
                for(const auto& [tile,bytes]:image->tiles)for(int y=tile.y*256;y<std::min(image->height,(tile.y+1)*256);++y){poll();for(int x=tile.x*256;x<std::min(image->width,(tile.x+1)*256);++x){
                    const auto pixel=vector_render_detail::output_terminal::raster_source(image->pixel(x,y),target_depth,target_depth,density,false,tables);
                    if(pixel.a)result.blend_pixel(x,y,pixel);
                }}
            }else if(target_depth==64&&direct_owner_image(l,base.width(),base.height(),base.dpi,32,VectorBounds{0,0,0,0}))throw std::length_error("Direct vector owner exceeds target-depth rendering limits");
            continue;
        }
        if(l.family==0x40002000){
            if(const auto owner=shape_owner_render(l,base.dpi,target_depth))composite_owner_raster(result,render_vector_shape_geometries(owner->planes,owner->context,stop),l,tables);
            else if(target_depth==64&&shape_owner_render(l,base.dpi,32))throw std::length_error("Shape owner exceeds target-depth rendering limits");
            continue;
        }
        if(l.family==0x40004000){
            if(const auto owner=ruler_owner_render(l,base.dpi,target_depth))composite_owner_raster(result,render_vector_ruler_geometries(owner->planes,owner->context,stop),l,tables);
            else if(target_depth==64&&ruler_owner_render(l,base.dpi,32))throw std::length_error("Ruler owner exceeds target-depth rendering limits");
            continue;
        }
        if(paint_owner_family(l.family)){
            if(const auto owner=paint_owner_render(l,base.dpi,target_depth))
                composite_owner_raster(result,owner->planes.empty()?render_vector_face_planes(owner->views,owner->context,stop):
                    vector_render_detail::render_stored_paint_planes(owner->planes,owner->context,stop,owner->preparation_work),l,tables);
            else if(target_depth==64&&paint_owner_render(l,base.dpi,32))
                throw std::length_error("Paint owner exceeds target-depth rendering limits");
            continue;
        }
        if(const auto owner=vector_owner_render(l,base.dpi,target_depth)){
            const auto derived=vector_render_detail::render_stored_line_planes(owner->stored_planes,owner->context,stop);
            composite_owner_raster(result,derived,l,tables);
            continue;
        }
        if(target_depth==64&&vector_owner_render(l,base.dpi,32))throw std::length_error("Vector Line owner exceeds target-depth rendering limits");
        for(const auto& p:l.planes){
            poll();const auto* text=text_raster_cache(l,p,base.dpi);const auto* image=p.raster?&*p.raster:text;if(!image||!p.common.flags[1])continue;
            if(const auto scale=line_render_scale(p,base.dpi)){
                const auto& raster=*p.raster;
                const int tile_side=256/scale;
                const auto render_block=[&](TileCoord position){
                    const auto left=std::max<std::int64_t>(std::int64_t(position.x)*tile_side,-std::int64_t(l.common.x));
                    const auto top=std::max<std::int64_t>(std::int64_t(position.y)*tile_side,-std::int64_t(l.common.y));
                    const auto right=std::min({std::int64_t(position.x)*tile_side+tile_side,std::int64_t(line_render_width(raster,scale)),std::int64_t(result.width)-l.common.x});
                    const auto bottom=std::min({std::int64_t(position.y)*tile_side+tile_side,std::int64_t(raster.height/scale),std::int64_t(result.height)-l.common.y});
                    for(auto y=top;y<bottom;++y)for(auto x=left;x<right;++x){
                        if((x&255)==0)poll();const auto color=line_render_pixel(l,p,int(x),int(y),scale,tables);
                        if(color.a)result.blend_pixel(int(x+l.common.x),int(y+l.common.y),color);
                    }
                };
                if(line_missing_white(l,scale)){
                    // Enumerate the clipped logical raster, including absent
                    // source blocks. Owner translation stays in int64 until
                    // clipping, including INT_MIN origins.
                    const auto left=std::max<std::int64_t>(0,-std::int64_t(l.common.x));
                    const auto top=std::max<std::int64_t>(0,-std::int64_t(l.common.y));
                    const auto right=std::min<std::int64_t>(raster.width,std::int64_t(result.width)-l.common.x);
                    const auto bottom=std::min<std::int64_t>(raster.height,std::int64_t(result.height)-l.common.y);
                    if(left<right&&top<bottom)
                        for(auto y=top/tile_side;y<=(bottom-1)/tile_side;++y)
                            for(auto x=left/tile_side;x<=(right-1)/tile_side;++x)
                                render_block({int(x),int(y)});
                }else for(const auto& [position,bytes]:raster.tiles)render_block(position);
                continue;
            }
            const bool native_text=!p.raster&&text&&p.runtime_text_cache->static_geometry;
            const auto alpha=native_text?std::uint16_t(65535):std::uint16_t(std::clamp(l.common.opacity,0,100)*std::clamp(p.common.opacity,0,100)*65535ull/10000);
            const auto& r=*image;
            for(const auto& [pos,bytes]:r.tiles){
                for(int y=pos.y*256;y<std::min(r.height,(pos.y+1)*256);++y)
                    for(int x=pos.x*256;x<std::min(r.width,(pos.x+1)*256);++x){
                        if((x&255)==0)poll();const auto c=r.pixel(x,y); if(!c.a)continue;
                        if(!native_text&&p.common.opacity_mode==5&&c.r==65535&&c.g==65535&&c.b==65535)continue;
                        const auto dx=std::int64_t(x)+l.common.x+(!p.raster&&text?p.runtime_text_cache->origin[0]:0);
                        const auto dy=std::int64_t(y)+l.common.y+(!p.raster&&text?p.runtime_text_cache->origin[1]:0);
                        if(dx<0||dy<0||dx>=result.width||dy>=result.height)continue;
                        result.blend_pixel(static_cast<int>(dx),static_cast<int>(dy),!native_text&&(r.depth==32||r.depth==64)?adjust_render_detail::general_rgb(c,tables):c,alpha);
                    }
            }
        }
    }
    return result;
}
Pixel DocumentData::composite_pixel(int x,int y) const {
    Pixel result;
    if(x<0||y<0||x>=base.width()||y>=base.height())return result;
    if(text_font_detail::requires_native_image(*this)||vector_render_detail::requires_native_paint_image(*this))return composite(32).pixel(x,y);
    const auto remaps=adjust_render_detail::prepare(*this);
    for(const auto& layer:layers){
        if(!layer.common.flags[1]||layer.family==0x40000200)continue;
        const auto* tables=remaps.empty()?nullptr:remaps[std::size_t(&layer-layers.data())].get();
        if(direct_family(layer)&&(layer.common.resolution!=base.dpi||layer.common.width!=base.width()||layer.common.height!=base.height())){
            if(const auto image=direct_owner_image(layer,base.width(),base.height(),base.dpi,32,VectorBounds{x,y,x+1,y+1})){
                const auto source=vector_render_detail::output_terminal::raster_source(image->pixel(x,y),32,32,vector_render_detail::output_terminal::density(layer.common.opacity,32),false,tables);
                if(source.a){result=over(result,source);result={std::uint16_t(result.r/257*257),std::uint16_t(result.g/257*257),std::uint16_t(result.b/257*257),std::uint16_t(result.a/257*257)};}
            }continue;
        }
        if(layer.family==0x40002000){
            if(const auto owner=shape_owner_render(layer,base.dpi,32)){
                const auto lx=std::int64_t(x)-layer.common.x,ly=std::int64_t(y)-layer.common.y;
                if(lx>=0&&ly>=0&&lx<owner->context.width&&ly<owner->context.height){const auto source=adjust_render_detail::general_rgb(vector_shape_geometries_pixel(owner->planes,owner->context,int(lx),int(ly)),tables);
                    if(source.a){result=over(result,source);result={std::uint16_t(result.r/257*257),std::uint16_t(result.g/257*257),std::uint16_t(result.b/257*257),std::uint16_t(result.a/257*257)};}}
            }
            continue;
        }
        if(layer.family==0x40004000){
            if(const auto owner=ruler_owner_render(layer,base.dpi,32)){
                const auto lx=std::int64_t(x)-layer.common.x,ly=std::int64_t(y)-layer.common.y;
                if(lx>=0&&ly>=0&&lx<owner->context.width&&ly<owner->context.height){const auto source=adjust_render_detail::general_rgb(vector_ruler_geometries_pixel(owner->planes,owner->context,int(lx),int(ly)),tables);
                    if(source.a){result=over(result,source);result={std::uint16_t(result.r/257*257),std::uint16_t(result.g/257*257),std::uint16_t(result.b/257*257),std::uint16_t(result.a/257*257)};}}
            }
            continue;
        }
        if(paint_owner_family(layer.family)){
            if(const auto owner=paint_owner_render(layer,base.dpi,32)){
                const auto lx=std::int64_t(x)-layer.common.x,ly=std::int64_t(y)-layer.common.y;
                if(lx>=0&&ly>=0&&lx<owner->context.width&&ly<owner->context.height){
                    const auto source=adjust_render_detail::general_rgb(owner->planes.empty()?vector_face_planes_pixel(owner->views,owner->context,int(lx),int(ly)):
                        vector_render_detail::stored_paint_planes_pixel(owner->planes,owner->context,int(lx),int(ly),owner->preparation_work),tables);
                    if(source.a){result=over(result,source);result={std::uint16_t(result.r/257*257),std::uint16_t(result.g/257*257),
                        std::uint16_t(result.b/257*257),std::uint16_t(result.a/257*257)};}
                }
            }
            continue;
        }
        if(const auto owner=vector_owner_render(layer,base.dpi,32)){
            const auto local_x=std::int64_t(x)-layer.common.x,local_y=std::int64_t(y)-layer.common.y;
            if(local_x<0||local_y<0||local_x>=owner->context.width||local_y>=owner->context.height)continue;
            const auto source=adjust_render_detail::general_rgb(vector_render_detail::stored_line_planes_pixel(owner->stored_planes,owner->context,int(local_x),int(local_y)),tables);
            if(!source.a)continue;
            result=over(result,source);
            result={std::uint16_t(result.r/257*257),std::uint16_t(result.g/257*257),
                    std::uint16_t(result.b/257*257),std::uint16_t(result.a/257*257)};
            continue;
        }
        for(const auto& plane:layer.planes){
            const auto* text=text_raster_cache(layer,plane,base.dpi);const auto* image=plane.raster?&*plane.raster:text;if(!image||!plane.common.flags[1])continue;
            if(const auto scale=line_render_scale(plane,base.dpi)){
                const auto local_x=std::int64_t(x)-layer.common.x;
                const auto local_y=std::int64_t(y)-layer.common.y;
                if(local_x<0||local_y<0||local_x>=line_render_width(*plane.raster,scale)||local_y>=plane.raster->height/scale)continue;
                const auto source=line_render_pixel(layer,plane,int(local_x),int(local_y),scale,tables);
                if(!source.a)continue;
                result=over(result,source);
                result={std::uint16_t(result.r/257*257),std::uint16_t(result.g/257*257),
                        std::uint16_t(result.b/257*257),std::uint16_t(result.a/257*257)};
                continue;
            }
            const auto local_x=std::int64_t(x)-layer.common.x-(!plane.raster&&text?plane.runtime_text_cache->origin[0]:0);
            const auto local_y=std::int64_t(y)-layer.common.y-(!plane.raster&&text?plane.runtime_text_cache->origin[1]:0);
            if(local_x<0||local_y<0||local_x>=image->width||local_y>=image->height)continue;
            const auto source=image->pixel(static_cast<int>(local_x),static_cast<int>(local_y));
            if(!source.a)continue;
            const bool native_text=!plane.raster&&text&&plane.runtime_text_cache->static_geometry;
            if(!native_text&&plane.common.opacity_mode==5&&source.r==65535&&source.g==65535&&source.b==65535)continue;
            const auto alpha=native_text?std::uint16_t(65535):std::uint16_t(std::clamp(layer.common.opacity,0,100)*std::clamp(plane.common.opacity,0,100)*65535ull/10000);
            result=over(result,!native_text&&(image->depth==32||image->depth==64)?adjust_render_detail::general_rgb(source,tables):source,alpha);
            // Match the existing 32-bit display compositor's per-plane
            // quantization, including intermediate alpha.
            result={std::uint16_t(result.r/257*257),std::uint16_t(result.g/257*257),
                    std::uint16_t(result.b/257*257),std::uint16_t(result.a/257*257)};
        }
    }
    return result;
}
DocumentData make_document(int w,int h,int depth,double dpi){
    Raster r(w,h,depth,dpi);
    DocumentData d;d.base.dpi=dpi;d.base.dialog_dpi=dpi;
    for(auto& l:d.base.lengths)l={0,0,dpi};
    d.base.lengths[0].value=d.base.lengths[4].value=d.base.lengths[14].value=w;
    d.base.lengths[1].value=d.base.lengths[5].value=d.base.lengths[15].value=h;
    d.base.lengths[2].value=w/2.0;d.base.lengths[3].value=h/2.0;
    Layer l;l.common.width=w;l.common.height=h;l.common.depth=depth;l.common.resolution=dpi;l.common.name=ascii("Layer 1");
    Plane p;p.common=l.common;p.common.name=ascii("Raster");p.raster=std::move(r);p.pixels_changed=true;
    l.planes.push_back(std::move(p));d.layers.push_back(std::move(l));return d;
}
namespace {
void require_selection_layout(const DocumentData& from,const DocumentData& to,HistorySelection selection,bool allow_appended_layers=false){
    if(selection!=HistorySelection::PreserveCurrent)return;
    if(from.layers.size()>to.layers.size()||(!allow_appended_layers&&from.layers.size()!=to.layers.size()))
        throw std::logic_error("Selection-preserving history cannot change layer membership");
    for(std::size_t i=0;i<from.layers.size();++i){
        const auto& a=from.layers[i];const auto& b=to.layers[i];
        if(a.family!=b.family||a.opaque!=b.opaque||a.planes.size()!=b.planes.size())
            throw std::logic_error("Selection-preserving history cannot replace layer objects");
        for(std::size_t p=0;p<a.planes.size();++p)
            if(a.planes[p].family!=b.planes[p].family||a.planes[p].opaque!=b.planes[p].opaque)
                throw std::logic_error("Selection-preserving history cannot replace plane objects");
    }
}
// Selection adoption intentionally does not advance the content generation.
// Freeze the fields it can replace without copying raster storage.
struct SelectionAdmission {
    std::vector<std::int64_t> values;
    template<class Emit> static void visit(const DocumentData& data,Emit emit){
        emit(data.current_layer);emit(data.layers.size());
        for(const auto& layer:data.layers){
            emit(layer.current_plane);emit(layer.common.flags[1]);emit(layer.common.flags[2]);emit(layer.planes.size());
            for(const auto& plane:layer.planes){emit(plane.common.flags[1]);emit(plane.common.flags[2]);}
        }
    }
    explicit SelectionAdmission(const DocumentData& data){visit(data,[&](auto value){values.push_back(value);});}
    bool matches(const DocumentData& data) const noexcept{
        std::size_t index{};bool equal=true;
        visit(data,[&](auto value){if(index>=values.size()||values[index]!=std::int64_t(value))equal=false;++index;});
        return equal&&index==values.size();
    }
};
template<class Members> std::vector<std::size_t> selected_group_order(const Members& members,std::size_t gap){
    if(gap>members.size())throw std::invalid_argument("排序插入位置超出对象范围。");
    std::vector<std::size_t> order;order.reserve(members.size());
    // sub_60E200 keeps each of the three segments in original order: unmarked
    // before the gap, every marked object, then unmarked after the gap.
    for(std::size_t i=0;i<gap;++i)if(!members[i].common.flags[2])order.push_back(i);
    for(std::size_t i=0;i<members.size();++i)if(members[i].common.flags[2])order.push_back(i);
    for(std::size_t i=gap;i<members.size();++i)if(!members[i].common.flags[2])order.push_back(i);
    return order;
}
std::shared_ptr<const FloatingSelection> remap_inserted_layer(const DocumentData& data,
    const std::shared_ptr<const FloatingSelection>& floating,std::size_t index,bool inserting){
    if(!floating)return {};
    if(floating->target){validate_targeted_floating(data,*floating);if(!inserting&&floating->target->layer==index)throw std::logic_error("无法移除仍绑定浮动内容的图层。");
        if(floating->target->layer<index)return floating;auto next=std::make_shared<FloatingSelection>(*floating);if(inserting)++next->target->layer;else --next->target->layer;return next;}
    if(!floating->sources&&!floating->same_document_source){validate_foreign_floating(data,*floating);return floating;}
    validate_local_floating(data,*floating);
    bool changed=false;
    for(const auto& source:*floating->sources){
        if(!inserting&&source.layer==index)throw std::logic_error("无法移除仍绑定浮动内容的图层。");
        changed|=source.layer>=index;
    }
    if(!changed)return floating;
    auto sources=std::make_shared<std::vector<FloatingSource>>(*floating->sources);
    for(auto& source:*sources)if(source.layer>=index){if(inserting)++source.layer;else --source.layer;}
    auto remapped=std::make_shared<FloatingSelection>(*floating);remapped->sources=std::move(sources);return remapped;
}
std::shared_ptr<const FloatingSelection> remap_inserted_plane(const DocumentData& data,
    const std::shared_ptr<const FloatingSelection>& floating,std::size_t layer,std::size_t index,bool inserting){
    if(!floating)return {};
    if(floating->target){validate_targeted_floating(data,*floating);if(floating->target->layer!=layer)return floating;
        if(!inserting&&floating->target->plane==index)throw std::logic_error("无法移除仍绑定浮动内容的平面。");if(floating->target->plane<index)return floating;
        auto next=std::make_shared<FloatingSelection>(*floating);if(inserting)++next->target->plane;else --next->target->plane;return next;}
    if(!floating->sources&&!floating->same_document_source){validate_foreign_floating(data,*floating);return floating;}
    validate_local_floating(data,*floating);bool changed=false;
    for(const auto& source:*floating->sources)if(source.layer==layer)for(auto plane:source.planes){
        if(!inserting&&plane==index)throw std::logic_error("无法移除仍绑定浮动内容的平面。");
        changed|=plane>=index;
    }
    if(!changed)return floating;
    auto sources=std::make_shared<std::vector<FloatingSource>>(*floating->sources);
    for(auto& source:*sources)if(source.layer==layer)for(auto& plane:source.planes)if(plane>=index){if(inserting)++plane;else --plane;}
    auto remapped=std::make_shared<FloatingSelection>(*floating);remapped->sources=std::move(sources);return remapped;
}
void select_inserted_plane(Layer& layer,std::size_t index) noexcept{
    layer.current_plane=int(index);for(std::size_t i=0;i<layer.planes.size();++i)layer.planes[i].common.flags[2]=i==index;
}
void remove_plane_selection(Layer& layer,std::size_t index) noexcept{
    if(layer.planes.empty()){layer.current_plane=-1;return;}
    bool repair=false;
    if(layer.current_plane>=0&&index<=std::size_t(layer.current_plane)){--layer.current_plane;repair=true;}
    if(layer.current_plane>=int(layer.planes.size())){layer.current_plane=int(layer.planes.size())-1;repair=true;}
    else if(layer.current_plane<0){layer.current_plane=0;repair=true;}
    if(repair)select_inserted_plane(layer,std::size_t(layer.current_plane));
}
void select_inserted_layer(DocumentData& data,std::size_t index) noexcept{
    data.current_layer=int(index);
    for(std::size_t i=0;i<data.layers.size();++i)data.layers[i].common.flags[2]=i==index;
}
void remove_layer_selection(DocumentData& data,std::size_t index) noexcept{
    if(data.layers.empty()){data.current_layer=-1;return;}
    bool repair=false;
    if(data.current_layer>=0&&index<=std::size_t(data.current_layer)){--data.current_layer;repair=true;}
    if(data.current_layer>=int(data.layers.size())){data.current_layer=int(data.layers.size())-1;repair=true;}
    else if(data.current_layer<0){data.current_layer=0;repair=true;}
    if(repair)for(std::size_t i=0;i<data.layers.size();++i)data.layers[i].common.flags[2]=int(i)==data.current_layer;
}
struct PreparedOrder {
    std::vector<std::size_t> inverse;
    std::vector<Layer> layers;
    std::vector<Plane> planes;
    std::shared_ptr<const FloatingSelection> floating;
    int current{};
};
void prepare_plane_transfer(Plane& destination,const Plane& source){
    // Allocate the MSVC empty-map sentinel before publishing history or pixels.
    if(source.raster)destination.raster.emplace();
}
void transfer_plane(Plane& destination,Plane& source) noexcept{
    static_assert(std::is_nothrow_swappable_v<CommonElement>);
    static_assert(std::is_nothrow_swappable_v<decltype(Raster::tiles)>);
    assert(destination.raster.has_value()==source.raster.has_value());
    using std::swap;
    swap(destination.opaque,source.opaque);swap(destination.family,source.family);
    swap(destination.common,source.common);
    destination.specific.swap(source.specific);destination.suffix.swap(source.suffix);destination.opaque_record.swap(source.opaque_record);
    swap(destination.pixels_changed,source.pixels_changed);swap(destination.raster_modified,source.raster_modified);swap(destination.visibility_modified,source.visibility_modified);
    swap(destination.runtime_text_cache,source.runtime_text_cache);
    swap(destination.runtime_adjust_cache,source.runtime_adjust_cache);
    swap(destination.frame_modified,source.frame_modified);swap(destination.vanishing_modified,source.vanishing_modified);
    if(source.raster){
        auto& to=*destination.raster;auto& from=*source.raster;
        swap(to.width,from.width);swap(to.height,from.height);swap(to.depth,from.depth);
        swap(to.resolution,from.resolution);swap(to.color_order,from.color_order);to.tiles.swap(from.tiles);
    }
}
void prepare_plane_removal(const Layer& owner,const std::vector<std::size_t>& indices,
                           std::vector<Plane>& survivors,std::vector<Plane>& detached){
    survivors.resize(owner.planes.size()-indices.size());
    std::size_t removed{},live{};
    for(std::size_t i=0;i<owner.planes.size();++i){
        if(removed<indices.size()&&indices[removed]==i)prepare_plane_transfer(detached[removed++],owner.planes[i]);
        else prepare_plane_transfer(survivors[live++],owner.planes[i]);
    }
}
void remove_prepared_planes(Layer& owner,const std::vector<std::size_t>& indices,
                            std::vector<Plane>& survivors,std::vector<Plane>& detached) noexcept{
    // Preserve the native sequential selection repair, including flags on
    // later detached planes, without vector::erase moving optional<Raster>.
    for(std::size_t i=0;i<indices.size();++i){
        transfer_plane(detached[i],owner.planes[indices[i]]);
        const auto index=indices[i]-i,remaining=owner.planes.size()-i-1;
        bool repair=false;
        if(owner.current_plane>=0&&index<=std::size_t(owner.current_plane)){--owner.current_plane;repair=true;}
        if(owner.current_plane>=int(remaining)){owner.current_plane=int(remaining)-1;repair=true;}
        else if(owner.current_plane<0){owner.current_plane=0;repair=true;}
        if(repair){
            std::size_t skipped{},ordinal{};
            for(std::size_t j=0;j<owner.planes.size();++j){
                if(skipped<=i&&indices[skipped]==j){++skipped;continue;}
                owner.planes[j].common.flags[2]=ordinal++==std::size_t(owner.current_plane);
            }
        }
    }
    std::size_t removed{},live{};
    for(std::size_t i=0;i<owner.planes.size();++i){
        if(removed<indices.size()&&indices[removed]==i){++removed;continue;}
        transfer_plane(survivors[live++],owner.planes[i]);
    }
    owner.planes.swap(survivors);
}
PreparedOrder prepare_order(const DocumentData& data,const std::shared_ptr<const FloatingSelection>& floating,
                            std::optional<std::size_t> layer,const std::vector<std::size_t>& order){
    if(layer&&*layer>=data.layers.size())throw std::invalid_argument("排序图层不存在。");
    const auto count=layer?data.layers[*layer].planes.size():data.layers.size();
    if(order.size()!=count)throw std::invalid_argument("排序必须包含全部对象。");
    PreparedOrder result;result.inverse.assign(count,count);result.floating=floating;
    for(std::size_t i=0;i<count;++i){
        if(order[i]>=count||result.inverse[order[i]]!=count)throw std::invalid_argument("排序包含无效或重复对象。");
        result.inverse[order[i]]=i;
    }
    // Reserve all live containers and remapped references before moving any
    // objects. Payloads and properties are not history snapshots.
    if(layer){result.planes.resize(count);for(std::size_t i=0;i<count;++i)prepare_plane_transfer(result.planes[i],data.layers[*layer].planes[order[i]]);}
    else result.layers.resize(count);
    const auto current=layer?data.layers[*layer].current_plane:data.current_layer;
    result.current=current>=0&&std::size_t(current)<count?int(result.inverse[current]):current;
    if(floating&&floating->target){validate_targeted_floating(data,*floating);auto next=std::make_shared<FloatingSelection>(*floating);
        if(!layer)next->target->layer=result.inverse[next->target->layer];else if(*layer==next->target->layer)next->target->plane=result.inverse[next->target->plane];result.floating=std::move(next);}
    else if(floating&&!floating->sources&&!floating->same_document_source)validate_foreign_floating(data,*floating);
    else if(floating){
        validate_local_floating(data,*floating);
        auto sources=std::make_shared<std::vector<FloatingSource>>(*floating->sources);bool changed=false;
        for(auto& source:*sources){
            if(!layer){const auto next=result.inverse[source.layer];changed|=source.layer!=next;source.layer=next;}
            else if(source.layer==*layer)for(auto& plane:source.planes){const auto next=result.inverse[plane];changed|=plane!=next;plane=next;}
        }
        if(changed){auto remapped=std::make_shared<FloatingSelection>(*floating);remapped->sources=std::move(sources);result.floating=std::move(remapped);}
    }
    return result;
}
void apply_order(DocumentData& data,std::optional<std::size_t> layer,const std::vector<std::size_t>& order,PreparedOrder& prepared) noexcept{
    static_assert(std::is_nothrow_move_assignable_v<Layer>);
    if(layer){auto& owner=data.layers[*layer];for(std::size_t i=0;i<order.size();++i)transfer_plane(prepared.planes[i],owner.planes[order[i]]);
        owner.planes.swap(prepared.planes);owner.current_plane=prepared.current;
    }else{for(std::size_t i=0;i<order.size();++i)prepared.layers[i]=std::move(data.layers[order[i]]);
        data.layers.swap(prepared.layers);data.current_layer=prepared.current;}
}
// Pixel snapshots own raster contents, not later element-property changes.
// Prepare matching vector payload colors before publishing either history stack.
// Reapply only differing common properties, preserving the snapshot's geometry
// and independent per-line edits when the common property did not change.
using PreservedVectorColors=std::vector<std::pair<std::size_t,Bytes>>;
PreservedVectorColors prepare_vector_properties(const DocumentData& from,const DocumentData& to){
    PreservedVectorColors result;
    for(std::size_t i=0;i<to.layers.size();++i){
        std::optional<Layer> candidate;
        for(std::size_t j=0;j<to.layers[i].planes.size();++j){
            const auto& target=to.layers[i].planes[j];const auto& source=from.layers[i].planes[j];
            if(propagates_plane_line_color(target.family)&&target.common.color!=source.common.color){
                if(!candidate)candidate=to.layers[i];
                set_vector_plane_line_color(*candidate,j,source.common.color);
            }
        }
        if(candidate)result.emplace_back(i,std::move(candidate->specific));
    }
    return result;
}
void publish_vector_properties(DocumentData& data,PreservedVectorColors& prepared) noexcept{
    for(auto& [layer,bytes]:prepared)data.layers[layer].specific.swap(bytes);
}
// The opposite snapshot's old name is never replayed as a property; move the
// live name without allocating after history publication.
void preserve_frame_properties(DocumentData& from,DocumentData& to) noexcept{
    if(from.layers.size()!=to.layers.size())return;
    for(std::size_t i=0;i<to.layers.size();++i){if(from.layers[i].family!=to.layers[i].family||from.layers[i].planes.size()!=to.layers[i].planes.size())return;
        for(std::size_t j=0;j<to.layers[i].planes.size();++j)if(from.layers[i].planes[j].family!=to.layers[i].planes[j].family)return;}
    for(std::size_t i=0;i<to.layers.size();++i)for(std::size_t j=0;j<to.layers[i].planes.size();++j){
        auto& dst=to.layers[i].planes[j];auto& src=from.layers[i].planes[j];
        if(dst.family==0x40001008||dst.family==0x40002008||dst.family==0x40000080){dst.specific.swap(src.specific);dst.frame_modified=src.frame_modified;dst.vanishing_modified=src.vanishing_modified;
            if(dst.family==0x40001008){to.base.lengths[2]=from.base.lengths[2];to.base.lengths[3]=from.base.lengths[3];}}
    }
}
void preserve_layer_properties(DocumentData& from,DocumentData& to,bool frame=true) noexcept{
    for(std::size_t i=0;i<to.layers.size();++i){
        auto& target=to.layers[i].common;auto& source=from.layers[i].common;
        target.name.swap(source.name);target.opacity=source.opacity;target.opacity_mode=source.opacity_mode;
        for(std::size_t j=0;j<to.layers[i].planes.size();++j){auto& dst=to.layers[i].planes[j].common;auto& src=from.layers[i].planes[j].common;dst.name.swap(src.name);dst.color=src.color;}
    }
    if(frame)preserve_frame_properties(from,to);
}
void preserve_selection(const DocumentData& from,DocumentData& to) noexcept{
    to.current_layer=from.current_layer;
    for(std::size_t i=0;i<to.layers.size();++i){
        auto& target=to.layers[i];const auto& source=from.layers[i];
        target.visibility_modified=source.visibility_modified;
        target.common.flags[1]=source.common.flags[1];target.common.flags[2]=source.common.flags[2];target.current_plane=source.current_plane;
        for(std::size_t p=0;p<target.planes.size();++p){
            target.planes[p].visibility_modified=source.planes[p].visibility_modified;
            target.planes[p].raster_modified=source.planes[p].raster_modified;target.planes[p].frame_modified=source.planes[p].frame_modified;target.planes[p].vanishing_modified=source.planes[p].vanishing_modified;
            target.planes[p].common.flags[1]=source.planes[p].common.flags[1];
            target.planes[p].common.flags[2]=source.planes[p].common.flags[2];
        }
    }
}
}
namespace {
bool same_raw_double(double a,double b) noexcept{return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);}
template<std::size_t N> bool same_raw_lengths(const std::array<Length,N>& a,const std::array<Length,N>& b) noexcept{
    for(std::size_t i=0;i<N;++i)if(a[i].unit!=b[i].unit||!same_raw_double(a[i].value,b[i].value)||!same_raw_double(a[i].reference,b[i].reference))return false;
    return true;
}
bool frame_content_changed(const DocumentData& before,const DocumentData& after) noexcept{
    if(before.layers.size()!=after.layers.size())return true;
    bool origin=false;
    for(std::size_t i=0;i<after.layers.size();++i){const auto& x=before.layers[i];const auto& y=after.layers[i];
        if(x.family!=y.family||x.planes.size()!=y.planes.size())return true;
        for(std::size_t j=0;j<y.planes.size();++j){const auto& p=x.planes[j];const auto& q=y.planes[j];
            if(p.family!=q.family)return true;
            if(q.family==0x40001008||q.family==0x40002008||q.family==0x40000080){if(p.specific!=q.specific)return true;origin|=q.family==0x40001008;}}
    }
    if(origin)for(std::size_t k=2;k<4;++k){const auto& a=before.base.lengths[k];const auto& b=after.base.lengths[k];
        if(a.unit!=b.unit||!same_raw_double(a.value,b.value)||!same_raw_double(a.reference,b.reference))return true;}
    return false;
}
bool same_raw_common(const CommonElement& a,const CommonElement& b) noexcept{
    auto af=a.flags,bf=b.flags;af[2]=bf[2]=0; // selection is session state
    return af==bf&&same_raw_double(a.resolution,b.resolution)&&same_raw_lengths(a.position,b.position)
        &&std::tie(a.width,a.height,a.depth,a.name,a.color,a.x,a.y,a.opacity,a.opacity_mode,a.field_14,a.suffix)
        ==std::tie(b.width,b.height,b.depth,b.name,b.color,b.x,b.y,b.opacity,b.opacity_mode,b.field_14,b.suffix);
}
bool same_raw_raster(const Raster& a,const Raster& b) noexcept{
    if(a.width!=b.width||a.height!=b.height||a.depth!=b.depth||a.color_order!=b.color_order
        ||!same_raw_double(a.resolution,b.resolution)||a.tiles.size()!=b.tiles.size())return false;
    auto x=a.tiles.begin(),y=b.tiles.begin();
    for(;x!=a.tiles.end();++x,++y){if(x->first!=y->first)return false;
        if(x->second!=y->second&&(!x->second||!y->second||*x->second!=*y->second))return false;}
    return true;
}
bool same_raw_content(const DocumentData& a,const DocumentData& b) noexcept{
    if(a.version!=b.version||a.dga!=b.dga||a.preview!=b.preview||a.layer_suffix!=b.layer_suffix||a.trailing!=b.trailing
        ||a.unrestored_external_content!=b.unrestored_external_content||a.layers.size()!=b.layers.size())return false;
    const auto& x=a.base;const auto& y=b.base;
    if(x.frame_mode!=y.frame_mode||x.flags!=y.flags||x.suffix!=y.suffix||!same_raw_lengths(x.lengths,y.lengths)
        ||!same_raw_double(x.scale,y.scale)||!same_raw_double(x.dpi,y.dpi)||!same_raw_double(x.dialog_dpi,y.dialog_dpi))return false;
    for(std::size_t i=0;i<a.layers.size();++i){const auto& l=a.layers[i];const auto& r=b.layers[i];
        if(l.opaque!=r.opaque||l.family!=r.family||!same_raw_common(l.common,r.common)||l.specific!=r.specific||!native_vector_detail::same_allocator_output(l,r)
            ||l.suffix!=r.suffix||l.plane_suffix!=r.plane_suffix||l.opaque_record!=r.opaque_record||l.planes.size()!=r.planes.size())return false;
        for(std::size_t j=0;j<l.planes.size();++j){const auto& p=l.planes[j];const auto& q=r.planes[j];
            if(p.opaque!=q.opaque||p.family!=q.family||!same_raw_common(p.common,q.common)||p.specific!=q.specific||p.suffix!=q.suffix
                ||p.opaque_record!=q.opaque_record||bool(p.raster)!=bool(q.raster)||(p.raster&&!same_raw_raster(*p.raster,*q.raster)))return false;}
    }
    return true;
}
}
bool Document::can_save_exact() const noexcept{
    if(has_text_preview()||has_floating_transform()||has_frame_move()||has_vanishing_point_move())return false;
    return !has_floating_selection()&&data.original&&raw_baseline_&&data.original==raw_baseline_->original&&same_raw_content(data,*raw_baseline_);
}
bool Document::modified() const noexcept{
    if(revision_!=saved_||runtime_dirty_||document_modify_count_||has_floating_selection())return true;
    for(const auto& layer:data.layers){if(layer.visibility_modified)return true;
        for(const auto& plane:layer.planes)if(plane.visibility_modified||plane.frame_modified||plane.vanishing_modified||plane.raster_modified)return true;}
    return false;
}
std::optional<std::uint32_t> archived_adjust_filter_kind(const Plane& plane) noexcept{
    if(plane.opaque||plane.family!=0x40000010u||plane.specific.size()<4)return {};
    Reader reader(plane.specific);const auto kind=reader.u32();
    if(kind){if(reader.remaining()<4)return {};const auto count=reader.u32();if(count>reader.remaining())return {};}
    return kind;
}
PlaneProperties Document::plane_properties(std::size_t layer,std::size_t plane) const{
    const auto& common=data.layers.at(layer).planes.at(plane).common;return {common.name,common.color};
}
bool Document::can_edit_current_plane_properties() const noexcept{
    if(has_text_preview()||has_floating_transform()||has_frame_move()||has_vanishing_point_move())return false;
    const auto* plane=data.active_plane();
    if(!plane||data.layers[data.current_layer].opaque||plane->opaque)return false;
    if(propagates_plane_line_color(plane->family)){
        try{const auto& owner=data.layers[data.current_layer];
            (void)validated_vector_plane_line_references(owner,std::size_t(owner.current_plane));
        }catch(...){return false;}
    }
    return true;
}
bool Document::set_plane_properties(std::size_t layer,std::size_t plane,PlaneProperties properties,const std::string& label){require_no_floating_transform();
    auto& node=data.layers.at(layer).planes.at(plane);auto& common=node.common;
    const bool name=common.name!=properties.name,color=common.color!=properties.color;
    if(!name&&!color)return false;
    if(data.layers[layer].opaque||node.opaque)
        throw std::invalid_argument("此平面的属性载荷尚未支持编辑。");
    if(color&&(!common.flags[4]||node.family==0x10000001u))throw std::invalid_argument("此平面不支持固定颜色属性。");
    std::optional<Layer> candidate;
    if(propagates_plane_line_color(node.family)){
        if(color){candidate=data.layers[layer];set_vector_plane_line_color(*candidate,plane,properties.color);}
        else (void)validated_vector_plane_line_references(data.layers[layer],plane);
    }
    PlanePropertyEdit edit;edit.layer=layer;edit.plane=plane;edit.color=color;
    if(color)edit.saved=common.color;else edit.saved=common.name;
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(edit)});
    if(candidate)data.layers[layer].specific.swap(candidate->specific);
    common.name.swap(properties.name);common.color=properties.color;
    trim_history();redo_.clear();undo_next_=true;
    document_modify_count_+=color?2:1;++state_generation_;return true;
}
LayerProperties Document::layer_properties(std::size_t layer) const{
    const auto& common=data.layers.at(layer).common;return {common.name,common.opacity,common.opacity_mode};
}
bool Document::set_layer_properties(std::size_t layer,LayerProperties properties,const std::string& label){require_no_floating_transform();
    auto& common=data.layers.at(layer).common;
    const bool name=common.name!=properties.name,opacity=common.opacity!=properties.opacity,mode=common.opacity_mode!=properties.opacity_mode;
    if(!name&&!opacity&&!mode)return false;
    if((opacity&&(properties.opacity<0||properties.opacity>100))||(mode&&properties.opacity_mode!=1&&properties.opacity_mode!=2))
        throw std::invalid_argument("图层不透明度参数无效。");
    LayerPropertyEdit edit;edit.layer=layer;
    if(mode){edit.field=LayerPropertyEdit::Field::OpacityMode;edit.saved=common.opacity_mode;}
    else if(opacity){edit.field=LayerPropertyEdit::Field::Opacity;edit.saved=common.opacity;}
    else {edit.field=LayerPropertyEdit::Field::Name;edit.saved=common.name;}
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(edit)});
    common.name.swap(properties.name);common.opacity=properties.opacity;common.opacity_mode=properties.opacity_mode;
    trim_history();redo_.clear();undo_next_=true;++document_modify_count_;++state_generation_;return true;
}
void Document::commit(DocumentData&& before,const std::string& label,HistorySelection selection){require_no_floating_transform();
    if(has_floating_selection())throw std::logic_error("实时内容操作必须在修改像素前确定浮动图像。");
    static_assert(std::is_nothrow_swappable_v<DocumentData>);
    static_assert(std::is_nothrow_move_constructible_v<Revision> && std::is_nothrow_move_assignable_v<Revision>);
    // Allocate the label and vector slot before consuming the rollback data.
    require_selection_layout(before,data,selection);
    raster_history_detail::Budget budget;auto manager=prepare_paint_manager(paint_history_manager_,data,true,budget);
    undo_.push_back({DocumentData{},label,revision_,selection,{},frame_content_changed(before,data)});
    std::swap(undo_.back().data,before);paint_history_manager_=std::move(manager);
    trim_history();
    redo_.clear();undo_next_=true;revision_=next_++;++state_generation_;
}
void Document::commit(const DocumentData& before,const std::string& label,HistorySelection selection){require_no_floating_transform();
    auto snapshot=before;commit(std::move(snapshot),label,selection);
}
void Document::commit_layer_move(DocumentData&& before,std::shared_ptr<const AttachedTemporaryLayers> attached_before,const std::string& label,bool restore_attached){
    bool refresh{};for(std::size_t i=0;i<std::min(before.layers.size(),data.layers.size());++i){const auto& old=before.layers[i];const auto& now=data.layers[i];if(((old.family&0x20000000u)||old.family==0x40002000u||old.family==0x40004000u)&&(old.common.x!=now.common.x||old.common.y!=now.common.y||old.common.width!=now.common.width||old.common.height!=now.common.height||old.specific!=now.specific))refresh=true;}
    commit(std::move(before),label,HistorySelection::PreserveCurrent);
    undo_.back().runtime=AttachedLayerMoveEdit{std::move(attached_before),refresh,restore_attached};
}
void Document::publish_prepared_without_history(DocumentData&& prepared,std::uint64_t expected_state_generation,
                                                std::stop_token stop){
    require_no_floating_transform();
    if(has_floating_selection())throw std::logic_error("Prepared storage publication requires settled artwork");
    if(&prepared==&data)throw std::invalid_argument("Prepared storage must be private");
    if(state_generation_!=expected_state_generation)throw std::logic_error("Prepared storage source generation changed");
    require_selection_layout(data,prepared,HistorySelection::PreserveCurrent);
    if(stop.stop_requested())throw std::runtime_error("Prepared storage publication cancelled");
    static_assert(std::is_nothrow_move_assignable_v<DocumentData>);
    const auto manager_before=paint_history_manager_;const auto current_owner=data.current_layer;
    preserve_selection(data,prepared);
    raster_history_detail::Budget local_budget;auto& budget=selection_command_budget_?*selection_command_budget_:local_budget;budget.stop=stop;auto manager=prepare_paint_manager(paint_history_manager_,prepared,true,budget);
    if(state_generation_!=expected_state_generation||paint_history_manager_!=manager_before||data.current_layer!=current_owner||stop.stop_requested())throw std::logic_error("Prepared storage source changed during history baseline preparation");
    // No allocation or user callback follows this final adoption. Same-owner
    // selection/visibility changes do not advance the content generation.
    preserve_selection(data,prepared);
    prepared.original=data.original;prepared.original_content=data.original_content;
    data=std::move(prepared);paint_history_manager_=std::move(manager);++state_generation_;
}
void Document::publish_vector_tool_content(DocumentData prepared,const std::string& label,std::uint64_t expected,const std::function<bool()>& current,std::optional<std::array<std::int32_t,4>> raw,std::stop_token stop,bool native_paint_history){
    if(state_generation_!=expected||!current())throw std::logic_error("Vector tool target changed before history preparation");
    prepare_selection_command(label,stop,[&](Document& staged,std::uint64_t&,std::uint64_t&){
        if(!current())throw std::logic_error("Vector tool target changed during history preparation");
        const auto index=std::size_t(staged.data.current_layer);const auto& owner=staged.data.layers.at(index);const auto* plane=staged.data.active_plane();
        if(owner.family==0x40002000u||owner.family==0x40004000u){
            raster_history_detail::Budget local_budget;local_budget.stop=stop;auto& budget=staged.selection_command_budget_?*staged.selection_command_budget_:local_budget;auto edit=std::make_shared<CaptureSourceEdit>();edit->owner=owner.common.runtime_identity;edit->family=owner.family;edit->hint=index;edit->layout=raster_history_detail::Edit::identity_layout(staged.data,budget);edit->storage=capture_source_storage(owner,budget);edit->whole_owner=true;
            for(std::size_t p=0;p<owner.planes.size();++p)edit->planes.push_back(p);
            staged.publish_prepared_without_history(std::move(prepared),staged.state_generation_,stop);staged.undo_.push_back({DocumentData{},label,staged.revision_,HistorySelection::PreserveCurrent,std::shared_ptr<const CaptureSourceEdit>(std::move(edit))});staged.redo_.clear();staged.undo_next_=true;staged.runtime_dirty_=true;
        }else{BatchContentTarget target;target.layer=index;target.vector_content=true;target.vector_damage=raw;target.vector_paint_history=native_paint_history;
            target.vector_draw_history=plane&&(plane->family==0x21210001u||plane->family==0x21220001u)&&(owner.family==0x22100000u||owner.family==0x24400000u||owner.family==0x21200000u);
            if(target.vector_draw_history)target.vector_plane=std::size_t(owner.current_plane);
            staged.publish_batch_content(std::move(prepared),std::span{&target,1},true,label,stop);
        }
        if(!current())throw std::logic_error("Vector tool target changed before history publication");
    });
}
void Document::publish_vector_selection_content(DocumentData prepared,bool history,bool storage_changed,const BatchContentTarget& target,const std::string& label,std::uint64_t expected,std::stop_token stop,const std::function<bool()>& current){
    if(state_generation_!=expected||(current&&!current()))throw std::logic_error("Vector command source changed before preparation");
    const auto live_selection=region_selection;
    prepare_selection_command(label,stop,[&](Document& staged,std::uint64_t& bytes,std::uint64_t& work){
        if(current&&!current())throw std::logic_error("Vector command source changed during history preparation");
        if(history)staged.publish_batch_content(std::move(prepared),std::span{&target,1},true,label,stop);
        else if(storage_changed)staged.publish_prepared_without_history(std::move(prepared),staged.state_generation_,stop);
        const auto flags=staged.application_vector_selection_flags.value_or(staged.region_selection.vector_flags);const bool mask=staged.region_selection.state==SelectionLayerState::Mask||staged.region_selection.state==SelectionLayerState::VectorIndicated;
        staged.vector_indication_active=vector_indication_detail::refresh_marks(staged.data,staged.vector_indication_active,staged.region_selection,flags,mask&&!(flags&1u)?selection_carrier_dpi(staged.region_selection,staged.data.base.dpi):0,bytes,work,stop);
        staged.region_selection=live_selection;
        if(current&&!current())throw std::logic_error("Vector command source changed before history publication");
    });
}
void Document::publish_batch_content(DocumentData&& prepared,std::span<const BatchContentTarget> targets,
    bool history,const std::string& label,std::stop_token stop){
    require_no_floating_transform();if(has_floating_selection())throw std::logic_error("Batch requires settled artwork");
    require_selection_layout(data,prepared,HistorySelection::PreserveCurrent);
    const auto generation=state_generation_;const SelectionIndicationWitness witness(*this);
    raster_history_detail::Budget local_budget;local_budget.stop=stop;auto& budget=selection_command_budget_?*selection_command_budget_:local_budget;modal_detail::data_charge(data,budget.bytes);modal_detail::data_charge(prepared,budget.bytes);
    const auto before=modal_detail::frozen(data,stop);auto candidate=std::move(prepared);
    const auto layout=raster_history_detail::Edit::identity_layout(before,budget);
    budget.allocate(sizeof(CompoundEdit)+targets.size()*3*sizeof(Revision));auto group=std::make_shared<CompoundEdit>();
    auto manager=paint_history_manager_;
    for(const auto& target:targets){budget.poll();const auto& owner=before.layers.at(target.layer);
        if(target.native_owner_entry){manager=ensure_paint_manager(manager,before,target.layer,false,budget,layout,target.vector_draw_history,true);continue;}
        if(history&&!target.raster_planes.empty()){
            budget.allocate(sizeof(raster_history_detail::Edit)+target.raster_planes.size()*sizeof(raster_history_detail::Edit::PlaneState));
            auto edit=std::make_shared<raster_history_detail::Edit>();edit->layout=layout;edit->native_raster_dirty=true;
            if(!target.raster_regions.empty()&&target.raster_regions.size()!=target.raster_planes.size())throw std::invalid_argument("Batch raster history ROI count mismatch");
            for(std::size_t i=0;i<target.raster_planes.size();++i){const auto p=target.raster_planes[i];const auto& plane=owner.planes.at(p);if(!plane.raster||plane.opaque)throw std::invalid_argument("Batch raster history requires decoded storage");
                const Rect roi=target.raster_regions.empty()?Rect{0,0,plane.raster->width,plane.raster->height}:target.raster_regions[i];if(target.skip_empty_raster_regions&&(roi.width<=0||roi.height<=0))continue;edit->planes.push_back({raster_history_detail::target(before,target.layer,p),roi,raster_history_detail::snapshot(*plane.raster,roi,budget),{}});}
            group->past.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::shared_ptr<const raster_history_detail::Edit>(std::move(edit))});
        }
        if(history&&target.vector_content&&!target.vector_storage_only){
            budget.allocate(sizeof(CaptureSourceEdit));auto edit=std::make_shared<CaptureSourceEdit>();edit->owner=owner.common.runtime_identity;edit->family=owner.family;edit->hint=target.layer;edit->layout=layout;
            if(owner.family==0x21200000u||owner.family==0x22100000u||owner.family==0x24400000u||owner.family==0x40002000u||target.vector_draw_history||target.vector_paint_history){
                manager=ensure_paint_manager(manager,before,target.layer,false,budget,layout,target.vector_draw_history,target.vector_paint_history);
                std::optional<VectorBounds> raw;if(target.vector_damage){const auto& v=*target.vector_damage;raw=VectorBounds{v[0],v[1],v[2],v[3]};}
                auto bound=with_paint_budget(budget,[&](auto& b){return (owner.family==0x21200000u||owner.family==0x40002000u||target.vector_paint_history)&&!target.vector_draw_history?manager->manager->bind(candidate.layers[target.layer],raw,b):manager->manager->bind_draw(owner,candidate.layers[target.layer],target.vector_plane,raw,b,target.vector_draw_planes);});
                edit->paint=bound.edit;candidate.layers[target.layer]=std::move(bound.source);manager=replace_paint_manager(manager,std::move(bound.manager),budget);
            }else{
                edit->whole_owner=true;edit->storage=capture_source_storage(owner,budget);
                for(std::size_t p=0;p<owner.planes.size();++p)if(!owner.planes[p].raster)edit->planes.push_back(p);
            }
            group->past.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::shared_ptr<const CaptureSourceEdit>(std::move(edit))});
        }
        if(target.vector_content&&!target.vector_storage_only&&!target.vector_paint_history&&!target.vector_draw_history)invalidate_select_paint_manager(manager,owner);
    }
    manager=prepare_paint_manager(manager,candidate,false,budget,std::ranges::any_of(targets,[](const auto& t){return t.native_owner_entry;}));
    std::optional<Revision> item;std::vector<Revision> undo;
    if(history&&!group->past.empty()){budget.allocate((undo_.size()+1)*sizeof(Revision)+label.size());item.emplace(Revision{DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::shared_ptr<const CompoundEdit>(std::move(group))});undo.reserve(undo_.size()+1);}
    budget.poll();if(state_generation_!=generation||!witness.matches(*this)||!modal_detail::data_same(data,before)||!raster_history_detail::same_identity_layout(data,before))throw std::logic_error("Batch content source changed before publication");
    preserve_selection(data,candidate);candidate.original=data.original;candidate.original_content=data.original_content;
    if(item){for(auto& old:undo_)undo.push_back(std::move(old));undo.push_back(std::move(*item));undo_.swap(undo);trim_history();redo_.clear();undo_next_=true;}
    data=std::move(candidate);paint_history_manager_=std::move(manager);runtime_dirty_=true;++state_generation_;
}
FillResult Document::apply_dust_tool(const FilterToolSettings& request,bool metric,const std::optional<Rect>& bounds,
    const BinaryMask* mask,std::stop_token stop,const std::optional<RasterSelection>& selection,const std::string& label){
    try{
        const auto cancelled=[&]{FillResult r;r.cancelled=true;return r;};
        if(stop.stop_requested())return cancelled();
        require_no_floating_transform();
        if(has_floating_selection())throw std::logic_error("Dust requires settled artwork");
        if(!can_apply_filter_tool(data,FilterToolKind::Dust))throw std::invalid_argument("Dust requires a marked canonical raster target");
        const auto settings=request;const SelectionIndicationWitness witness(*this);
        const auto revision=revision_,saved=saved_,next=next_;const auto undo_size=undo_.size(),redo_size=redo_.size();const bool direction=undo_next_;
        const auto undo_id=undo_.empty()?0:undo_.back().id,redo_id=redo_.empty()?0:redo_.back().id;
        const auto manager_before=paint_history_manager_;const auto baseline=raw_baseline_;
        const auto original=data.original;const auto content=data.original_content;
        const auto grid_generation=grid_generation_,guide_generation=guide_generation_,attached_generation=attached_temporary_generation_;
        const auto attached=attached_temporary_layers_;const auto dirty=runtime_dirty_;const auto modify_count=document_modify_count_;
        const auto unchanged=[&]{return witness.matches(*this)&&revision_==revision&&saved_==saved&&next_==next&&undo_next_==direction
            &&undo_.size()==undo_size&&redo_.size()==redo_size&&(undo_.empty()||undo_.back().id==undo_id)&&(redo_.empty()||redo_.back().id==redo_id)
            &&paint_history_manager_==manager_before&&raw_baseline_==baseline&&data.original==original&&data.original_content==content
            &&grid_generation_==grid_generation&&guide_generation_==guide_generation&&attached_temporary_generation_==attached_generation&&attached_temporary_layers_==attached
            &&runtime_dirty_==dirty&&document_modify_count_==modify_count;};
        raster_history_detail::Budget budget;budget.stop=stop;modal_detail::data_charge(data,budget.bytes);
        const auto frozen=modal_detail::frozen(data,stop);modal_detail::data_charge(frozen,budget.bytes,0,false);
        auto candidate=frozen;candidate.original=original;candidate.original_content=content;
        auto frozen_selection=witness.selection;
        if(frozen_selection.mask){const auto& bits=*frozen_selection.mask;budget.visit(2*(std::uint64_t(bits.width())*bits.height()+((std::uint64_t(bits.width())+255)/256)*((std::uint64_t(bits.height())+255)/256)));budget.allocate(((std::uint64_t(bits.width())+31)/32)*bits.height()*4+((std::uint64_t(bits.width())+255)/256)*((std::uint64_t(bits.height())+255)/256));frozen_selection.mask=std::make_shared<const BinaryMask>(bits);}
        const auto current=[&]{return unchanged()&&modal_detail::data_same(data,frozen)&&raster_history_detail::same_identity_layout(data,frozen)&&modal_detail::selection_same(region_selection,frozen_selection,stop);};
        if(!current())throw std::logic_error("Dust source changed during candidate preparation");
        // Retain live COW identity while frozen remains the deep comparison
        // witness. Publication below only replaces the consumed raster slots.
        for(std::size_t l=0;l<candidate.layers.size();++l)for(std::size_t p=0;p<candidate.layers[l].planes.size();++p)if(auto& image=candidate.layers[l].planes[p].raster){
            auto live=data.layers[l].planes[p].raster->tiles.begin();for(auto& [key,bytes]:image->tiles){(void)key;bytes=live->second;++live;}}
        filter_detail::FilterCosts costs{budget.bytes,budget.work};std::vector<filter_detail::DustBinding> bound;
        auto result=filter_detail::dust_tool_data(candidate,settings,metric,bounds,mask,stop,selection,&bound,&costs);
        if(!result.handled||result.cancelled)return result;
        budget.bytes=costs.storage;budget.work=costs.work;
        budget.allocate(sizeof(raster_history_detail::Edit)+bound.size()*(sizeof(raster_history_detail::Edit::PlaneState)+sizeof(filter_detail::DustBinding)));
        auto edit=std::make_shared<raster_history_detail::Edit>();edit->native_raster_dirty=true;edit->layout=raster_history_detail::Edit::identity_layout(frozen,budget);edit->planes.reserve(bound.size());
        for(const auto& binding:bound){const auto& source=*frozen.layers[binding.layer].planes[binding.plane].raster;const auto roi=raster_history_detail::clipped(binding.roi,source);
            edit->planes.push_back({raster_history_detail::target(frozen,binding.layer,binding.plane),roi,raster_history_detail::snapshot(source,roi,budget),{}});}
        auto manager=prepare_paint_manager(manager_before,candidate,true,budget);
        std::optional<Revision> item;std::vector<Revision> prepared_history;
        if(result.performed){
            item.emplace(Revision{DocumentData{},label,revision,HistorySelection::PreserveCurrent,std::shared_ptr<const raster_history_detail::Edit>(std::move(edit))});
            budget.allocate((undo_size+1)*sizeof(Revision)+label.size());prepared_history.reserve(undo_size+1);
        }
        budget.poll();if(!current())throw std::logic_error("Dust source changed before publication");require_no_floating_transform();budget.poll();
        static_assert(std::is_nothrow_move_assignable_v<Raster>);
        static_assert(std::is_nothrow_move_constructible_v<Revision>&&std::is_nothrow_move_assignable_v<Revision>);
        // No allocation or callback follows the final witness check.
        for(const auto& binding:bound){auto& live=data.layers[binding.layer].planes[binding.plane];auto& prepared=candidate.layers[binding.layer].planes[binding.plane];*live.raster=std::move(*prepared.raster);live.pixels_changed=true;live.raster_modified=true;}
        paint_history_manager_=std::move(manager);
        if(item){for(auto& previous:undo_)prepared_history.push_back(std::move(previous));prepared_history.push_back(std::move(*item));undo_.swap(prepared_history);
            trim_history();redo_.clear();undo_next_=true;revision_=next_++;}
        ++state_generation_;return result;
    }catch(const std::exception& error){FillResult r;if(stop.stop_requested())r.cancelled=true;else{r.handled=false;r.error=error.what();}return r;}
}
DustFilterResult Document::apply_dust_filter(const DustSettings& request,std::stop_token stop,const std::string& label){
    try{
        const auto cancelled=[&]{DustFilterResult r;r.status=DustFilterStatus::Cancelled;return r;};
        if(stop.stop_requested())return cancelled();
        require_no_floating_transform();
        if(has_floating_selection())throw std::logic_error("Dust requires settled artwork");
        if(!can_apply_dust_filter(data))throw std::invalid_argument("Ordinary Dust has no admitted current owner");
        const auto settings=request;const SelectionIndicationWitness witness(*this);
        const auto revision=revision_,saved=saved_,next=next_;const auto undo_size=undo_.size(),redo_size=redo_.size();const bool direction=undo_next_;
        const auto undo_id=undo_.empty()?0:undo_.back().id,redo_id=redo_.empty()?0:redo_.back().id;
        const auto manager_before=paint_history_manager_;const auto baseline=raw_baseline_;
        const auto original=data.original;const auto content=data.original_content;
        const auto grid_generation=grid_generation_,guide_generation=guide_generation_,attached_generation=attached_temporary_generation_;
        const auto attached=attached_temporary_layers_;const auto dirty=runtime_dirty_;const auto modify_count=document_modify_count_;
        const auto unchanged=[&]{return witness.matches(*this)&&revision_==revision&&saved_==saved&&next_==next&&undo_next_==direction
            &&undo_.size()==undo_size&&redo_.size()==redo_size&&(undo_.empty()||undo_.back().id==undo_id)&&(redo_.empty()||redo_.back().id==redo_id)
            &&paint_history_manager_==manager_before&&raw_baseline_==baseline&&data.original==original&&data.original_content==content
            &&grid_generation_==grid_generation&&guide_generation_==guide_generation&&attached_temporary_generation_==attached_generation&&attached_temporary_layers_==attached
            &&runtime_dirty_==dirty&&document_modify_count_==modify_count;};
        raster_history_detail::Budget budget;budget.stop=stop;modal_detail::data_charge(data,budget.bytes);
        const auto frozen=modal_detail::frozen(data,stop);modal_detail::data_charge(frozen,budget.bytes,0,false);
        auto candidate=frozen;candidate.original=original;candidate.original_content=content;
        auto frozen_selection=witness.selection;
        if(frozen_selection.mask){const auto& bits=*frozen_selection.mask;budget.visit(2*(std::uint64_t(bits.width())*bits.height()+((std::uint64_t(bits.width())+255)/256)*((std::uint64_t(bits.height())+255)/256)));budget.allocate(((std::uint64_t(bits.width())+31)/32)*bits.height()*4+((std::uint64_t(bits.width())+255)/256)*((std::uint64_t(bits.height())+255)/256));frozen_selection.mask=std::make_shared<const BinaryMask>(bits);}
        const auto current=[&]{return unchanged()&&modal_detail::data_same(data,frozen)&&raster_history_detail::same_identity_layout(data,frozen)&&modal_detail::selection_same(region_selection,frozen_selection,stop);};
        if(!current())throw std::logic_error("Dust source changed during candidate preparation");
        // Retain live COW identity while frozen remains the deep comparison
        // witness. Publication below only replaces the consumed raster slots.
        for(std::size_t l=0;l<candidate.layers.size();++l)for(std::size_t p=0;p<candidate.layers[l].planes.size();++p)if(auto& image=candidate.layers[l].planes[p].raster){
            auto live=data.layers[l].planes[p].raster->tiles.begin();for(auto& [key,bytes]:image->tiles){(void)key;bytes=live->second;++live;}}
        filter_detail::FilterCosts costs{budget.bytes,budget.work};filter_detail::DustFilterTargets targets;
        auto result=filter_detail::dust_filter_data(candidate,settings,frozen_selection,stop,targets,costs);
        if(result.rejected()||result.cancelled())return result;
        const auto& bound=targets.bound;
        budget.bytes=costs.storage;budget.work=costs.work;
        budget.allocate(sizeof(raster_history_detail::Edit)+bound.size()*(sizeof(raster_history_detail::Edit::PlaneState)+sizeof(filter_detail::DustBinding)));
        auto edit=std::make_shared<raster_history_detail::Edit>();edit->native_raster_dirty=true;edit->layout=raster_history_detail::Edit::identity_layout(frozen,budget);edit->planes.reserve(bound.size());
        for(const auto& binding:bound){const auto& source=*frozen.layers[binding.layer].planes[binding.plane].raster;const auto roi=raster_history_detail::clipped(binding.roi,source);
            edit->planes.push_back({raster_history_detail::target(frozen,binding.layer,binding.plane),roi,raster_history_detail::snapshot(source,roi,budget),{}});}
        auto manager=result.applied()?prepare_paint_manager(manager_before,candidate,true,budget):manager_before;
        std::optional<Revision> item;std::vector<Revision> prepared_history;
        if(result.applied()){
            budget.allocate((undo_size+1)*sizeof(Revision)+label.size());
            item.emplace(Revision{DocumentData{},label,revision,HistorySelection::PreserveCurrent,std::shared_ptr<const raster_history_detail::Edit>(std::move(edit))});
            prepared_history.reserve(undo_size+1);
        }
        budget.poll();if(!current())throw std::logic_error("Dust source changed before publication");require_no_floating_transform();budget.poll();
        static_assert(std::is_nothrow_move_assignable_v<Raster>);
        static_assert(std::is_nothrow_move_constructible_v<Revision>&&std::is_nothrow_move_assignable_v<Revision>);
        // No allocation or callback follows the final witness check.
        for(const auto index:targets.published){auto& live=data.layers[targets.layer].planes[index];auto& prepared=candidate.layers[targets.layer].planes[index];*live.raster=std::move(*prepared.raster);live.pixels_changed=prepared.pixels_changed;live.raster_modified=prepared.raster_modified;}
        paint_history_manager_=std::move(manager);
        if(item){for(auto& previous:undo_)prepared_history.push_back(std::move(previous));prepared_history.push_back(std::move(*item));undo_.swap(prepared_history);
            trim_history();redo_.clear();undo_next_=true;}
        ++state_generation_;return result;
    }catch(const std::exception& error){DustFilterResult r;r.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())r.error=error.what();return r;}
}
DustFilterPreview Document::capture_dust_filter_preview(std::stop_token stop) const {
    auto preview=capture_filter_preview(FilterKind::Dust,stop);return DustFilterPreview(std::move(preview.state_));
}
FilterPreview Document::capture_filter_preview(FilterKind kind,std::stop_token stop) const {
    return capture_filter_preview_impl(kind,stop,{});
}
FilterPreview Document::capture_batch_filter_preview(std::size_t owner,FilterKind kind,std::stop_token stop,std::array<std::uint64_t,2> consumed) const {
    return capture_filter_preview_impl(kind,stop,owner,consumed);
}
FilterPreview Document::capture_filter_preview_impl(FilterKind kind,std::stop_token stop,std::optional<std::size_t> batch_owner,std::array<std::uint64_t,2> consumed) const {
    modal_detail::poll(stop);require_no_floating_transform();
    if(consumed[0]>modal_detail::max_bytes||consumed[1]>modal_detail::max_work)throw std::length_error("Filter cumulative budget seed exceeds limits");
    const auto owner=batch_owner.value_or(data.current_layer<0?data.layers.size():std::size_t(data.current_layer));
    if(has_floating_selection()||owner>=data.layers.size()||(!batch_owner&&!can_apply_document_filter(data,kind))||!document_filter_executes_on_layer(kind,data.layers[owner].family))
        throw std::logic_error("Filter preview requires an admitted settled execution owner");
    const SelectionIndicationWitness witness(*this);
    const auto revision=revision_,saved=saved_,next=next_;const auto undo_size=undo_.size(),redo_size=redo_.size();const bool direction=undo_next_;
    const auto undo_id=undo_.empty()?0:undo_.back().id,redo_id=redo_.empty()?0:redo_.back().id;
    const auto manager=paint_history_manager_;const auto baseline=raw_baseline_;
    const auto original=data.original;const auto content=data.original_content;
    const auto grid_generation=grid_generation_,guide_generation=guide_generation_,attached_generation=attached_temporary_generation_;
    const auto attached=attached_temporary_layers_;const auto dirty=runtime_dirty_;const auto modify_count=document_modify_count_;
    raster_history_detail::Budget budget{consumed[0],consumed[1],stop};budget.allocate(sizeof(filter_detail::FilterPreviewState)+1024);modal_detail::data_charge(data,budget.bytes);
    auto state=std::make_shared<filter_detail::FilterPreviewState>();state->owner=this;state->kind=kind;state->batch_owner=batch_owner;
    state->current=[witness,revision,saved,next,undo_size,redo_size,direction,undo_id,redo_id,manager,baseline,original,content,
        grid_generation,guide_generation,attached_generation,attached,dirty,modify_count](const Document& d){
        return witness.matches(d)&&d.revision_==revision&&d.saved_==saved&&d.next_==next&&d.undo_next_==direction
            &&d.undo_.size()==undo_size&&d.redo_.size()==redo_size&&(d.undo_.empty()||d.undo_.back().id==undo_id)&&(d.redo_.empty()||d.redo_.back().id==redo_id)
            &&d.paint_history_manager_==manager&&d.raw_baseline_==baseline&&d.data.original==original&&d.data.original_content==content
            &&d.grid_generation_==grid_generation&&d.guide_generation_==guide_generation&&d.attached_temporary_generation_==attached_generation&&d.attached_temporary_layers_==attached
            &&d.runtime_dirty_==dirty&&d.document_modify_count_==modify_count;
    };
    state->baseline=modal_detail::frozen(data,stop);state->selection=witness.selection;
    if(auto& mask=state->selection.mask){const auto& bits=*mask;budget.visit(3*(std::uint64_t(bits.width())*bits.height()+((std::uint64_t(bits.width())+255)/256)*((std::uint64_t(bits.height())+255)/256)));
        budget.allocate(((std::uint64_t(bits.width())+31)/32)*bits.height()*4+((std::uint64_t(bits.width())+255)/256)*((std::uint64_t(bits.height())+255)/256));mask=std::make_shared<const BinaryMask>(bits);}
    filter_detail::FilterCosts costs{budget.bytes,budget.work};
    // Native6577A0 copies every raster-bit child's baseline, independently of
    // marks/visibility. Validate those sources before handing work to a thread.
    (void)filter_detail::filter_preview_bindings(state->baseline,kind,RegionSelection{},stop,costs,batch_owner);
    if(!state->current(*this)||!modal_detail::data_same(data,state->baseline)||!raster_history_detail::same_identity_layout(data,state->baseline)
        ||!modal_detail::selection_same(region_selection,state->selection,stop))throw std::logic_error("Filter preview source changed during capture");
    require_no_floating_transform();modal_detail::poll(stop);state->initial_costs=costs;return FilterPreview(std::move(state));
}
DustFilterResult Document::commit_dust_filter_preview(DustFilterPreview& preview,std::stop_token stop,const std::string& label){
    auto common=FilterPreview(preview.state_);return commit_filter_preview(common,stop,label);
}
DustFilterResult Document::apply_gaussian_filter(const GaussianBlurSettings& settings,std::stop_token stop,const std::string& label){
    const auto request=settings;
    try{auto preview=capture_filter_preview(FilterKind::GaussianBlur,stop);auto result=preview.run(request,false,stop);
        if(result.rejected()||result.cancelled())return result;return commit_filter_preview(preview,stop,label);
    }catch(const std::exception& error){DustFilterResult result;result.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())result.error=error.what();return result;}
}
DustFilterResult Document::apply_contrast_filter(const ContrastSettings& settings,std::stop_token stop,const std::string& label){
    if(data.current_layer>=0&&std::size_t(data.current_layer)<data.layers.size()&&data.layers[data.current_layer].family==0x40000200){
        if(!can_apply_document_filter(data,FilterKind::Contrast))return {DustFilterStatus::Rejected,0,false,"Adjust Apply query rejected"};
        AdjustParameters p;p.settings=settings;return execute_adjust_filter(p,AdjustFilterExecution::Direct,stop,label);
    }
    const auto request=settings;
    try{auto preview=capture_filter_preview(FilterKind::Contrast,stop);auto result=preview.run(request,false,stop);
        if(result.rejected()||result.cancelled())return result;return commit_filter_preview(preview,stop,label);
    }catch(const std::exception& error){DustFilterResult result;result.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())result.error=error.what();return result;}
}
DustFilterResult Document::apply_level_filter(const LevelSettings& settings,std::stop_token stop,const std::string& label){
    if(data.current_layer>=0&&std::size_t(data.current_layer)<data.layers.size()&&data.layers[data.current_layer].family==0x40000200){
        if(!can_apply_document_filter(data,FilterKind::Level))return {DustFilterStatus::Rejected,0,false,"Adjust Apply query rejected"};
        AdjustParameters p;p.kind=FilterKind::Level;p.settings=settings;return execute_adjust_filter(p,AdjustFilterExecution::Direct,stop,label);
    }
    const auto request=settings;
    try{auto preview=capture_filter_preview(FilterKind::Level,stop);auto result=preview.run(request,false,stop);
        if(result.rejected()||result.cancelled())return result;return commit_filter_preview(preview,stop,label);
    }catch(const std::exception& error){DustFilterResult result;result.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())result.error=error.what();return result;}
}
DustFilterResult Document::apply_tone_curve_filter(const ToneCurveSettings& settings,std::stop_token stop,const std::string& label){
    if(data.current_layer>=0&&std::size_t(data.current_layer)<data.layers.size()&&data.layers[data.current_layer].family==0x40000200){
        if(!can_apply_document_filter(data,FilterKind::ToneCurve))return {DustFilterStatus::Rejected,0,false,"Adjust Apply query rejected"};
        try{AdjustParameters p;p.kind=FilterKind::ToneCurve;p.settings=filter_detail::freeze_tone_curve_settings(settings);return execute_adjust_filter(p,AdjustFilterExecution::Direct,stop,label);}
        catch(const std::exception& error){return {stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected,0,false,stop.stop_requested()?std::string{}:error.what()};}
    }
    try{modal_detail::poll(stop);const auto request=filter_detail::freeze_tone_curve_settings(settings);
        auto preview=capture_filter_preview(FilterKind::ToneCurve,stop);auto result=preview.run(request,false,stop);
        if(result.rejected()||result.cancelled())return result;return commit_filter_preview(preview,stop,label);
    }catch(const std::exception& error){DustFilterResult result;result.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())result.error=error.what();return result;}
}
DustFilterResult Document::apply_auto_contrast_filter(std::stop_token stop,const std::string& label){
    try{auto preview=capture_filter_preview(FilterKind::AutoContrast,stop);auto result=preview.run(std::monostate{},false,stop);
        if(result.rejected()||result.cancelled())return result;return commit_filter_preview(preview,stop,label);
    }catch(const std::exception& error){DustFilterResult result;result.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())result.error=error.what();return result;}
}
DustFilterResult Document::apply_hsv_filter(const HsvSettings& settings,std::stop_token stop,const std::string& label){
    const auto request=settings;
    try{auto preview=capture_filter_preview(FilterKind::Hsv,stop);auto result=preview.run(request,false,stop);
        if(result.rejected()||result.cancelled())return result;return commit_filter_preview(preview,stop,label);
    }catch(const std::exception& error){DustFilterResult result;result.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())result.error=error.what();return result;}
}
DustFilterResult Document::apply_color_balance_filter(const ColorBalanceSettings& settings,std::stop_token stop,const std::string& label){
    const auto request=settings;
    try{auto preview=capture_filter_preview(FilterKind::ColorBalance,stop);auto result=preview.run(request,false,stop);
        if(result.rejected()||result.cancelled())return result;return commit_filter_preview(preview,stop,label);
    }catch(const std::exception& error){DustFilterResult result;result.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())result.error=error.what();return result;}
}
DustFilterResult Document::apply_unsharp_filter(const UnsharpMaskSettings& settings,std::stop_token stop,const std::string& label){
    const auto request=settings;
    try{auto preview=capture_filter_preview(FilterKind::UnsharpMask,stop);auto result=preview.run(request,false,stop);
        if(result.rejected()||result.cancelled())return result;return commit_filter_preview(preview,stop,label);
    }catch(const std::exception& error){DustFilterResult result;result.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())result.error=error.what();return result;}
}
DustFilterResult Document::apply_fixed_convolution_filter(FilterKind kind,std::stop_token stop,const std::string& label){
    try{if(!filter_detail::fixed_convolution(kind))throw std::invalid_argument("Expected a fixed convolution preset");
        auto preview=capture_filter_preview(kind,stop);auto result=preview.run(std::monostate{},false,stop);
        if(result.rejected()||result.cancelled())return result;return commit_filter_preview(preview,stop,label);
    }catch(const std::exception& error){DustFilterResult result;result.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())result.error=error.what();return result;}
}
DustFilterResult Document::commit_filter_preview(FilterPreview& preview,std::stop_token stop,const std::string& label){
    try{
        modal_detail::poll(stop);require_no_floating_transform();
        const auto state=preview.state_;
        if(!state||state->owner!=this||!state->ready||!state->output)throw std::logic_error("Filter preview is not ready for this document");
        filter_detail::FilterPreviewState::Use use(state->busy);
        if(has_floating_selection())throw std::logic_error("Filter preview requires settled artwork");
        const auto current=[&]{return state->current(*this)&&modal_detail::data_same(data,state->baseline)
            &&raster_history_detail::same_identity_layout(data,state->baseline)&&modal_detail::selection_same(region_selection,state->selection,stop);};
        if(!current())throw std::logic_error("Filter preview source changed before acceptance");
        auto costs=state->execution_costs;modal_detail::data_charge(*state->output,costs.storage,0,false);
        const auto count=state->baseline.layers[state->targets.layer].planes.size();
        modal_detail::charge(costs.storage,(state->targets.published.size()+count)*sizeof(std::size_t)+count
            +(state->cached?0:state->targets.bound.size()*sizeof(filter_detail::DustBinding)),modal_detail::max_bytes,"Filter preview publication exceeds512MiB");
        modal_detail::charge(costs.work,3*count,modal_detail::max_work,"Filter preview publication exceeds128Mi work");
        auto candidate=*state->output;
        auto bound=state->cached?filter_detail::filter_preview_bindings(state->baseline,state->kind,state->selection,stop,costs):state->targets.bound;
        auto result=state->result;if(state->cached)result.status=DustFilterStatus::Applied;
        // Cached Apply skips the driver and binds each raster child's own ROI.
        // Empty selected intersections add neither INFO nor a dirty latch.
        std::vector<std::uint8_t> slots(count);for(const auto index:state->targets.published)slots[index]=1;
        for(const auto& binding:bound){auto& plane=candidate.layers[binding.layer].planes[binding.plane];plane.pixels_changed=true;plane.raster_modified=true;slots[binding.plane]=1;}
        std::vector<std::size_t> published;published.reserve(count);for(std::size_t i=0;i<count;++i)if(slots[i])published.push_back(i);
        raster_history_detail::Budget budget{costs.storage,costs.work,stop};
        // Computation used the deep baseline, never mutable live backing.
        // Untouched captured tiles can regain their matching live COW pointer.
        if(!state->current(*this)||!modal_detail::data_same(data,state->baseline)||!raster_history_detail::same_identity_layout(data,state->baseline))
            throw std::logic_error("Filter preview source changed during candidate copying");
        for(const auto index:published){auto& image=*candidate.layers[state->targets.layer].planes[index].raster;
            const auto& captured=state->baseline.layers[state->targets.layer].planes[index].raster->tiles;
            const auto& live=data.layers[state->targets.layer].planes[index].raster->tiles;budget.visit(image.tiles.size());
            for(auto& [key,bytes]:image.tiles){const auto old=captured.find(key),now=live.find(key);
                if(old!=captured.end()&&now!=live.end()&&bytes==old->second)bytes=now->second;}}
        budget.allocate(sizeof(raster_history_detail::Edit)+bound.size()*sizeof(raster_history_detail::Edit::PlaneState));
        auto edit=std::make_shared<raster_history_detail::Edit>();edit->native_raster_dirty=true;
        edit->layout=raster_history_detail::Edit::identity_layout(state->baseline,budget);edit->planes.reserve(bound.size());
        for(const auto& binding:bound){const auto& image=*state->baseline.layers[binding.layer].planes[binding.plane].raster;
            edit->planes.push_back({raster_history_detail::target(state->baseline,binding.layer,binding.plane),binding.roi,raster_history_detail::snapshot(image,binding.roi,budget),{}});}
        auto manager=result.applied()?prepare_paint_manager(paint_history_manager_,candidate,true,budget):paint_history_manager_;
        std::optional<Revision> item;std::vector<Revision> history;
        if(result.applied()){budget.allocate((undo_.size()+1)*sizeof(Revision)+label.size());
            item.emplace(Revision{DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::shared_ptr<const raster_history_detail::Edit>(std::move(edit))});history.reserve(undo_.size()+1);}
        budget.poll();if(!current())throw std::logic_error("Filter preview source changed during acceptance");require_no_floating_transform();budget.poll();
        // The ready token is private and immutable while this thread accepts it.
        // All capacity and snapshots are prepared before the no-throw tail.
        const auto layer=state->targets.layer;
        for(const auto index:published){auto& live=data.layers[layer].planes[index];auto& prepared=candidate.layers[layer].planes[index];
            *live.raster=std::move(*prepared.raster);live.pixels_changed=prepared.pixels_changed;live.raster_modified=prepared.raster_modified;}
        paint_history_manager_=std::move(manager);
        if(item){for(auto& old:undo_)history.push_back(std::move(old));history.push_back(std::move(*item));undo_.swap(history);
            trim_history();redo_.clear();undo_next_=true;}
        state->execution_costs={budget.bytes,budget.work};        state->ready=false;state->consumed=true;++state_generation_;return result;
    }catch(const std::exception& error){DustFilterResult result;result.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())result.error=error.what();return result;}
}
void Document::commit_raster_selection(DocumentData&& before,const RegionSelection& before_selection,
                                      RegionSelection after_selection,const std::string& label){
    require_no_floating_transform();
    if(has_floating_selection()||before.layers.empty())throw std::logic_error("Paired raster/selection history requires settled nonempty artwork");
    require_selection_layout(before,data,HistorySelection::PreserveCurrent);
    static_assert(std::is_nothrow_move_assignable_v<RegionSelection>);
    // Existing selection-only records always have empty Revision.data. A
    // nonempty compatible snapshot uniquely marks this paired record without
    // changing the RuntimeHistory or Document object layout.
    raster_history_detail::Budget budget;auto manager=prepare_paint_manager(paint_history_manager_,data,true,budget);
    RegionSelectionEdit edit{before_selection,after_selection,false};
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(edit),frame_content_changed(before,data)});
    std::swap(undo_.back().data,before);paint_history_manager_=std::move(manager);
    region_selection=std::move(after_selection);++region_selection_generation_;
    trim_history();
    redo_.clear();undo_next_=true;revision_=next_++;++state_generation_;
}
void Document::transact(const std::string& label,const std::function<void(DocumentData&)>& operation,HistorySelection selection){require_no_floating_transform();
    finalize_floating_selection();
    auto before=data;
    try {
        operation(data);
        require_selection_layout(before,data,selection);
        if(selection==HistorySelection::PreserveCurrent)preserve_selection(before,data);
        commit(std::move(before),label,selection);
    }
    catch(...) { data=std::move(before);throw; }
}
void Document::transact(const std::string& label,const std::function<void(DocumentData&)>& operation,
                        HistorySelection selection,std::stop_token stop){
    require_no_floating_transform();
    const auto poll=[&]{if(stop.stop_requested())throw std::runtime_error("Document transaction cancelled");};
    poll();
    if(has_floating_selection())throw std::logic_error("Stop-aware transaction requires settled artwork");
    auto before=data;
    // Admission does not edit live data. In particular, a stop or allocation
    // failure here must not restore the older snapshot over allocator reentry.
    std::string prepared_label=label;
    undo_.reserve(undo_.size()+1);
    poll();
    try{
        operation(data);
        require_selection_layout(before,data,selection);
        if(selection==HistorySelection::PreserveCurrent)preserve_selection(before,data);
        // Operations must not consume their parent's preallocated history slot.
        if(undo_.size()==undo_.capacity())throw std::logic_error("Document transaction history changed during operation");
        static_assert(std::is_nothrow_swappable_v<DocumentData>);
        static_assert(std::is_nothrow_move_constructible_v<Revision> && std::is_nothrow_move_assignable_v<Revision>);
        raster_history_detail::Budget budget;budget.stop=stop;auto manager=prepare_paint_manager(paint_history_manager_,data,true,budget);
        Revision item{DocumentData{},std::move(prepared_label),revision_,selection,{},frame_content_changed(before,data)};
        poll();
        undo_.push_back(std::move(item));
        std::swap(undo_.back().data,before);paint_history_manager_=std::move(manager);
        trim_history();
        redo_.clear();undo_next_=true;revision_=next_++;++state_generation_;
    }catch(...){data=std::move(before);throw;}
}
bool Document::delete_selected_layers(const std::string& label){require_no_floating_transform();
    if(data.layers.size()<2)return false;
    finalize_floating_selection();
    std::vector<std::size_t> indices;
    for(std::size_t i=0;i<data.layers.size();++i)if(data.layers[i].common.flags[2])indices.push_back(i);
    return delete_layers(indices,label);
}
bool Document::delete_invisible_layers(const std::string& label){require_no_floating_transform();
    if(data.layers.size()<2)return false;
    finalize_floating_selection();
    std::vector<std::size_t> indices;
    for(std::size_t i=0;i<data.layers.size();++i)if(!data.layers[i].common.flags[1])indices.push_back(i);
    return delete_layers(indices,label);
}
bool Document::delete_layers(const std::vector<std::size_t>& indices,const std::string& label){require_no_floating_transform();
    for(std::size_t i=0;i<indices.size();++i)if(indices[i]>=data.layers.size()||(i&&indices[i]<=indices[i-1]))
        throw std::invalid_argument("删除图层索引必须有序、唯一且存在。");
    if(data.layers.size()<2)return false;
    finalize_floating_selection();
    if(indices.empty())return false;
    // Native records the first selected node before rejecting delete-all.
    // That rejected command leaves one token but no history and keeps redo.
    if(indices.size()==data.layers.size()){++document_modify_count_;++state_generation_;return false;}
    auto manager=paint_history_manager_;
    if(manager){auto candidate=data;for(std::size_t i=0;i<indices.size();++i){const auto index=indices[i]-i;candidate.layers.erase(candidate.layers.begin()+index);remove_layer_selection(candidate,index);}if(candidate.current_layer>=0)candidate.layers[candidate.current_layer].common.flags[2]=1;
        raster_history_detail::Budget budget;const auto bound=paint_manager_owner(*manager,data);bool erased=!bound;
        if(bound){const auto where=std::lower_bound(indices.begin(),indices.end(),*bound);erased=where!=indices.end()&&*where==*bound;if(!erased)manager=relocate_paint_manager(manager,candidate,*bound-std::size_t(where-indices.begin()),budget);}
        manager=prepare_paint_manager(manager,candidate,erased,budget);}
    DeleteLayersEdit edit{indices,std::vector<Layer>(indices.size())};
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::RestoreSnapshot,std::move(edit)});
    auto& detached=std::get<DeleteLayersEdit>(undo_.back().runtime).detached;
    for(std::size_t i=0;i<indices.size();++i){const auto index=indices[i]-i;
        detached[i]=std::move(data.layers[index]);data.layers.erase(data.layers.begin()+index);remove_layer_selection(data,index);}
    if(data.current_layer>=0)data.layers[data.current_layer].common.flags[2]=1;
    paint_history_manager_=std::move(manager);
    trim_history();redo_.clear();undo_next_=true;++document_modify_count_;++state_generation_;return true;
}
void Document::insert_layer(std::size_t index,Layer layer,const std::string& label){insert_layer_checked(index,std::move(layer),label,{});}
void Document::insert_layer_checked(std::size_t index,Layer layer,const std::string& label,const std::function<void()>& before_publish){require_no_floating_transform();
    if(selection_command_budget_)selection_command_budget_->allocate(sizeof(Revision)+label.size());
    layer.common.runtime_identity=next_element_identity();
    for(auto& plane:layer.planes)plane.common.runtime_identity=next_element_identity();
    if(index>data.layers.size()||data.layers.size()>=std::size_t(std::numeric_limits<int>::max()))throw std::invalid_argument("新图层插入位置无效。");
    std::vector<Layer> layers(data.layers.size()+1);
    auto floating=remap_inserted_layer(data,floating_selection_,index,true);
    auto manager=paint_history_manager_;
    if(manager){auto candidate=data;candidate.layers.insert(candidate.layers.begin()+index,layer);select_inserted_layer(candidate,index);raster_history_detail::Budget local_budget;auto& budget=selection_command_budget_?*selection_command_budget_:local_budget;if(const auto bound=paint_manager_owner(*manager,data))manager=relocate_paint_manager(manager,candidate,*bound+(*bound>=index),budget);manager=prepare_paint_manager(manager,candidate,false,budget);}
    Revision ready{DocumentData{},label,revision_,HistorySelection::RestoreSnapshot,AddLayerEdit{index,{}}};
    undo_.reserve(undo_.size()+1);
    if(before_publish)before_publish();
    static_assert(std::is_nothrow_move_constructible_v<Revision>);
    undo_.push_back(std::move(ready));
    for(std::size_t i=0;i<data.layers.size();++i)layers[i+(i>=index)]=std::move(data.layers[i]);
    layers[index]=std::move(layer);data.layers.swap(layers);select_inserted_layer(data,index);
    if(floating_selection_!=floating){floating_selection_=std::move(floating);++floating_selection_generation_;}
    paint_history_manager_=std::move(manager);
    trim_history();redo_.clear();undo_next_=true;++document_modify_count_;++state_generation_;
}
bool Document::can_delete_selected_planes() const noexcept{
    if(has_text_preview()||has_floating_transform()||has_frame_move()||has_vanishing_point_move())return false;
    if(data.current_layer<0||data.current_layer>=int(data.layers.size()))return false;
    const auto& owner=data.layers[data.current_layer];
    if(owner.opaque||owner.planes.size()<2||owner.current_plane<0||owner.current_plane>=int(owner.planes.size()))return false;
    const auto& current=owner.planes[owner.current_plane];
    // sub_480140 checks the current plane, not each selected candidate.
    switch(current.family){case 0x11110001:case 0x11110002:case 0x11410001:case 0x12100001:case 0x12400001:return false;}
    return true;
}
bool Document::delete_selected_planes(const std::string& label){require_no_floating_transform();
    if(!can_delete_selected_planes())return false;
    const auto layer=std::size_t(data.current_layer);std::vector<std::size_t> indices;
    for(std::size_t i=0;i<data.layers[layer].planes.size();++i)if(data.layers[layer].planes[i].common.flags[2])indices.push_back(i);
    if(indices.empty()||indices.size()==data.layers[layer].planes.size())return false;
    // Native supplies one Multi to both floating finalization and AddPlane(0).
    // Stage the complete group so any allocation failure publishes neither.
    const bool foreign=floating_selection_&&!floating_selection_->sources&&!floating_selection_->same_document_source;
    auto staged=*this;staged.collecting_history_=true;staged.undo_.clear();staged.redo_.clear();staged.undo_next_=true;staged.finalize_floating_selection();
    // Foreign finalization appends typed AddLayer children and selects a newly
    // inserted layer. Keep the captured owner/indices above, checking that the
    // entire old prefix remains intact rather than re-resolving that selection.
    require_selection_layout(data,staged.data,HistorySelection::PreserveCurrent,foreign);
    DeletePlanesEdit edit{layer,indices,std::vector<Plane>(indices.size())};
    staged.undo_.push_back({DocumentData{},label,staged.revision_,HistorySelection::RestoreSnapshot,std::move(edit)});
    auto& owner=staged.data.layers[layer];auto& detached=std::get<DeletePlanesEdit>(staged.undo_.back().runtime).detached;
    for(std::size_t i=0;i<indices.size();++i){const auto index=indices[i]-i;detached[i]=std::move(owner.planes[index]);
        owner.planes.erase(owner.planes.begin()+index);remove_plane_selection(owner,index);}
    if(owner.current_plane>=0)owner.planes[owner.current_plane].common.flags[2]=1;
    raster_history_detail::Budget local_budget;auto& budget=staged.selection_command_budget_?*staged.selection_command_budget_:local_budget;staged.paint_history_manager_=prepare_paint_manager(staged.paint_history_manager_,staged.data,true,budget);
    ++staged.document_modify_count_;
    auto group=std::make_shared<const CompoundEdit>(CompoundEdit{std::move(staged.undo_),std::move(staged.redo_)});
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::RestoreSnapshot,std::move(group)});
    trim_history();
    staged.undo_=std::move(undo_);staged.state_generation_=state_generation_+1;
    static_assert(std::is_nothrow_swappable_v<Document>);staged.history_limit_=history_limit_;staged.collecting_history_=collecting_history_;staged.trim_history();std::swap(*this,staged);return true;
}
bool Document::can_copy_current_plane() const noexcept{
    if(has_text_preview()||has_floating_transform()||has_frame_move()||has_vanishing_point_move())return false;
    if(data.current_layer<0||data.current_layer>=int(data.layers.size()))return false;
    const auto& owner=data.layers[data.current_layer];
    if(owner.opaque||owner.current_plane<0||owner.current_plane>=int(owner.planes.size()))return false;
    switch(owner.family){
    case 0x10000100:case 0x10000400:case 0x10008000:case 0x10010000:case 0x10040000:
    case 0x40000800:case 0x40020000:case 0x48040800:return false;
    }
    const auto& source=owner.planes[owner.current_plane];
    if(source.opaque)return false;
    if(!source.raster){
        //4800B0+63B850 exclude both Pure and both Paint aliases. The
        //remaining vector CPlane Copy slot copies common metadata only.
        if(!owner.common.runtime_operable)return false;
        if(source.family==0x21210001||source.family==0x21220001)
            return owner.family==0x22100000||owner.family==0x24400000||owner.family==0x21200000||owner.family==0x20800000||owner.family==0x20000200;
        return (source.family==0x40000001&&owner.family==0x40004000)||(source.family==0x40000002&&owner.family==0x40002000);
    }
    return source.family==0x10000001||source.family==0x11210001||source.family==0x11220001||source.family==0x11240001;
}
bool Document::copy_current_plane(const std::string& label){require_no_floating_transform();
    if(!can_copy_current_plane())return false;
    const auto layer=std::size_t(data.current_layer);const auto& owner=data.layers[layer];
    const auto& source=owner.planes[owner.current_plane];
    auto plane=source.raster?source:make_plane(owner,source.family,{},source.common.color);
    if(!source.raster){plane.common=source.common;plane.common.suffix.clear();plane.raster_modified=source.raster_modified;}
    const auto suffix=encode_cp936("的拷贝");
    plane.common.name.insert(plane.common.name.end(),suffix.begin(),suffix.end());
    // sub_60D4B0 copies common metadata and dirtiness, but leaves the newly
    // constructed plane's +0x94 mode and collection ordinal untouched.
    plane.common.opacity_mode=1;plane.common.field_14=-1;plane.pixels_changed=true;
    insert_plane(layer,owner.planes.size(),std::move(plane),label);return true;
}
void Document::insert_plane(std::size_t layer,std::size_t index,Plane plane,const std::string& label){require_no_floating_transform();
    if(selection_command_budget_)selection_command_budget_->allocate(sizeof(Revision)+label.size());
    plane.common.runtime_identity=next_element_identity();
    if(layer>=data.layers.size()||index>data.layers[layer].planes.size()||data.layers[layer].planes.size()>=std::size_t(std::numeric_limits<int>::max()))
        throw std::invalid_argument("新平面插入位置无效。");
    auto& owner=data.layers[layer];std::vector<Plane> planes(owner.planes.size()+1);
    for(std::size_t i=0;i<owner.planes.size();++i)prepare_plane_transfer(planes[i+(i>=index)],owner.planes[i]);
    prepare_plane_transfer(planes[index],plane);
    auto floating=remap_inserted_plane(data,floating_selection_,layer,index,true);
    auto manager=paint_history_manager_;
    if(manager){auto candidate=data;candidate.layers[layer].planes.insert(candidate.layers[layer].planes.begin()+index,plane);select_inserted_plane(candidate.layers[layer],index);raster_history_detail::Budget local_budget;auto& budget=selection_command_budget_?*selection_command_budget_:local_budget;manager=prepare_paint_manager(manager,candidate,true,budget);}
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::RestoreSnapshot,AddPlaneEdit{layer,index,{}}});
    for(std::size_t i=0;i<owner.planes.size();++i)transfer_plane(planes[i+(i>=index)],owner.planes[i]);
    transfer_plane(planes[index],plane);owner.planes.swap(planes);select_inserted_plane(owner,index);
    if(floating_selection_!=floating){floating_selection_=std::move(floating);++floating_selection_generation_;}
    paint_history_manager_=std::move(manager);
    trim_history();redo_.clear();undo_next_=true;++document_modify_count_;++state_generation_;
}
void Document::move_selected_layers(std::size_t gap,const std::string& label){order_layers(selected_group_order(data.layers,gap),label);}
void Document::move_selected_planes(std::size_t layer,std::size_t gap,const std::string& label){
    if(layer>=data.layers.size())throw std::invalid_argument("排序图层不存在。");
    order_planes(layer,selected_group_order(data.layers[layer].planes,gap),label);
}
void Document::order_layers(const std::vector<std::size_t>& order,const std::string& label){order_elements({},order,label);}
void Document::order_planes(std::size_t layer,const std::vector<std::size_t>& order,const std::string& label){order_elements(layer,order,label);}
void Document::order_elements(std::optional<std::size_t> layer,const std::vector<std::size_t>& order,const std::string& label){require_no_floating_transform();
    auto prepared=prepare_order(data,floating_selection_,layer,order);
    auto manager=paint_history_manager_;
    if(manager){auto candidate=data;auto copy=prepare_order(candidate,floating_selection_,layer,order);apply_order(candidate,layer,order,copy);raster_history_detail::Budget budget;if(const auto bound=paint_manager_owner(*manager,data))manager=relocate_paint_manager(manager,candidate,layer?*bound:std::size_t(std::ranges::find(order,*bound)-order.begin()),budget);manager=prepare_paint_manager(manager,candidate,bool(layer),budget);}
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,OrderEdit{layer,std::move(prepared.inverse)}});
    apply_order(data,layer,order,prepared);
    if(floating_selection_!=prepared.floating){floating_selection_=std::move(prepared.floating);++floating_selection_generation_;}
    paint_history_manager_=std::move(manager);
    trim_history();redo_.clear();undo_next_=true;++document_modify_count_;++state_generation_;
}
void Document::adopt_current_selection(const Document& current){require_no_floating_transform();
    require_selection_layout(current.data,data,HistorySelection::PreserveCurrent);
    auto candidate=data;preserve_selection(current.data,candidate);raster_history_detail::Budget budget;auto manager=prepare_paint_manager(paint_history_manager_,candidate,false,budget);
    preserve_selection(current.data,data);paint_history_manager_=std::move(manager);
}
bool Document::set_visibility(std::size_t layer,std::optional<std::size_t> plane,bool visible,bool mark_modified){require_no_floating_transform();
    if(layer>=data.layers.size()||(plane&&*plane>=data.layers[layer].planes.size()))throw std::invalid_argument("可见性目标不存在。");
    auto& flags=plane?data.layers[layer].planes[*plane].common.flags:data.layers[layer].common.flags;
    if(bool(flags[1])==visible)return false;
    flags[1]=visible;if(mark_modified){if(plane)data.layers[layer].planes[*plane].visibility_modified=true;else data.layers[layer].visibility_modified=true;}
    ++state_generation_;return true;
}
void Document::set_batch_layer_visibility(std::size_t layer,std::int32_t value){require_no_floating_transform();
    auto& owner=data.layers.at(layer);owner.common.flags[1]=value;owner.visibility_modified=true;++state_generation_;
}
void Document::transform_page(std::int32_t kind,std::stop_token stop,const std::string& label){
    require_no_floating_transform();const auto inverse=page_geometry_detail::inverse(kind);modal_detail::poll(stop);
    const SelectionIndicationWitness witness(*this);const auto generation=state_generation_;const auto manager=paint_history_manager_;
    raster_history_detail::Budget budget;budget.stop=stop;modal_detail::data_charge(data,budget.bytes);const auto before=modal_detail::frozen(data,stop);modal_detail::data_charge(before,budget.bytes);
    const auto initial_floating=floating_selection_;const auto floating_generation=floating_selection_generation_;std::optional<DocumentData> floating_before;
    auto candidate=*this;candidate.data=before;
    if(initial_floating){if(!initial_floating->layers)throw std::invalid_argument("Page floating payload has no storage");floating_before.emplace();floating_before->layers=*initial_floating->layers;modal_detail::data_charge(*floating_before,budget.bytes);*floating_before=modal_detail::frozen(*floating_before,stop);
        budget.allocate(sizeof(FloatingSelection));auto payload=std::make_shared<FloatingSelection>(*initial_floating);payload->layers=std::make_shared<const std::vector<Layer>>(floating_before->layers);
        if(initial_floating->sources){if(initial_floating->sources->size()>100000)throw std::length_error("Page floating source map exceeds100000");budget.allocate(initial_floating->sources->size()*sizeof(FloatingSource));for(const auto& source:*initial_floating->sources){if(source.planes.size()>100000)throw std::length_error("Page floating plane map exceeds100000");budget.allocate(source.planes.size()*sizeof(std::size_t));}payload->sources=std::make_shared<const std::vector<FloatingSource>>(*initial_floating->sources);}candidate.floating_selection_=std::move(payload);}
    candidate.finalize_floating_selection(label);modal_detail::poll(stop);
    auto selected=selection_history_snapshot(candidate.data,candidate.region_selection);
    page_geometry_detail::apply(candidate.data,kind,budget.bytes,budget.work,stop);
    candidate.region_selection=cleared_selection({}, {candidate.data.base.width(),candidate.data.base.height()});candidate.region_selection.carrier_dpi=candidate.data.base.dpi;
    candidate.vector_indication_active=false;++candidate.region_selection_generation_;candidate.floating_selection_.reset();++candidate.floating_selection_generation_;
    budget.allocate(sizeof(CompoundEdit)+2*sizeof(Revision)+(candidate.undo_.size()+1)*sizeof(Revision)+label.size()*3);
    auto group=std::make_shared<CompoundEdit>();group->past.reserve(2);
    group->past.push_back({DocumentData{},label,candidate.revision_,HistorySelection::PreserveCurrent,RegionSelectionEdit{std::move(selected),{},{}}});
    group->past.push_back({DocumentData{},label,candidate.revision_,HistorySelection::PreserveCurrent,PageTransformEdit{inverse}});
    candidate.paint_history_manager_=prepare_paint_manager(candidate.paint_history_manager_,candidate.data,true,budget);
    candidate.undo_.push_back({DocumentData{},label,candidate.revision_,HistorySelection::PreserveCurrent,std::shared_ptr<const CompoundEdit>(std::move(group))});candidate.trim_history();candidate.redo_.clear();candidate.undo_next_=true;
    candidate.runtime_dirty_=true;++candidate.document_modify_count_;candidate.state_generation_=generation+1;
    budget.poll();bool same_floating=floating_selection_==initial_floating&&floating_selection_generation_==floating_generation;if(same_floating&&floating_before){DocumentData live;live.layers=*initial_floating->layers;same_floating=modal_detail::data_same(live,*floating_before);}
    if(state_generation_!=generation||!witness.matches(*this)||!same_floating||paint_history_manager_!=manager||!modal_detail::data_same(data,before)||!raster_history_detail::same_identity_layout(data,before))throw std::logic_error("Page transform source changed before publication");
    static_assert(std::is_nothrow_swappable_v<Document>);std::swap(*this,candidate);
}
void Document::adopt_current_runtime(const Document& current){require_no_floating_transform();
    if(has_floating_selection()||current.has_floating_selection())throw std::logic_error("不能把未写回的浮动图像附着到另一份画稿内容。");
    // Copy the allocating Guide array before publishing either runtime object.
    auto replacement=current.guide;
    guide=std::move(replacement);grid=current.grid;
    guide_generation_=current.guide_generation_;grid_generation_=current.grid_generation_;
    application_vector_selection_flags=current.application_vector_selection_flags;
    vector_indication_active=current.vector_indication_active;
    region_selection=current.region_selection;region_selection_generation_=current.region_selection_generation_;
    attached_temporary_layers_=current.attached_temporary_layers_;attached_temporary_generation_=current.attached_temporary_generation_;
}
void Document::swap_runtime(Document& other){require_no_floating_transform();other.require_no_floating_transform();
    static_assert(std::is_nothrow_swappable_v<GuideState>);
    using std::swap;swap(grid,other.grid);swap(guide,other.guide);
    swap(grid_generation_,other.grid_generation_);swap(guide_generation_,other.guide_generation_);
    swap(application_vector_selection_flags,other.application_vector_selection_flags);
    swap(vector_indication_active,other.vector_indication_active);
    swap(region_selection,other.region_selection);swap(region_selection_generation_,other.region_selection_generation_);
    swap(floating_selection_,other.floating_selection_);swap(floating_selection_generation_,other.floating_selection_generation_);
    swap(floating_same_document_source_,other.floating_same_document_source_);
    swap(attached_temporary_layers_,other.attached_temporary_layers_);swap(attached_temporary_generation_,other.attached_temporary_generation_);
}
void Document::prepare_selection_command(const std::string& label,std::stop_token stop,const std::function<void(Document&,std::uint64_t&,std::uint64_t&)>& operation,bool publish,std::array<std::uint64_t,2> prior_costs){
    require_no_floating_transform();
    (void)selection_carrier_size(region_selection,data.base.width(),data.base.height());
    (void)selection_carrier_dpi(region_selection,data.base.dpi);
    const SelectionIndicationWitness witness(*this);const auto manager=paint_history_manager_;
    const auto attached=attached_temporary_layers_;const auto attached_generation=attached_temporary_generation_;
    const auto grid_before=grid;const auto guide_before=guide;const auto path_before=path;
    const auto grid_generation=grid_generation_,guide_generation=guide_generation_;
    const auto revision=revision_,saved=saved_,next=next_,modify_count=document_modify_count_;
    const auto undo_size=undo_.size(),redo_size=redo_.size();const bool direction=undo_next_,dirty=runtime_dirty_;
    const auto undo_id=undo_.empty()?0:undo_.back().id,redo_id=redo_.empty()?0:redo_.back().id;
    const auto baseline=raw_baseline_;const auto original=data.original;const auto original_content=data.original_content;
    const auto mask_generation=region_selection.mask?region_selection.mask->mutation_generation():0;
    raster_history_detail::Budget local_budget;local_budget.stop=stop;auto& budget=selection_command_budget_?*selection_command_budget_:local_budget;
    budget.allocate(prior_costs[0]);budget.visit(prior_costs[1]);
    modal_detail::data_charge(data,budget.bytes);const auto frozen=modal_detail::frozen(data,stop);modal_detail::data_charge(frozen,budget.bytes);
    budget.allocate((undo_.size()+redo_.size()+2)*sizeof(Revision)+label.size()*3+sizeof(CompoundEdit));
    Document staged(frozen);staged.history_limit_=history_limit_;staged.collecting_history_=true;
    staged.vanishing_move_=vanishing_move_;staged.vanishing_move_preparing_=vanishing_move_preparing_;staged.frame_move_=frame_move_;staged.frame_move_preparing_=frame_move_preparing_;staged.floating_transform_=floating_transform_;staged.floating_transform_preparing_=floating_transform_preparing_;staged.text_preview_owner_=text_preview_owner_;
    staged.undo_next_=undo_next_;staged.revision_=revision_;staged.saved_=saved_;staged.next_=next_;staged.state_generation_=state_generation_;staged.selection_command_budget_=&budget;staged.building_clipboard_group_=building_clipboard_group_;
    staged.attached_temporary_layers_=attached_temporary_layers_;staged.attached_temporary_generation_=attached_temporary_generation_;staged.grid_generation_=grid_generation_;staged.guide_generation_=guide_generation_;staged.region_selection_generation_=region_selection_generation_;staged.floating_selection_generation_=floating_selection_generation_;
    staged.floating_selection_=floating_selection_;staged.floating_same_document_source_=floating_same_document_source_;staged.runtime_dirty_=runtime_dirty_;staged.document_modify_count_=document_modify_count_;staged.raw_baseline_=raw_baseline_;staged.paint_history_manager_=paint_history_manager_;
    staged.grid=grid;staged.guide=guide;staged.region_selection=region_selection;staged.application_vector_selection_flags=application_vector_selection_flags;staged.vector_indication_active=vector_indication_active;staged.path=path;
    if(staged.region_selection.mask){const auto& mask=*staged.region_selection.mask;
        budget.allocate(((std::uint64_t(mask.width())+31)/32)*mask.height()*4+std::uint64_t((mask.width()+255)/256)*((mask.height()+255)/256));
        staged.region_selection.mask=std::make_shared<const BinaryMask>(mask);
    }
    std::optional<DocumentData> floating_before;
    if(witness.floating){
        if(!witness.floating->layers)throw std::invalid_argument("选区命令的浮动内容缺少载体。");
        floating_before.emplace();floating_before->layers=*witness.floating->layers;modal_detail::data_charge(*floating_before,budget.bytes);
        *floating_before=modal_detail::frozen(*floating_before,stop);modal_detail::data_charge(*floating_before,budget.bytes);
        auto payload=std::make_shared<FloatingSelection>(*witness.floating);payload->layers=std::make_shared<const std::vector<Layer>>(floating_before->layers);
        staged.floating_selection_=std::move(payload);
    }
    operation(staged,budget.bytes,budget.work);
    staged.selection_command_budget_=selection_command_budget_;
    std::vector<Revision> combined_history;if(publish&&!staged.undo_.empty())combined_history.reserve(undo_.size()+staged.undo_.size());
    budget.poll();bool same_floating=true;
    if(floating_before){DocumentData live;live.layers=*witness.floating->layers;same_floating=modal_detail::data_same(live,*floating_before);}
    if(!witness.matches(*this)||paint_history_manager_!=manager||!same_floating
       ||attached_temporary_layers_!=attached||attached_temporary_generation_!=attached_generation
       ||grid!=grid_before||guide!=guide_before||path!=path_before||grid_generation_!=grid_generation||guide_generation_!=guide_generation
       ||revision_!=revision||saved_!=saved||next_!=next||document_modify_count_!=modify_count||runtime_dirty_!=dirty
       ||undo_next_!=direction||undo_.size()!=undo_size||redo_.size()!=redo_size
       ||(undo_.empty()?0:undo_.back().id)!=undo_id||(redo_.empty()?0:redo_.back().id)!=redo_id
       ||raw_baseline_!=baseline||data.original!=original||data.original_content!=original_content
       ||(region_selection.mask&&region_selection.mask->mutation_generation()!=mask_generation)
       ||!modal_detail::data_same(data,frozen)||!raster_history_detail::same_identity_layout(data,frozen))
        throw std::logic_error("选区命令准备期间源已改变。");
    budget.poll();require_no_floating_transform();
    if(!publish)return;
    staged.data.original=original;staged.data.original_content=original_content;
    if(staged.region_selection_generation_==region_selection_generation_)staged.region_selection=region_selection;
    if(staged.undo_.empty()){staged.undo_=std::move(undo_);staged.redo_=std::move(redo_);}
    else{for(auto& item:undo_)combined_history.push_back(std::move(item));for(auto& item:staged.undo_)combined_history.push_back(std::move(item));staged.undo_=std::move(combined_history);}
    static_assert(std::is_nothrow_swappable_v<Document>);
    staged.state_generation_=state_generation_+1;staged.history_limit_=history_limit_;staged.collecting_history_=collecting_history_;staged.trim_history();std::swap(*this,staged);
}
bool Document::can_copy_selection() const noexcept{
    return has_floating_selection()||selection_copy_detail::query(data,region_selection,application_vector_selection_flags.value_or(region_selection.vector_flags));
}
bool Document::can_copy_selection(const SelectionCopyOptions& options) const noexcept{
    if(!options.current_plane_only||has_floating_selection())return can_copy_selection();
    if(data.current_layer<0||std::size_t(data.current_layer)>=data.layers.size())return false;const auto& owner=data.layers[std::size_t(data.current_layer)];
    return owner.common.flags[1]&&owner.current_plane>=0&&std::size_t(owner.current_plane)<owner.planes.size()&&owner.planes[std::size_t(owner.current_plane)].common.flags[1];
}
bool Document::can_cut_clipboard(const SelectionCopyOptions& options)const noexcept{
    if(has_floating_selection())return true;
    if(options.current_plane_only)return can_copy_selection(options)&&data.layers[std::size_t(data.current_layer)].common.runtime_operable;
    return selection_copy_detail::query(data,region_selection,application_vector_selection_flags.value_or(region_selection.vector_flags),true);
}
std::shared_ptr<const SelectionCopyPacket> Document::copy_selection(std::stop_token stop){
    return copy_selection(SelectionCopyOptions{},stop);
}
std::shared_ptr<const SelectionCopyPacket> Document::copy_selection(const SelectionCopyOptions& options,std::stop_token stop){
    require_no_floating_transform();if(!can_copy_selection(options))return {};
    const auto captured_options=options;
    const bool live=has_floating_selection();std::shared_ptr<const SelectionCopyPacket> packet;
    const auto original=data.original;const auto original_content=data.original_content;const auto selection_before=region_selection;
    prepare_selection_command({},stop,[&](Document& staged,std::uint64_t& bytes,std::uint64_t& work){
        if(live){
            packet=selection_copy_detail::live(*staged.floating_selection_,staged.floating_selection_->native_clipboard?staged.floating_selection_->clipboard_local:staged.floating_same_document_source_,bytes,work,stop);
            if(staged.floating_selection_->native_clipboard){auto result=std::make_shared<SelectionCopyPacket>(*packet);result->sources=staged.floating_selection_->clipboard_sources;result->current_plane_only=staged.floating_selection_->clipboard_current_plane_only;packet=std::move(result);}
            if(staged.floating_same_document_source_&&staged.floating_selection_->sources){
                auto result=std::make_shared<SelectionCopyPacket>(*packet);auto sources=std::make_shared<std::vector<SelectionCopyPacket::Source>>();
                for(const auto& mapping:*staged.floating_selection_->sources){modal_detail::poll(stop);const auto& owner=staged.data.layers.at(mapping.layer);SelectionCopyPacket::Source source;source.owner=owner.common.runtime_identity;
                    modal_detail::charge(bytes,sizeof(source)+mapping.planes.size()*sizeof(std::uint64_t),modal_detail::max_bytes,"COPY source bindings exceed storage limit");
                    for(auto p:mapping.planes)source.planes.push_back(owner.planes.at(p).common.runtime_identity);sources->push_back(std::move(source));}
                result->sources=std::move(sources);packet=std::move(result);
            }
        }
        else{
            auto selection=staged.region_selection;selection.vector_flags=staged.application_vector_selection_flags.value_or(selection.vector_flags);
            packet=selection_copy_detail::selected(staged.data,selection,captured_options,staged.vector_indication_active,bytes,work,stop);
            //COPY owns only indication effects. Keep the live selection and
            //original archive identities, including ExactRaw eligibility.
            staged.region_selection=selection_before;staged.data.original=original;staged.data.original_content=original_content;
        }
    },!live);
    return packet;
}
std::shared_ptr<const SelectionCopyPacket> Document::cut_clipboard(const SelectionCopyOptions& options,const std::string& label,std::stop_token stop){
    require_no_floating_transform();if(!can_copy_selection(options))return {};
    std::shared_ptr<const SelectionCopyPacket> packet;
    prepare_selection_command(label,stop,[&](Document& staged,std::uint64_t&,std::uint64_t&){
        const auto before=staged.data;const bool active=staged.vector_indication_active;
        packet=staged.copy_selection(options,stop);
        if(staged.floating_selection_){staged.discard_floating_selection(label);return;}
        //COPY's tail is an export effect; CUT consumes the original live
        //marks and owns its own single remainder tail and source history.
        staged.data=before;staged.vector_indication_active=active;
        auto history=std::move(staged.undo_);staged.undo_.clear();staged.redo_.clear();
        staged.begin_floating_capture(label,{},true,options.current_plane_only);
        if(!staged.undo_.empty()){
            const auto captured=std::get<std::shared_ptr<const CompoundEdit>>(staged.undo_.back().runtime);auto contents=std::make_shared<CompoundEdit>(*captured);
            std::erase_if(contents->past,[](const auto& item){return std::holds_alternative<RegionSelectionEdit>(item.runtime)||std::holds_alternative<FloatingSelectionEdit>(item.runtime);});
            staged.undo_.clear();if(!contents->past.empty())staged.undo_.push_back({DocumentData{},label,staged.revision_,HistorySelection::PreserveCurrent,std::shared_ptr<const CompoundEdit>(std::move(contents))});
        }
        staged.floating_selection_.reset();++staged.floating_selection_generation_;
        std::vector<std::size_t> text;
        if(!options.current_plane_only)for(std::size_t i=0;i<before.layers.size();++i){const auto& owner=before.layers[i];if((owner.family==0x40000800u||owner.family==0x48040800u)&&owner.common.flags[1]&&owner.common.flags[2]&&owner.common.runtime_operable&&!owner.planes.empty()&&owner.planes[0].common.flags[1])text.push_back(i);}
        if(!text.empty()){
            DeleteLayersEdit removed{text,std::vector<Layer>(text.size())};
            for(std::size_t i=0;i<text.size();++i){const auto index=text[i]-i;removed.detached[i]=std::move(staged.data.layers[index]);staged.data.layers.erase(staged.data.layers.begin()+index);remove_layer_selection(staged.data,index);}
            staged.undo_.push_back({DocumentData{},label,staged.revision_,HistorySelection::RestoreSnapshot,std::move(removed)});++staged.document_modify_count_;staged.runtime_dirty_=true;
            raster_history_detail::Budget local_budget;auto& budget=staged.selection_command_budget_?*staged.selection_command_budget_:local_budget;staged.paint_history_manager_=prepare_paint_manager(staged.paint_history_manager_,staged.data,true,budget);
        }
        if(staged.region_selection.active())staged.select_region(cleared_selection({},selection_carrier_size(staged.region_selection,staged.data.base.width(),staged.data.base.height())),label);
        if(!staged.undo_.empty()){auto group=std::make_shared<const CompoundEdit>(CompoundEdit{std::move(staged.undo_),{}});history.push_back({DocumentData{},label,staged.revision_,HistorySelection::PreserveCurrent,std::move(group)});}
        staged.undo_=std::move(history);staged.trim_history();staged.redo_.clear();staged.undo_next_=true;
    });return packet;
}
bool Document::paste_clipboard(const SelectionCopyPacket& holder,bool local,const std::string& label,PasteSelection selection,const FloatingPreviewValidator& validate,std::stop_token stop){
    require_no_floating_transform();if(!holder.layers||holder.layers->empty())return false;
    if(selection!=PasteSelection::SourceFrame&&selection!=PasteSelection::Content)throw std::invalid_argument("Invalid clipboard selection policy");
    const auto captured=holder;
    prepare_selection_command(label,stop,[&](Document& staged,std::uint64_t& bytes,std::uint64_t&){
        DocumentData borrowed;borrowed.layers=*captured.layers;modal_detail::data_charge(borrowed,bytes);const auto frozen=modal_detail::frozen(borrowed,stop);modal_detail::data_charge(frozen,bytes);auto packet=captured;packet.layers=std::make_shared<const std::vector<Layer>>(frozen.layers);
        auto history=std::move(staged.undo_);staged.undo_.clear();staged.redo_.clear();staged.finalize_floating_selection(label);
        auto imported=prepare_plain_paste(staged.data,packet,selection==PasteSelection::Content,staged.region_selection);
        staged.select_region(std::move(imported.selection),label);
        if(imported.floating){auto floating=std::make_shared<FloatingSelection>(*imported.floating);floating->native_clipboard=true;floating->clipboard_local=local&&packet.source_is_current_document;floating->clipboard_sources=packet.sources;floating->clipboard_current_plane_only=packet.current_plane_only;
            FloatingSelectionEdit edit;edit.exchange_data=false;edit.foreign_payload=true;staged.undo_.push_back({DocumentData{},label,staged.revision_,HistorySelection::PreserveCurrent,std::move(edit)});
            staged.floating_selection_=std::move(floating);staged.floating_same_document_source_=false;++staged.floating_selection_generation_;if(validate)validate(staged.data,staged.floating_selection_);
        }
        DocumentData current;current.layers=*captured.layers;if(!modal_detail::data_same(current,frozen))throw std::logic_error("Clipboard changed during import preparation");
        auto group=std::make_shared<const CompoundEdit>(CompoundEdit{std::move(staged.undo_),std::move(staged.redo_)});history.push_back({DocumentData{},label,staged.revision_,HistorySelection::PreserveCurrent,std::move(group)});
        staged.undo_=std::move(history);staged.trim_history();staged.redo_.clear();staged.undo_next_=true;
    });return true;
}
bool Document::convert_paste(const SelectionCopyPacket& holder,const ConvertPasteOptions& options,const std::string& label,PasteSelection selection,const FloatingPreviewValidator& validate,std::stop_token stop){
    require_no_floating_transform();const auto candidates=convert_paste_candidates(data,holder,options.current_plane_only);const auto captured=options;
    const auto& choices=captured.new_layer?candidates.layers:candidates.planes;if(std::ranges::find(choices,captured.family)==choices.end())return false;
    const auto packet=holder;
    prepare_selection_command(label,stop,[&](Document& staged,std::uint64_t& bytes,std::uint64_t&){
        DocumentData borrowed;borrowed.layers=*packet.layers;modal_detail::data_charge(borrowed,bytes);const auto frozen=modal_detail::frozen(borrowed,stop);modal_detail::data_charge(frozen,bytes);
        auto history=std::move(staged.undo_);staged.undo_.clear();staged.redo_.clear();staged.finalize_floating_selection(label);
        FloatingTarget target;
        if(captured.new_layer){LayerCreateOptions create;create.family=captured.family;create.name=captured.name;staged.insert_layer(staged.data.layers.size(),make_layer(staged.data.base,create),label);target={std::size_t(staged.data.current_layer),std::size_t(staged.data.layers[std::size_t(staged.data.current_layer)].current_plane)};}
        else{target.layer=std::size_t(staged.data.current_layer);target.plane=staged.data.layers.at(target.layer).planes.size();staged.insert_plane(target.layer,target.plane,make_plane(staged.data.layers.at(target.layer),captured.family,captured.name),label);}
        FloatingSelection exported;exported.kind=FloatingKind::General;exported.layers=std::make_shared<const std::vector<Layer>>(frozen.layers);exported.tight_bounds=holder.tight_bounds;exported.capture_frame=holder.capture_frame;
        staged.paste_targeted_floating(exported,target,label,selection,validate);
        DocumentData current;current.layers=*packet.layers;if(!modal_detail::data_same(current,frozen))throw std::logic_error("Clipboard changed during conversion preparation");
        auto group=std::make_shared<const CompoundEdit>(CompoundEdit{std::move(staged.undo_),std::move(staged.redo_)});history.push_back({DocumentData{},label,staged.revision_,HistorySelection::PreserveCurrent,std::move(group)});
        staged.undo_=std::move(history);staged.trim_history();staged.redo_.clear();staged.undo_next_=true;
    });return true;
}
bool Document::layer_to_selection(const std::string& label,std::stop_token stop){
    require_no_floating_transform();
    if(data.current_layer<0||std::size_t(data.current_layer)>=data.layers.size()||data.layers[data.current_layer].family!=0x10008000)return false;
    const auto owner=data.layers[data.current_layer].common.runtime_identity;
    prepare_selection_command(label,stop,[&](Document& staged,std::uint64_t& bytes,std::uint64_t& work){
        //6759C0 retains the source CLayer pointer across6752B0 settlement.
        staged.edit_region_selection(label,[&](const DocumentData& settled,const RegionSelection& previous){
            const Layer* source{};for(const auto& layer:settled.layers)if(layer.common.runtime_identity==owner){if(source)throw std::logic_error("选区源图层标识重复。");source=&layer;}
            if(!source)throw std::logic_error("浮动结算后选区源图层已丢失。");
            return selection_detail::from_selection_layer(*source,previous,settled.base.width(),settled.base.height(),settled.base.dpi,bytes,work,stop);
        });
    });return true;
}
bool Document::combine_selection_layer(bool subtract,const std::string& label,std::stop_token stop){
    require_no_floating_transform();if(data.current_layer<0||std::size_t(data.current_layer)>=data.layers.size()||data.layers[std::size_t(data.current_layer)].family!=0x10008000u)return false;
    const auto owner=data.layers[std::size_t(data.current_layer)].common.runtime_identity;
    prepare_selection_command(label,stop,[&](Document& staged,std::uint64_t& bytes,std::uint64_t& work){bool propagate{};staged.edit_region_selection(label,[&](const DocumentData& settled,const RegionSelection& previous){
        const Layer* source{};for(const auto& layer:settled.layers)if(layer.common.runtime_identity==owner){if(source)throw std::logic_error("Duplicate selection layer identity");source=&layer;}if(!source)throw std::logic_error("Selection source was removed during settlement");
        return selection_detail::combine_selection_layer(*source,previous,subtract,settled.base.width(),settled.base.height(),settled.base.dpi,bytes,work,stop,&propagate);
    },&propagate);});return true;
}
bool Document::can_select_color(SelectionColorCommand command) const noexcept{
    if(command<SelectionColorCommand::Color||command>SelectionColorCommand::ColorSpace||data.current_layer<0||std::size_t(data.current_layer)>=data.layers.size())return false;
    const auto& owner=data.layers[std::size_t(data.current_layer)];const auto* plane=data.active_plane();
    return owner.common.flags[1]&&!(owner.family&0x40000000u)&&plane&&plane->common.flags[1]
        &&(command!=SelectionColorCommand::ColorSpace||plane->common.opacity_mode!=4);
}
bool Document::select_color(SelectionColorCommand command,Pixel reference,const std::string& label,bool additive_space,bool minimum_width,std::stop_token stop){
    require_no_floating_transform();if(!can_select_color(command))return false;
    prepare_selection_command(label,stop,[&](Document& staged,std::uint64_t& bytes,std::uint64_t& work){
        auto group=std::make_shared<CompoundEdit>();
        group->past.push_back({DocumentData{},label,staged.revision_,HistorySelection::PreserveCurrent,RegionSelectionEdit{selection_history_snapshot(staged.data,staged.region_selection),{},{}}});
        auto history=std::move(staged.undo_);staged.undo_.clear();staged.redo_.clear();
        //6752B0 records Select first, then optional mode1 settlement. Resolve
        //the current target only AFTER that settlement, unlike6759C0.
        if(staged.floating_selection_&&(staged.floating_selection_->target||staged.floating_selection_->sources)){
            const auto bind=[&](std::size_t index){const auto& owner=staged.data.layers.at(index);modal_detail::charge(work,staged.data.layers.size(),modal_detail::max_work,"Color identity work exceeds limit");if(std::ranges::count(staged.data.layers,owner.common.runtime_identity,[](const auto& l){return l.common.runtime_identity;})!=1)throw std::invalid_argument("Color floating owner identity is not unique");(void)raster_scope_planes(owner,bytes,work,stop);};
            if(staged.floating_selection_->target)bind(staged.floating_selection_->target->layer);else for(const auto& mapping:*staged.floating_selection_->sources)bind(mapping.layer);
        }
        staged.finalize_floating_selection(label);
        for(auto& item:staged.undo_)group->past.push_back(std::move(item));staged.undo_.clear();
        const auto clear=[&]{
            vector_indication_detail::clear_marks(staged.data,staged.vector_indication_active,bytes,work,stop);
            auto next=cleared_selection({},selection_carrier_size(staged.region_selection,staged.data.base.width(),staged.data.base.height()));
            next.carrier_dpi=staged.region_selection.carrier_dpi;next.vector_flags=staged.region_selection.vector_flags;
            staged.set_region_selection(std::move(next));staged.vector_indication_active=false;
        };
        const auto* plane=staged.data.active_plane();
        const bool visible=staged.data.current_layer>=0&&std::size_t(staged.data.current_layer)<staged.data.layers.size()
            &&staged.data.layers[std::size_t(staged.data.current_layer)].common.flags[1]&&plane&&plane->common.flags[1];
        if(visible){
            const auto family=plane->family&0xf0000000u;
            std::optional<selection_color_detail::BitmapSelection> bitmap;
            if(family==0x10000000u)bitmap=selection_color_detail::raster_after_admission(staged.data,staged.region_selection,command,reference,additive_space,stop);
            else if(family==0x20000000u){
                const bool add=command==SelectionColorCommand::ColorAdd||(command==SelectionColorCommand::ColorSpace&&additive_space);
                if(!add)clear();
                const auto flags=staged.application_vector_selection_flags.value_or(staged.region_selection.vector_flags);
                if(flags&1u)bitmap=selection_color_detail::vector_after_admission(staged.data,staged.region_selection,command,reference,additive_space,minimum_width,stop);
                else{
                    auto result=selection_color_detail::primitives_after_admission(staged.data,staged.vector_indication_active,staged.region_selection,command,reference,additive_space,stop);
                    staged.data=std::move(result.indications.data);staged.vector_indication_active=result.indications.active;
                    staged.set_region_selection(std::move(result.selection));
                    //Direct primitive publication has no615390 tail and no
                    //content snapshot. Undo/Redo recompute from selection.
                }
            }
            if(bitmap){
                if(bitmap->action==selection_color_detail::BitmapAction::Clear)clear();
                else if(bitmap->action==selection_color_detail::BitmapAction::Refresh){
                    const auto flags=staged.application_vector_selection_flags.value_or(bitmap->selection.vector_flags);
                    const bool mask=bitmap->selection.state==SelectionLayerState::Mask||bitmap->selection.state==SelectionLayerState::VectorIndicated;
                    const auto dpi=mask&&!(flags&1u)?selection_carrier_dpi(bitmap->selection,staged.data.base.dpi):0;
                    staged.vector_indication_active=vector_indication_detail::refresh_marks(staged.data,staged.vector_indication_active,bitmap->selection,flags,dpi,bytes,work,stop);
                    staged.set_region_selection(std::move(bitmap->selection));
                }
            }
        }
        Revision item{DocumentData{},label,staged.revision_,HistorySelection::PreserveCurrent,std::shared_ptr<const CompoundEdit>(std::move(group))};
        history.reserve(history.size()+1);history.push_back(std::move(item));
        staged.undo_=std::move(history);staged.redo_.clear();staged.undo_next_=true;
    });return true;
}
void Document::set_region_selection(RegionSelection selection) {require_no_floating_transform();
    if(application_vector_selection_flags)selection.vector_flags=*application_vector_selection_flags;
    region_selection=std::move(selection);++region_selection_generation_;
}
void Document::select_region(RegionSelection selected,const std::string& label,bool refresh){require_no_floating_transform();
    inherit_selection_carrier(selected,region_selection);
    if(has_floating_selection()){finalize_floating(std::move(selected),label);return;}
    const SelectionIndicationWitness witness(*this);selected.vector_flags=witness.flags();
    auto before=selection_history_snapshot(data,region_selection);
    const auto source=indication_source(data,refresh&&needs_selection_indications(witness.active,selected,witness.flags()));
    auto prepared=refresh?prepare_selection_indications(source,witness.active,selected,witness.flags()):std::nullopt;
    Revision next{DocumentData{},label,revision_,HistorySelection::PreserveCurrent,RegionSelectionEdit{std::move(before),{},{}}};
    // Reserve on the live history, then validate the source. No allocating
    // work or callbacks follow this check and the prepared-state publication.
    undo_.reserve(undo_.size()+1);
    if(!witness.matches(*this)||(source&&!modal_detail::data_same(data,*source)))throw std::logic_error("发布选区指示期间画稿已改变。");
    require_no_floating_transform();
    static_assert(std::is_nothrow_move_constructible_v<Revision>);
    undo_.push_back(std::move(next));
    if(prepared){if(prepared->redraw)std::swap(data,prepared->data);vector_indication_active=prepared->active;}
    trim_history();
    redo_.clear();undo_next_=true;set_region_selection(std::move(selected));++state_generation_;
}
void Document::edit_region_selection(const std::string& label,const std::function<RegionSelection(const DocumentData&,const RegionSelection&)>& operation,const bool* refresh){require_no_floating_transform();
    if(floating_selection_&&floating_selection_->native_clipboard){
        const auto previous=region_selection;
        prepare_selection_command(label,{},[&](Document& staged,std::uint64_t&,std::uint64_t&){
            auto history=std::move(staged.undo_);staged.undo_.clear();staged.redo_.clear();staged.finalize_floating_selection(label);
            auto selected=operation(staged.data,previous);staged.select_region(std::move(selected),label,!refresh||*refresh);
            auto selection_item=std::move(staged.undo_.back());staged.undo_.pop_back();staged.undo_.insert(staged.undo_.begin(),std::move(selection_item));
            auto group=std::make_shared<const CompoundEdit>(CompoundEdit{std::move(staged.undo_),{}});history.push_back({DocumentData{},label,staged.revision_,HistorySelection::PreserveCurrent,std::move(group)});
            staged.undo_=std::move(history);staged.trim_history();staged.redo_.clear();staged.undo_next_=true;
        });return;
    }
    const auto generation=state_generation_,selection_generation=region_selection_generation_;
    const auto floating=floating_selection_;const auto previous=region_selection;const SelectionAdmission admission(data);
    const auto unchanged=[&]{return state_generation_==generation&&region_selection_generation_==selection_generation&&floating_selection_==floating
        &&region_selection==previous&&admission.matches(data);};
    if(!floating){
        auto selected=operation(data,previous);
        if(!unchanged())throw std::logic_error("生成选区期间画稿已改变。");
        select_region(std::move(selected),label,!refresh||*refresh);return;
    }
    if(!floating->target&&!floating->sources&&!floating->same_document_source){
        auto staged=*this;staged.collecting_history_=true;staged.undo_.clear();staged.redo_.clear();staged.undo_next_=true;staged.finalize_foreign_floating({},label);
        auto selected=operation(staged.data,previous);
        if(!unchanged())throw std::logic_error("生成选区期间画稿已改变。");
        staged.select_region(std::move(selected),label,!refresh||*refresh);
        auto group=std::make_shared<const CompoundEdit>(CompoundEdit{std::move(staged.undo_),std::move(staged.redo_)});
        undo_.push_back({DocumentData{},label,revision_,HistorySelection::RestoreSnapshot,std::move(group)});
        trim_history();
        staged.undo_=std::move(undo_);staged.state_generation_=state_generation_+1;staged.history_limit_=history_limit_;staged.collecting_history_=collecting_history_;staged.trim_history();std::swap(*this,staged);return;
    }
    auto materialized=prepare_floating_materialization(data,*floating);
    auto selected=operation(materialized.data,previous);
    if(!unchanged())throw std::logic_error("生成选区期间画稿已改变。");
    publish_floating_materialization(std::move(materialized.data),materialized.content_history,std::move(selected),label,!refresh||*refresh);
}
bool Document::invert_region_selection(const std::string& label){require_no_floating_transform();
    if(!region_selection.active())return false;
    edit_region_selection(label,[](const DocumentData& settled,const RegionSelection& previous){
        return invert_selection(previous,settled.base.width(),settled.base.height());
    });
    return true;
}
bool Document::morph_region_selection(SelectionMorphology mode,int radius_px,const std::string& label,std::stop_token stop){require_no_floating_transform();
    if(region_selection.state==SelectionLayerState::Cleared||region_selection.state==SelectionLayerState::FullPlane)return false;
    edit_region_selection(label,[&](const DocumentData& settled,const RegionSelection& previous){
        return morph_selection(previous,settled.base.width(),settled.base.height(),mode,radius_px,stop);
    });
    return true;
}
bool Document::select_raster_color(SelectionColorCommand command,Pixel reference,const std::string& label,bool additive_space,std::stop_token stop){require_no_floating_transform();
    if(!can_select_raster_color(data,command))return false;
    const auto generation=state_generation_,selection_generation=region_selection_generation_;
    const auto floating=floating_selection_;const auto before=region_selection;const SelectionAdmission admission(data);
    // Prepare history as well as pixels on a private receiver. A selection
    // delivered by allocation during the last history slot remains newer.
    auto staged=*this;
    staged.edit_region_selection(label,[&](const DocumentData& settled,const RegionSelection& previous){
        return retas::select_raster_color(settled,previous,command,reference,additive_space,stop);
    });
    if(stop.stop_requested())throw std::runtime_error("Color selection cancelled");
    if(state_generation_!=generation||region_selection_generation_!=selection_generation||floating_selection_!=floating
        ||region_selection!=before||!admission.matches(data))throw std::logic_error("生成颜色选区期间画稿已改变。");
    static_assert(std::is_nothrow_swappable_v<Document>);
    staged.state_generation_=state_generation_+1;staged.history_limit_=history_limit_;staged.collecting_history_=collecting_history_;staged.trim_history();std::swap(*this,staged);
    return true;
}
bool Document::select_vector_color_bitmap(SelectionColorCommand command,Pixel reference,const std::string& label,bool additive_space,bool minimum_width,std::stop_token stop){require_no_floating_transform();
    if(!can_select_vector_color_bitmap(data,command))return false;
    const auto generation=state_generation_,selection_generation=region_selection_generation_;
    const auto floating=floating_selection_;const auto before=region_selection;const SelectionAdmission admission(data);
    // Prepare history as well as pixels on a private receiver. A selection
    // delivered by allocation during the last history slot remains newer.
    auto staged=*this;
    staged.edit_region_selection(label,[&](const DocumentData& settled,const RegionSelection& previous){
        return retas::select_vector_color_bitmap(settled,previous,command,reference,additive_space,minimum_width,stop);
    });
    if(stop.stop_requested())throw std::runtime_error("Color selection cancelled");
    if(state_generation_!=generation||region_selection_generation_!=selection_generation||floating_selection_!=floating
        ||region_selection!=before||!admission.matches(data))throw std::logic_error("生成颜色选区期间画稿已改变。");
    static_assert(std::is_nothrow_swappable_v<Document>);
    staged.state_generation_=state_generation_+1;staged.history_limit_=history_limit_;staged.collecting_history_=collecting_history_;staged.trim_history();std::swap(*this,staged);
    return true;
}
bool Document::clear_vector_indications(std::stop_token stop){return apply_vector_indications(false,0,stop);}
bool Document::refresh_vector_indications(double carrier_dpi,std::stop_token stop){return apply_vector_indications(true,carrier_dpi,stop);}
bool Document::apply_vector_indications(bool refresh,double carrier_dpi,std::stop_token stop){require_no_floating_transform();
    if(!refresh&&!vector_indication_active)return false;
    const auto generation=state_generation_,selection_generation=region_selection_generation_;
    const auto before=region_selection;const auto flags=application_vector_selection_flags;const auto active=vector_indication_active;
    const auto floating=floating_selection_;std::uint64_t metadata{};modal_detail::data_charge(data,metadata);
    if(metadata>512ull*1024*1024/3)throw std::length_error("Vector indication document snapshot exceeds storage limit");
    //Vector payloads and metadata are owning copies. Untouched raster tiles
    //stay COW-shared; in-place borrowed pixel edits remain caller-serialized.
    auto witness=data;
    auto prepared=refresh?prepare_vector_indication_refresh(witness,active,before,flags.value_or(before.vector_flags),carrier_dpi,stop):prepare_vector_indication_clear(witness,active,stop);
    if(stop.stop_requested())throw std::runtime_error("Vector indication cancelled");
    if(generation!=state_generation_||selection_generation!=region_selection_generation_||region_selection!=before||application_vector_selection_flags!=flags
        ||vector_indication_active!=active||floating_selection_!=floating||!modal_detail::data_same(data,witness))throw std::logic_error("刷新矢量指示期间画稿已改变。");
    if(!prepared.redraw)return false;
    static_assert(std::is_nothrow_swappable_v<DocumentData>);std::swap(data,prepared.data);vector_indication_active=prepared.active;++state_generation_;return true;
}
bool Document::begin_floating_selection_move(const std::string& label){require_no_floating_transform();
    return begin_floating_selection_move(label,{});
}
bool Document::begin_floating_selection_move(const std::string& label,const FloatingPreviewValidator& validate){require_no_floating_transform();
    return begin_floating_capture(label,validate,false);
}
bool Document::begin_floating_capture(const std::string& label,const FloatingPreviewValidator& validate,bool command,bool current_only){require_no_floating_transform();
    if(!command&&!region_selection.active())return false;
    const SelectionIndicationWitness witness(*this);const auto manager_before=paint_history_manager_;
    const auto revision=revision_,saved=saved_,next=next_;const auto undo_size=undo_.size(),redo_size=redo_.size();const bool direction=undo_next_;
    const auto undo_id=undo_.empty()?0:undo_.back().id,redo_id=redo_.empty()?0:redo_.back().id;
    const auto original=data.original;const auto content=data.original_content;const auto raw=raw_baseline_;
    raster_history_detail::Budget local_budget;auto& budget=selection_command_budget_?*selection_command_budget_:local_budget;modal_detail::data_charge(data,budget.bytes);const auto frozen=modal_detail::frozen(data,{});
    modal_detail::data_charge(frozen,budget.bytes,0,false);auto source=frozen;source.original=original;source.original_content=content;
    auto selected=witness.selection;auto frozen_selection=selected;
    if(selected.mask){const auto& mask=*selected.mask;budget.allocate(((std::uint64_t(mask.width())+31)/32)*mask.height()*4+((std::uint64_t(mask.width())+255)/256)*((std::uint64_t(mask.height())+255)/256));frozen_selection.mask=std::make_shared<const BinaryMask>(mask);}
    auto before=selection_history_snapshot(source,selected);
    selected.vector_flags=witness.flags();selected.carrier_size=selection_carrier_size(selected,source.base.width(),source.base.height());
    if(selected.state==SelectionLayerState::FullPlane&&!selected.mask){
        budget.allocate(((std::uint64_t(selected.carrier_size[0])+31)/32)*selected.carrier_size[1]*4+((std::uint64_t(selected.carrier_size[0])+255)/256)*((std::uint64_t(selected.carrier_size[1])+255)/256));
        auto mask=std::make_shared<BinaryMask>(selected.carrier_size[0],selected.carrier_size[1]);const auto r=selected.bounds;
        const int left=int(std::clamp<std::int64_t>(r.x,0,mask->width())),top=int(std::clamp<std::int64_t>(r.y,0,mask->height()));
        const int right=int(std::clamp<std::int64_t>(std::int64_t(r.x)+std::max(0,r.width),left,mask->width()));
        const int bottom=int(std::clamp<std::int64_t>(std::int64_t(r.y)+std::max(0,r.height),top,mask->height()));
        budget.visit(std::uint64_t(right-left)*(bottom-top));for(int y=top;y<bottom;++y){budget.poll();for(int x=left;x<right;++x)mask->set(x,y);}selected.mask=std::move(mask);
    }
    budget.allocate(sizeof(CompoundEdit));auto group=std::make_shared<CompoundEdit>();
    group->past.push_back({DocumentData{},label,revision,HistorySelection::PreserveCurrent,RegionSelectionEdit{std::move(before),{},{}}});
    auto manager=manager_before;std::optional<floating_detail::Content> prepared_capture;
    if(witness.floating){
        if(validate)validate(source,witness.floating);
        FloatingSelectionEdit edit;edit.mode=FloatingSelectionEdit::Mode::Offset;edit.exchange_data=false;edit.primary_offset=witness.floating->delta;
        group->past.push_back({DocumentData{},label,revision,HistorySelection::PreserveCurrent,std::move(edit)});
    }else{
        prepared_capture=floating_detail::capture_content(source,selected,budget.bytes,budget.work,budget.stop,current_only);
        auto& capture=prepared_capture->capture;const auto layout=raster_history_detail::Edit::identity_layout(source,budget);
        std::size_t paint_index{};
        for(const auto& target:prepared_capture->sources){const auto& owner=source.layers.at(target.layer);
            manager=ensure_paint_manager(manager,source,target.layer,false,budget,layout);
            if(owner.family==0x21200000){
                while(paint_index<capture.paint_source_history.size()&&capture.paint_source_history[paint_index].layer<target.layer)++paint_index;
                if(paint_index==capture.paint_source_history.size()||capture.paint_source_history[paint_index].layer!=target.layer)continue;
                const auto bound=with_paint_budget(budget,[&](auto& paint){return manager->manager->bind(capture.remainder.layers[target.layer],capture.paint_source_history[paint_index].raw_damage,paint);});
                budget.allocate(sizeof(CaptureSourceEdit));auto edit=std::make_shared<CaptureSourceEdit>();edit->owner=owner.common.runtime_identity;edit->family=owner.family;edit->hint=target.layer;edit->layout=layout;edit->paint=bound.edit;
                capture.remainder.layers[target.layer]=bound.source;manager=replace_paint_manager(manager,bound.manager,budget);
                group->past.push_back({DocumentData{},label,revision,HistorySelection::PreserveCurrent,std::shared_ptr<const CaptureSourceEdit>(std::move(edit))});
            }else if(!target.planes.empty()){
                invalidate_select_paint_manager(manager,owner);
                const bool whole=bool(owner.family&0x20000000u)||selection_detail::extension_move_owner(owner);
                if(whole){budget.allocate(sizeof(CaptureSourceEdit));auto edit=std::make_shared<CaptureSourceEdit>();edit->owner=owner.common.runtime_identity;edit->family=owner.family;edit->hint=target.layer;edit->layout=layout;edit->storage=capture_source_storage(owner,budget);edit->whole_owner=true;
                    for(std::size_t p=0;p<owner.planes.size();++p)edit->planes.push_back(p);
                    group->past.push_back({DocumentData{},label,revision,HistorySelection::PreserveCurrent,std::shared_ptr<const CaptureSourceEdit>(std::move(edit))});
                }else for(auto index:target.planes){const auto& plane=owner.planes.at(index);
                    if(plane.raster&&plane.raster->can_edit()){
                        const auto roi=selected.state==SelectionLayerState::FullPlane?Rect{0,0,plane.raster->width,plane.raster->height}:raster_history_detail::capture_roi(owner,plane,selected);
                        budget.allocate(sizeof(raster_history_detail::Edit)+sizeof(raster_history_detail::Edit::PlaneState));auto edit=std::make_shared<raster_history_detail::Edit>();edit->layout=layout;
                        edit->planes.push_back({raster_history_detail::target(source,target.layer,index),roi,raster_history_detail::snapshot(*plane.raster,roi,budget),{}});
                        group->past.push_back({DocumentData{},label,revision,HistorySelection::PreserveCurrent,std::shared_ptr<const raster_history_detail::Edit>(std::move(edit))});
                    }else{budget.allocate(sizeof(CaptureSourceEdit));auto edit=std::make_shared<CaptureSourceEdit>();edit->owner=owner.common.runtime_identity;edit->family=owner.family;edit->hint=target.layer;edit->layout=layout;edit->storage=capture_source_storage(owner,budget);edit->planes={index};
                        group->past.push_back({DocumentData{},label,revision,HistorySelection::PreserveCurrent,std::shared_ptr<const CaptureSourceEdit>(std::move(edit))});}
                }
            }
        }
        floating_detail::finish_capture(capture,selected,witness.active,budget.bytes,budget.work);
        if(capture.floating){
            FloatingSelectionEdit edit;edit.exchange_data=false;edit.source_history=capture.source_history;edit.binding=raster_history_detail::FloatingBinding::capture(source,*capture.floating,budget);
            group->past.push_back({DocumentData{},label,revision,HistorySelection::PreserveCurrent,std::move(edit)});
            if(validate)validate(capture.remainder,capture.floating);
        }
    }
    const bool has_payload=witness.floating||(prepared_capture&&prepared_capture->capture.floating);
    Revision item{DocumentData{},label,revision,HistorySelection::PreserveCurrent,std::shared_ptr<const CompoundEdit>(std::move(group))};
    std::vector<Revision> prepared_undo;prepared_undo.reserve(undo_size+1);
    if(!witness.matches(*this)||revision_!=revision||saved_!=saved||next_!=next||undo_next_!=direction||undo_.size()!=undo_size||redo_.size()!=redo_size
        ||(!undo_.empty()&&undo_.back().id!=undo_id)||(!redo_.empty()&&redo_.back().id!=redo_id)||paint_history_manager_!=manager_before
        ||data.original!=original||data.original_content!=content||raw_baseline_!=raw||!modal_detail::data_same(data,frozen)||!raster_history_detail::same_identity_layout(data,frozen)||!modal_detail::selection_same(region_selection,frozen_selection))throw std::logic_error("预览浮动图像期间画稿已改变。");
    require_no_floating_transform();
    static_assert(std::is_nothrow_move_constructible_v<Revision>);
    for(auto& old:undo_)prepared_undo.push_back(std::move(old));prepared_undo.push_back(std::move(item));undo_.swap(prepared_undo);
    if(prepared_capture){auto& capture=prepared_capture->capture;
        if(capture.source_history)runtime_dirty_=true;
        data=std::move(capture.remainder);vector_indication_active=capture.indication_active;paint_history_manager_=std::move(manager);
        if(capture.floating){floating_selection_=std::move(capture.floating);floating_same_document_source_=floating_selection_->same_document_source;++floating_selection_generation_;}
    }
    trim_history();redo_.clear();undo_next_=true;
    set_region_selection(std::move(selected));++state_generation_;return has_payload;
}
void Document::move_floating_selection(int dx,int dy){require_no_floating_transform();
    move_floating_selection(dx,dy,{});
}
void Document::move_floating_selection(int dx,int dy,const FloatingPreviewValidator& validate){require_no_floating_transform();
    if(!floating_selection_)throw std::logic_error("没有可移动的浮动图像。");
    const auto generation=state_generation_,selection_generation=region_selection_generation_;const auto original=floating_selection_;
    const auto previous=region_selection;const SelectionAdmission admission(data);
    auto selected=move_selection(region_selection,data.base.width(),data.base.height(),dx,dy);
    const auto x=std::int64_t(floating_selection_->delta[0])+dx,y=std::int64_t(floating_selection_->delta[1])+dy;
    if(x<std::numeric_limits<int>::min()||x>std::numeric_limits<int>::max()||y<std::numeric_limits<int>::min()||y>std::numeric_limits<int>::max())
        throw std::invalid_argument("浮动图像移动超出支持范围。");
    auto floating=std::make_shared<FloatingSelection>(*floating_selection_);floating->delta={int(x),int(y)};
    if(floating->target)validate_targeted_floating(data,*floating);
    else if(!floating->sources&&!floating->same_document_source)validate_foreign_floating(data,*floating);
    if(validate)validate(data,floating);
    if(state_generation_!=generation||region_selection_generation_!=selection_generation||floating_selection_!=original
        ||region_selection!=previous||!admission.matches(data))
        throw std::logic_error("预览浮动图像移动期间画稿已改变。");
    floating_selection_=std::move(floating);++floating_selection_generation_;set_region_selection(std::move(selected));++state_generation_;
}
bool Document::paste_targeted_floating(const FloatingSelection& exported,FloatingTarget target,const std::string& label,const FloatingPreviewValidator& validate){return paste_foreign_floating(exported,label,validate,&target);}
bool Document::paste_foreign_floating(const FloatingSelection& exported,const std::string& label,const FloatingPreviewValidator& validate,const FloatingTarget* target){return paste_foreign_floating(exported,label,PasteSelection::SourceFrame,validate,target);}
bool Document::paste_targeted_floating(const FloatingSelection& exported,FloatingTarget target,const std::string& label,PasteSelection selection,const FloatingPreviewValidator& validate){return paste_foreign_floating(exported,label,selection,validate,&target);}
bool Document::paste_foreign_floating(const FloatingSelection& exported,const std::string& label,PasteSelection selection,const FloatingPreviewValidator& validate,const FloatingTarget* target){require_no_floating_transform();
    if(selection!=PasteSelection::SourceFrame&&selection!=PasteSelection::Content)throw std::invalid_argument("粘贴选区方式无效。");
    // Explicit foreign entry: the caller has resolved cross-document identity.
    // Source-object references in exported data are not adopted as local maps.
    const auto generation=state_generation_,selection_generation=region_selection_generation_;
    const auto original=floating_selection_;const auto previous=region_selection;const SelectionAdmission admission(data);
    auto staged=*this;staged.collecting_history_=true;staged.undo_.clear();staged.redo_.clear();staged.undo_next_=true;staged.finalize_floating_selection(label);
    const bool shrink=selection==PasteSelection::Content;
    auto imported=target?prepare_targeted_paste(staged.data,exported,*target,shrink,staged.region_selection):prepare_plain_paste(staged.data,exported,shrink,staged.region_selection);
    staged.select_region(std::move(imported.selection),label);
    if(imported.floating){
        FloatingSelectionEdit edit;edit.exchange_data=false;edit.foreign_payload=true;
        staged.undo_.push_back({DocumentData{},label,staged.revision_,HistorySelection::PreserveCurrent,std::move(edit)});
        // Foreign items retain false provenance while earlier local snapshots
        // in the group keep the receiver's existing history provenance.
        staged.floating_selection_=std::move(imported.floating);++staged.floating_selection_generation_;
        if(validate)validate(staged.data,staged.floating_selection_);
    }
    if(state_generation_!=generation||region_selection_generation_!=selection_generation||floating_selection_!=original
        ||region_selection!=previous||!admission.matches(data))throw std::logic_error("预览粘贴期间画稿已改变。");
    auto group=std::make_shared<const CompoundEdit>(CompoundEdit{std::move(staged.undo_),std::move(staged.redo_)});
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::RestoreSnapshot,std::move(group)});
    trim_history();
    staged.undo_=std::move(undo_);staged.state_generation_=state_generation_+1;
    static_assert(std::is_nothrow_swappable_v<Document>);staged.history_limit_=history_limit_;staged.collecting_history_=collecting_history_;staged.trim_history();std::swap(*this,staged);return true;
}
void Document::finalize_floating_selection(const std::string& label){finalize_floating({},label);}
void Document::discard_floating_selection(const std::string& label){require_no_floating_transform();
    if(!floating_selection_)return;
    // Clone the source-side remainder before publishing the same mode-1
    // exchange used by finalization. Deliberately skip raster writeback.
    publish_floating_materialization(data,false,{},label);
}
bool Document::cut_selection(std::optional<DocumentData> remainder,const std::string& label){require_no_floating_transform();
    if(floating_selection_)throw std::logic_error("浮动图像必须使用独立剪切历史。");
    if(!remainder&&!region_selection.active())return false;
    if(remainder)require_selection_layout(data,*remainder,HistorySelection::PreserveCurrent);
    auto staged=*this;staged.collecting_history_=true;staged.undo_.clear();staged.redo_.clear();staged.undo_next_=true;
    if(remainder)staged.transact(label,[&](DocumentData& target){std::swap(target,*remainder);},HistorySelection::PreserveCurrent);
    if(staged.region_selection.active()){
        const auto size=selection_carrier_size(staged.region_selection,staged.data.base.width(),staged.data.base.height());
        staged.select_region(cleared_selection({},size),label);
    }
    auto group=std::make_shared<const CompoundEdit>(CompoundEdit{std::move(staged.undo_),std::move(staged.redo_)});
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(group)});
    trim_history();
    staged.undo_=std::move(undo_);staged.state_generation_=state_generation_+1;
    static_assert(std::is_nothrow_swappable_v<Document>);staged.history_limit_=history_limit_;staged.collecting_history_=collecting_history_;staged.trim_history();std::swap(*this,staged);return true;
}
void Document::finalize_floating(std::optional<RegionSelection> selected,const std::string& label,bool prepared){require_no_floating_transform();
    if(!prepared){if(!floating_selection_)return;prepare_selection_command(label,{},[&](Document& staged,std::uint64_t&,std::uint64_t&){staged.finalize_floating(selected,label,true);});return;}
    if(!floating_selection_)return;
    if(floating_selection_->native_clipboard){finalize_clipboard_floating(std::move(selected),label);return;}
    if(!floating_selection_->target&&!floating_selection_->sources&&!floating_selection_->same_document_source){finalize_foreign_floating(std::move(selected),label);return;}
    auto materialized=prepare_floating_materialization(data,*floating_selection_);
    publish_floating_materialization(std::move(materialized.data),materialized.content_history,std::move(selected),label);
}
void Document::finalize_clipboard_floating(std::optional<RegionSelection> selected,const std::string& label){
    const auto clipboard_coordinate=[](std::int64_t n){if(n<INT32_MIN||n>INT32_MAX)throw std::invalid_argument("Clipboard coordinate overflow");return int(n);};
    const auto original=floating_selection_;auto staged=*this;staged.building_clipboard_group_=true;staged.undo_.clear();staged.redo_.clear();staged.floating_selection_.reset();
    auto mappings=std::make_shared<std::vector<FloatingSource>>();auto lowered=std::make_shared<std::vector<Layer>>();
    for(std::size_t i=0;i<original->layers->size();++i){const auto& source=original->layers->at(i);const auto* identity=original->clipboard_local&&original->clipboard_sources&&i<original->clipboard_sources->size()?&original->clipboard_sources->at(i):nullptr;
        std::optional<std::size_t> owner_index;const bool text=source.family==0x40000800u||source.family==0x48040800u;
        if(text){auto added=source;added.common.runtime_identity=next_element_identity();added.common.x=clipboard_coordinate(std::int64_t(added.common.x)+original->delta[0]);added.common.y=clipboard_coordinate(std::int64_t(added.common.y)+original->delta[1]);for(auto& p:added.planes){p.common.runtime_identity=next_element_identity();p.runtime_text_cache.reset();}staged.insert_layer(staged.data.layers.size(),std::move(added),label);continue;}
        if(identity&&!text)for(std::size_t l=0;l<staged.data.layers.size();++l)if(staged.data.layers[l].common.runtime_identity==identity->owner&&staged.data.layers[l].family==source.family){if(owner_index)throw std::logic_error("Duplicate clipboard source owner identity");owner_index=l;}
        const bool force_layer=source.family==0x10000100u||source.family==0x10008000u||((source.family&0xf0000000u)==0x40000000u&&source.family!=0x40001000u);
        if(!owner_index&&!force_layer)for(std::size_t l=0;l<staged.data.layers.size();++l)if(staged.data.layers[l].family==source.family){owner_index=l;break;}
        if(!owner_index){
            DocumentData payload;payload.layers={source};if(staged.selection_command_budget_)modal_detail::data_charge(payload,staged.selection_command_budget_->bytes);
            auto added=modal_detail::frozen(payload,{}).layers.front();added.runtime_vector_face_colors.reset();added.common.runtime_identity=next_element_identity();added.common.x=clipboard_coordinate(std::int64_t(added.common.x)+original->delta[0]);added.common.y=clipboard_coordinate(std::int64_t(added.common.y)+original->delta[1]);
            for(auto& child:added.planes){child.common.runtime_identity=next_element_identity();child.runtime_text_cache.reset();}
            staged.insert_layer(staged.data.layers.size(),std::move(added),label);continue;
        }
        if(!staged.data.layers[*owner_index].common.runtime_operable)throw std::invalid_argument("Clipboard destination owner is not runtime-operable");
        FloatingSource mapping{*owner_index,{}};
        for(std::size_t p=0;p<source.planes.size();++p){const auto& incoming=source.planes[p];auto& owner=staged.data.layers[*owner_index];std::optional<std::size_t> target;
            if(identity&&p<identity->planes.size())for(std::size_t q=0;q<owner.planes.size();++q)if(owner.planes[q].common.runtime_identity==identity->planes[p]&&owner.planes[q].family==incoming.family){if(target)throw std::logic_error("Duplicate clipboard source plane identity");target=q;}
            const bool reuse=(incoming.family&0xf0f00000u)==0x10100000u||incoming.family==0x11410001u||incoming.family==0x12400001u||incoming.family==0x24100001u||incoming.family==0x22100002u||incoming.family==0x40000004u||incoming.family==0x40001008u;
            if(!target&&reuse)for(std::size_t q=0;q<owner.planes.size();++q)if(owner.planes[q].family==incoming.family){target=q;break;}
            if(!target){Plane added;if(incoming.family==0x40000001u||incoming.family==0x40000002u){added.common=owner.common;added.family=incoming.family;added.common.runtime_identity=next_element_identity();added.common.name=incoming.common.name;added.common.width=added.common.height=added.common.depth=0;added.common.position={Length{0,-1,0},Length{0,-1,0}};added.common.field_14=-1;added.common.suffix.clear();added.common.opacity_mode=1;Writer w,b,t,l;t.i32(0);t.i32(0);b.section(t.data);l.i32(0);if(incoming.family==0x40000001u){b.section(l.data);w.section(b.data);}else{b.i32(0);w.section(b.data);w.section(l.data);Writer f;f.i32(0);f.i32(0);f.i32(0);w.section(f.data);}w.i32(0);added.specific=std::move(w.data);}else added=make_plane(owner,incoming.family,incoming.common.name,incoming.common.color);added.common.flags=incoming.common.flags;added.common.runtime_operable=incoming.common.runtime_operable;added.common.opacity=incoming.common.opacity;added.common.color=incoming.common.color;added.common.x=incoming.common.x;added.common.y=incoming.common.y;added.common.resolution=incoming.common.resolution;
                if(added.raster){added.common.depth=incoming.common.depth;added.raster.emplace(added.common.width,added.common.height,added.common.depth,added.common.resolution);}
                target=owner.planes.size();staged.insert_plane(*owner_index,*target,std::move(added),label);}
            mapping.planes.push_back(*target);
        }mappings->push_back(std::move(mapping));lowered->push_back(source);
    }
    auto local=std::make_shared<FloatingSelection>(*original);local->native_clipboard=false;local->same_document_source=true;local->layers=std::move(lowered);local->sources=std::move(mappings);staged.floating_selection_=local;staged.floating_same_document_source_=true;
    auto materialized=local->layers->empty()?FloatingMaterialization{staged.data,false}:prepare_floating_materialization(staged.data,*local);staged.publish_floating_materialization(std::move(materialized.data),materialized.content_history,std::move(selected),label);
    //Mode1 restores the resident import, while the preceding topology/content
    //children restore its destinations independently.
    auto outer=std::get<std::shared_ptr<const CompoundEdit>>(staged.undo_.back().runtime);auto contents=std::make_shared<CompoundEdit>(*outer);
    auto& floating=std::get<FloatingSelectionEdit>(contents->past.back().runtime);floating.opposite=original;floating.foreign_payload=true;floating.binding.reset();
    staged.undo_.back().runtime=std::shared_ptr<const CompoundEdit>(std::move(contents));
    auto group=std::make_shared<const CompoundEdit>(CompoundEdit{std::move(staged.undo_),{}});undo_.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(group)});trim_history();
    staged.undo_=std::move(undo_);staged.state_generation_=state_generation_+1;staged.floating_same_document_source_=false;staged.building_clipboard_group_=building_clipboard_group_;staged.history_limit_=history_limit_;staged.collecting_history_=collecting_history_;staged.trim_history();std::swap(*this,staged);
}
void Document::finalize_foreign_floating(std::optional<RegionSelection> selected,const std::string& label){require_no_floating_transform();
    auto added=prepare_pasted_layers(data,*floating_selection_);
    if(added.size()>std::size_t(std::numeric_limits<int>::max())-data.layers.size())throw std::invalid_argument("粘贴图层数量超出支持范围。");
    auto staged=*this;staged.collecting_history_=true;staged.undo_.clear();staged.redo_.clear();staged.undo_next_=true;
    staged.data.layers.reserve(staged.data.layers.size()+added.size());staged.undo_.reserve(added.size()+1);
    // One typed item per appended layer; do not use insert_layer's public
    // 100-entry trimming inside this single (potentially larger) compound.
    for(auto& layer:added){
        layer.common.runtime_identity=next_element_identity();
        for(auto& plane:layer.planes)plane.common.runtime_identity=next_element_identity();
        const auto index=staged.data.layers.size();
        staged.undo_.push_back({DocumentData{},label,staged.revision_,HistorySelection::RestoreSnapshot,AddLayerEdit{index,{}}});
        // Match AddLayer reinsertion's live +0x28 state on first publication.
        layer.common.flags[1]=1;
        staged.data.layers.push_back(std::move(layer));select_inserted_layer(staged.data,index);++staged.document_modify_count_;
    }
    FloatingSelectionEdit edit;edit.mode=FloatingSelectionEdit::Mode::Finalize;edit.exchange_data=false;edit.foreign_payload=true;
    // Preserve the foreign capture frame only as rebuild runtime validation
    // side information. Local/native history snapshots still omit that frame.
    edit.opposite=std::make_shared<FloatingSelection>(*floating_selection_);
    if(selected){inherit_selection_carrier(*selected,region_selection);
        edit.selection=RegionSelectionEdit{selection_history_snapshot(data,region_selection),{},{}};}
    staged.undo_.push_back({DocumentData{},label,staged.revision_,HistorySelection::PreserveCurrent,std::move(edit)});
    staged.floating_selection_.reset();++staged.floating_selection_generation_;if(selected)staged.set_region_selection(std::move(*selected));
    raster_history_detail::Budget local_budget;auto& budget=staged.selection_command_budget_?*staged.selection_command_budget_:local_budget;staged.paint_history_manager_=prepare_paint_manager(staged.paint_history_manager_,staged.data,false,budget);
    auto group=std::make_shared<const CompoundEdit>(CompoundEdit{std::move(staged.undo_),std::move(staged.redo_)});
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::RestoreSnapshot,std::move(group)});
    trim_history();
    staged.undo_=std::move(undo_);staged.state_generation_=state_generation_+1;staged.history_limit_=history_limit_;staged.collecting_history_=collecting_history_;staged.trim_history();std::swap(*this,staged);
}
void Document::publish_floating_materialization(DocumentData candidate,bool content_history,std::optional<RegionSelection> selected,const std::string& label,bool refresh_selection){require_no_floating_transform();
    const SelectionIndicationWitness witness(*this);raster_history_detail::Budget local_budget;auto& budget=selection_command_budget_?*selection_command_budget_:local_budget;
    modal_detail::data_charge(data,budget.bytes);modal_detail::data_charge(candidate,budget.bytes);
    const auto frozen=modal_detail::frozen(data,{});const auto layout=raster_history_detail::Edit::identity_layout(data,budget);
    auto group=std::make_shared<CompoundEdit>();auto manager=paint_history_manager_;
    if(selected){inherit_selection_carrier(*selected,region_selection);group->past.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,RegionSelectionEdit{selection_history_snapshot(data,region_selection),{},{}}});}
    const auto& floating=*floating_selection_;
    std::map<std::size_t,std::set<std::size_t>> targets;
    if(content_history){
        if(floating.target)targets[floating.target->layer].insert(floating.target->plane);
        else if(floating.sources)for(const auto& mapping:*floating.sources)targets[mapping.layer].insert(mapping.planes.begin(),mapping.planes.end());
        else throw std::logic_error("Floating materialization has no content targets");
    }
    for(const auto& [index,planes]:targets){const auto& owner=data.layers.at(index);auto& after=candidate.layers.at(index);
        budget.visit(data.layers.size()+owner.planes.size());
        if(owner.family&0x10000000u){
            const bool geometry=owner.common.x!=after.common.x||owner.common.y!=after.common.y||owner.common.width!=after.common.width||owner.common.height!=after.common.height;
            auto edit=std::make_shared<raster_history_detail::Edit>();edit->layout=layout;
            if(geometry){edit->primary_geometry=raster_history_detail::Edit::Geometry{owner.common.x,owner.common.y,owner.common.width,owner.common.height};
                for(std::size_t p=0;p<owner.planes.size();++p)if(owner.planes[p].raster)edit->planes.push_back({raster_history_detail::target(data,index,p),{},raster_history_detail::copy_all(*owner.planes[p].raster,budget),{}});
            }else for(auto p:planes){const auto& plane=owner.planes.at(p);if(!plane.raster)throw std::logic_error("Floating raster target lost its backing");
                Rect roi{};bool found{},whole{};
                for(std::size_t i=0;i<floating.layers->size();++i){const auto& source=floating.layers->at(i);
                    if(!floating.target&&floating.sources->at(i).layer!=index)continue;
                    for(std::size_t q=0;q<source.planes.size();++q){if(!floating.target&&floating.sources->at(i).planes.at(q)!=p)continue;
                        if(!source.planes[q].raster){whole=true;continue;}
                        for(const auto& [tile,bytes]:source.planes[q].raster->tiles){(void)bytes;budget.visit(1);
                            const auto factor=floating.target&&(plane.family&0x00f00000u)==0x00200000u?std::trunc(plane.common.resolution/owner.common.resolution):1.;
                            const auto offset=[&](int coordinate,int delta,int origin){const double value=(double(coordinate)+delta-origin)*factor;if(!std::isfinite(value)||value<INT32_MIN||value>INT32_MAX)throw std::invalid_argument("Floating history placement overflows");return std::int64_t(value);};
                            const auto dx=offset(source.common.x,floating.delta[0],owner.common.x),dy=offset(source.common.y,floating.delta[1],owner.common.y);
                            const auto x=dx+tile.x*256ll,y=dy+tile.y*256ll;
                            if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX)throw std::invalid_argument("Floating history ROI overflows");
                            const int w=std::min(256,source.planes[q].raster->width-tile.x*256),h=std::min(256,source.planes[q].raster->height-tile.y*256);
                            const bool cursor=dx%256&&(x+w)%256==0&&x+w>=0&&x+w<plane.raster->width&&(plane.raster->depth>=32||plane.common.opacity_mode==4);
                            const auto r=raster_history_detail::clipped({int(x),int(y),w+int(cursor),h},*plane.raster);
                            if(r.width<=0||r.height<=0)continue;if(!found){roi=r;found=true;}else{const int right=std::max(roi.x+roi.width,r.x+r.width),bottom=std::max(roi.y+roi.height,r.y+r.height);roi.x=std::min(roi.x,r.x);roi.y=std::min(roi.y,r.y);roi.width=right-roi.x;roi.height=bottom-roi.y;}
                        }
                    }
                }
                if(whole)roi={0,0,plane.raster->width,plane.raster->height};else if(!found)continue;
                edit->planes.push_back({raster_history_detail::target(data,index,p),roi,raster_history_detail::snapshot(*plane.raster,roi,budget),{}});
            }
            if(!edit->planes.empty())group->past.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::shared_ptr<const raster_history_detail::Edit>(std::move(edit))});
        }else{
            auto edit=std::make_shared<CaptureSourceEdit>();edit->owner=owner.common.runtime_identity;edit->family=owner.family;edit->hint=index;edit->layout=layout;
            if(!ordinary_pure_history_owner(owner)&&(owner.family==0x21200000u||owner.family==0x22100000u||owner.family==0x24400000u||owner.family==0x40002000u)){
                const bool draw=owner.family!=0x21200000u&&owner.family!=0x40002000u;
                manager=ensure_paint_manager(manager,data,index,false,budget,layout,draw);
                auto bound=with_paint_budget(budget,[&](auto& b){return draw?manager->manager->bind_draw(owner,after,{}, {},b):manager->manager->bind(after,{},b);});
                edit->paint=std::move(bound.edit);after=std::move(bound.source);manager=replace_paint_manager(manager,std::move(bound.manager),budget);
            }else{edit->storage=capture_source_storage(owner,budget);edit->whole_owner=true;for(std::size_t p=0;p<owner.planes.size();++p)edit->planes.push_back(p);
                invalidate_select_paint_manager(manager,owner);
                // VecDraw clears and restores Vert lists only. A Pure child
                // needs the owning pool/list snapshot to make paste reversible.
                if(ordinary_pure_history_owner(owner))manager=ensure_paint_manager(manager,candidate,index,true,budget,layout,true);
            }
            group->past.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::shared_ptr<const CaptureSourceEdit>(std::move(edit))});
        }
    }
    FloatingSelectionEdit edit;edit.mode=FloatingSelectionEdit::Mode::Finalize;edit.exchange_data=false;edit.source_history=content_history;
    edit.foreign_payload=!floating.target&&!floating.sources&&!floating.same_document_source;
    edit.opposite=edit.foreign_payload?std::make_shared<FloatingSelection>(floating):floating_history_snapshot(floating);
    if(floating.sources)edit.binding=raster_history_detail::FloatingBinding::capture(data,floating,budget);
    group->past.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(edit)});
    bool active=vector_indication_active;
    if(selected&&refresh_selection){const auto flags=application_vector_selection_flags.value_or(selected->vector_flags);const bool mask=selected->state==SelectionLayerState::Mask||selected->state==SelectionLayerState::VectorIndicated;
        active=vector_indication_detail::refresh_marks(candidate,active,*selected,flags,mask&&!(flags&1u)?selection_carrier_dpi(*selected,data.base.dpi):0,budget.bytes,budget.work,{});}
    manager=prepare_paint_manager(manager,candidate,false,budget);
    Revision item{DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::shared_ptr<const CompoundEdit>(std::move(group))};std::vector<Revision> prepared;prepared.reserve(undo_.size()+1);
    if(!witness.matches(*this)||!modal_detail::data_same(data,frozen))throw std::logic_error("Floating content changed during history preparation");
    for(auto& old:undo_)prepared.push_back(std::move(old));prepared.push_back(std::move(item));undo_.swap(prepared);trim_history();redo_.clear();undo_next_=true;
    data=std::move(candidate);vector_indication_active=active;floating_selection_.reset();++floating_selection_generation_;if(selected)set_region_selection(std::move(*selected));
    paint_history_manager_=std::move(manager);if(content_history)runtime_dirty_=true;++state_generation_;
}
void Document::begin_text_preview(std::uint64_t owner){require_no_floating_transform();if(!owner)throw std::invalid_argument("Invalid text preview owner");text_preview_owner_=owner;}
void Document::require_no_floating_transform() const {if(has_text_preview())throw std::logic_error("Text preview owns this document; accept or cancel first");if(has_vanishing_point_move())throw std::logic_error("Vanishing Move owns this document; accept or cancel first");if(has_frame_move())throw std::logic_error("Frame Move owns this document; accept or cancel first");if(has_floating_transform())throw std::logic_error("Floating Transform owns this document; accept or cancel first");}
void Document::require_current_floating_transform() const {if(!floating_transform_)throw std::logic_error("No active Floating Transform");if(!floating_transform_->matches(*this))throw std::logic_error("Floating Transform source or runtime changed");}
FloatingTransformBegin Document::begin_floating_transform(FloatingTransformOperation operation,std::uint64_t origin,std::stop_token stop){
    require_no_floating_transform();const SelectionIndicationWitness witness(*this);floating_transform_preparing_=true;struct ClearPreparing{bool& value;~ClearPreparing(){value=false;}} clear{floating_transform_preparing_};FloatingTransformBegin result;std::shared_ptr<FloatingTransformSession> session;
    try{session=FloatingTransformSession::prepare(*this,operation,origin,stop,result);}catch(const std::invalid_argument&){return FloatingTransformBegin::Unsupported;}
    if(!session)return result;if(!witness.matches(*this)||!session->matches(*this))throw std::logic_error("Transform source changed during staged capture");
    if(result==FloatingTransformBegin::NoUsablePayload){
        auto candidate=session->remainder();auto baseline=session->raw_baseline();
        if(session->before().original_content&&session->before().original_content->original==session->before().original)baseline=candidate.original_content;
        if(!witness.matches(*this)||!session->matches(*this)||stop.stop_requested())throw std::logic_error("Transform source changed before failed-capture publication");
        if(session->failed_capture_changed()){
            data=std::move(candidate);raw_baseline_=std::move(baseline);vector_indication_active=session->capture_indication_active();
            if(session->failed_capture_source_history())runtime_dirty_=true;
            ++state_generation_;
        }
        return result;
    }
    const auto admission=std::shared_ptr<FloatingTransformSession>(new FloatingTransformSession(session->impl_));auto candidate=session->remainder();auto floating=session->live_floating();auto selected=session->selection_live();auto baseline=session->raw_baseline();
    session->bind(*this,candidate,selected,floating,{state_generation_+1,region_selection_generation_+1,floating_selection_generation_+1},stop);
    if(session->before().original_content&&session->before().original_content->original==session->before().original)baseline=candidate.original_content;
    if(!witness.matches(*this)||!admission->matches(*this)||stop.stop_requested())throw std::logic_error("Transform source changed before begin publication");data=std::move(candidate);region_selection=std::move(selected);floating_selection_=std::move(floating);raw_baseline_=std::move(baseline);floating_same_document_source_=true;vector_indication_active=session->capture_indication_active();floating_transform_=std::move(session);
    ++state_generation_;++region_selection_generation_;++floating_selection_generation_;return result;
}
FloatingTransformCommandResult Document::execute_floating_transform_command(FloatingTransformCommand command,std::uint64_t origin,std::stop_token stop){
    std::uint32_t flags{};switch(command){case FloatingTransformCommand::Scale:flags=2;break;case FloatingTransformCommand::Rotate:flags=4;break;case FloatingTransformCommand::ReverseHorizontal:flags=16;break;case FloatingTransformCommand::ReverseVertical:flags=32;break;default:return {};}
    if(floating_transform_preparing_)throw std::logic_error("Transform begin is still preparing");
    modal_detail::poll(stop);const auto generation=state_generation_;const bool fresh=!has_floating_transform();FloatingTransformCommandResult result;result.handled=true;
    if(fresh){result.begin=begin_floating_transform(flags==4?FloatingTransformOperation::Rotate:FloatingTransformOperation::Scale,origin,stop);if(*result.begin!=FloatingTransformBegin::Started){result.state_changed=state_generation_!=generation;return result;}}
    const auto original=floating_transform_;
    if((flags&0x30)||(!fresh&&std::uint32_t(original->state().operation)!=flags)){
        require_current_floating_transform();auto candidate=original->command(flags,stop);auto selected=candidate->selection_live();auto floating=candidate->live_floating();
        const bool reverse=flags&0x30;
        candidate->bind(*this,original->admitted_data(),reverse?selected:region_selection,reverse?floating:floating_selection_,
            {state_generation_+1,region_selection_generation_+unsigned(reverse),floating_selection_generation_+unsigned(reverse)},stop);
        if(floating_transform_!=original||!original->matches(*this)||stop.stop_requested())throw std::logic_error("Transform command owner changed before publication");
        if(reverse){region_selection=std::move(selected);floating_selection_=std::move(floating);++region_selection_generation_;++floating_selection_generation_;}
        floating_transform_=std::move(candidate);++state_generation_;
    }
    result.records=floating_transform_->state().records;
    if(fresh&&(flags&0x30))result.outcome=accept_floating_transform("Transform",stop);
    result.session_active=has_floating_transform();result.state_changed=state_generation_!=generation;return result;
}
void Document::switch_floating_transform_operation(FloatingTransformOperation operation){require_current_floating_transform();const auto original=floating_transform_;auto candidate=original->switch_operation(operation);if(floating_transform_!=original||!original->matches(*this))throw std::logic_error("Transform owner changed during preparation");candidate->bind(*this,original->admitted_data(),region_selection,floating_selection_,{state_generation_+1,region_selection_generation_,floating_selection_generation_});if(floating_transform_!=original||!original->matches(*this))throw std::logic_error("Transform owner changed before publication");floating_transform_=std::move(candidate);++state_generation_;}
void Document::preview_floating_transform(const FloatingTransformPacket& packet,std::stop_token stop){require_current_floating_transform();const auto original=floating_transform_;auto candidate=original->preview(packet,stop);if(floating_transform_!=original||!original->matches(*this)||stop.stop_requested())throw std::logic_error("Transform owner changed during preparation");candidate->bind(*this,original->admitted_data(),region_selection,floating_selection_,{state_generation_+1,region_selection_generation_,floating_selection_generation_},stop);if(floating_transform_!=original||!original->matches(*this)||stop.stop_requested())throw std::logic_error("Transform owner changed before publication");floating_transform_=std::move(candidate);++state_generation_;}
void Document::position_floating_transform(int dx,int dy,std::optional<FloatingTransformPoint> pivot){
    require_current_floating_transform();
    const auto original=floating_transform_;
    auto candidate=original->position(dx,dy,pivot);
    auto selected=candidate->selection_live();
    auto floating=candidate->live_floating();
    candidate->bind(*this,original->admitted_data(),selected,floating,
                    {state_generation_+1,region_selection_generation_+1,floating_selection_generation_+1});
    if(floating_transform_!=original||!original->matches(*this))throw std::logic_error("Transform changed during movement");
    region_selection=std::move(selected);floating_selection_=std::move(floating);floating_transform_=std::move(candidate);
    ++state_generation_;++region_selection_generation_;++floating_selection_generation_;
}
void Document::release_floating_transform_scale(const FloatingTransformPacket& packet,std::uint32_t reversals,std::stop_token stop){require_current_floating_transform();const auto original=floating_transform_;if(reversals&~0x30u)throw std::invalid_argument("Invalid Transform reverse flags");auto candidate=original->release(packet,2|reversals|packet.reversals,0,stop);auto selected=candidate->selection_live();auto floating=candidate->live_floating();if(floating_transform_!=original||!original->matches(*this)||stop.stop_requested())throw std::logic_error("Transform owner changed during preparation");candidate->bind(*this,original->admitted_data(),selected,floating,{state_generation_+1,region_selection_generation_+1,floating_selection_generation_+1},stop);if(floating_transform_!=original||!original->matches(*this)||stop.stop_requested())throw std::logic_error("Transform owner changed before publication");region_selection=std::move(selected);floating_selection_=std::move(floating);floating_transform_=std::move(candidate);++state_generation_;++region_selection_generation_;++floating_selection_generation_;}
void Document::release_floating_transform_rotate(const FloatingTransformPacket& packet,std::int32_t degrees,std::stop_token stop){require_current_floating_transform();const auto original=floating_transform_;auto candidate=original->release(packet,4,degrees,stop);auto selected=candidate->selection_live();auto floating=candidate->live_floating();if(floating_transform_!=original||!original->matches(*this)||stop.stop_requested())throw std::logic_error("Transform owner changed during preparation");candidate->bind(*this,original->admitted_data(),selected,floating,{state_generation_+1,region_selection_generation_+1,floating_selection_generation_+1},stop);if(floating_transform_!=original||!original->matches(*this)||stop.stop_requested())throw std::logic_error("Transform owner changed before publication");region_selection=std::move(selected);floating_selection_=std::move(floating);floating_transform_=std::move(candidate);++state_generation_;++region_selection_generation_;++floating_selection_generation_;}
void Document::reverse_floating_transform(bool vertical,std::stop_token stop){require_current_floating_transform();const auto original=floating_transform_;auto candidate=original->release(original->state().packet,vertical?32u:16u,0,stop);auto selected=candidate->selection_live();auto floating=candidate->live_floating();if(floating_transform_!=original||!original->matches(*this)||stop.stop_requested())throw std::logic_error("Transform owner changed during preparation");candidate->bind(*this,original->admitted_data(),selected,floating,{state_generation_+1,region_selection_generation_+1,floating_selection_generation_+1},stop);if(floating_transform_!=original||!original->matches(*this)||stop.stop_requested())throw std::logic_error("Transform owner changed before publication");region_selection=std::move(selected);floating_selection_=std::move(floating);floating_transform_=std::move(candidate);++state_generation_;++region_selection_generation_;++floating_selection_generation_;}
FloatingTransformOutcome Document::accept_floating_transform(const std::string& label,std::stop_token stop){
    require_current_floating_transform();const auto session=floating_transform_;const SelectionIndicationWitness witness(*this);
    std::vector<std::shared_ptr<const raster_history_detail::Edit>> paste;
    auto available=session->available_budget();auto materialized=session->materialize(stop,&available[0],&available[1],&paste);
    if(!session->matches(*this))throw std::logic_error("Transform source changed during materialization");
    auto selected=session->selection_live();auto original_selected=selection_history_snapshot(session->before(),session->selection_before());auto prior=session->prior_floating();
    const bool changed=!same_raw_content(materialized.data,session->before());
    raster_history_detail::Budget budget{raster_history_detail::storage_limit-available[1],raster_history_detail::work_limit-available[0],stop};
    budget.allocate(sizeof(CompoundEdit)+(paste.size()+3)*sizeof(Revision)+label.size()*(paste.size()+4));
    auto group=std::make_shared<CompoundEdit>();group->past.reserve(paste.size()+3);
    if(const auto capture=session->capture_history();capture&&!capture->planes.empty())group->past.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,capture});
    group->past.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,RegionSelectionEdit{std::move(original_selected),{},{}}});
    if(prior){FloatingSelectionEdit floating;floating.mode=FloatingSelectionEdit::Mode::Finalize;floating.exchange_data=false;floating.opposite=floating_history_snapshot(*prior);floating.binding=raster_history_detail::FloatingBinding::capture(session->before(),*prior,budget);group->past.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(floating)});}
    for(auto& edit:paste)group->past.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(edit)});
    //61C6F0 refreshes the current carrier with current application flags only
    //after its compound has been assembled. The result is the final latch,
    //not the native old-or-new redraw return value.
    const bool mask=selected.state==SelectionLayerState::Mask||selected.state==SelectionLayerState::VectorIndicated;
    const double dpi=mask&&!(witness.flags()&1u)?selection_carrier_dpi(selected,materialized.data.base.dpi):0;
    const bool active=vector_indication_detail::refresh_marks(materialized.data,witness.active,selected,witness.flags(),dpi,budget.bytes,budget.work,stop);
    Revision prepared{DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::shared_ptr<const CompoundEdit>(std::move(group))};
    std::vector<Revision> prepared_undo;prepared_undo.reserve(undo_.size()+1);
    if(floating_transform_!=session||!witness.matches(*this)||!session->matches(*this)||stop.stop_requested())throw std::logic_error("Transform source changed before accept publication");
    static_assert(std::is_nothrow_move_constructible_v<Revision>);
    for(auto& item:undo_)prepared_undo.push_back(std::move(item));prepared_undo.push_back(std::move(prepared));undo_.swap(prepared_undo);
    data=std::move(materialized.data);region_selection=std::move(selected);vector_indication_active=active;floating_selection_.reset();floating_transform_.reset();trim_history();redo_.clear();undo_next_=true;revision_=next_++;if(changed||materialized.content_history)runtime_dirty_=true;++state_generation_;++region_selection_generation_;++floating_selection_generation_;return {true,changed,true};
}
void Document::cancel_floating_transform(){
    if(floating_transform_preparing_)throw std::logic_error("Transform begin is still preparing");if(!floating_transform_)return;
    const auto session=floating_transform_;const SelectionIndicationWitness witness(*this);
    const bool admitted_hints=session->matches(*this)&&raster_history_detail::same_identity_layout(data,session->admitted_data());
    const auto available=session->available_budget();raster_history_detail::Budget budget{raster_history_detail::storage_limit-available[1],0,{}};
    // Cancel can escape a stale session. Freeze the current source, so its
    // unbound owners/properties survive and allocator reentry is still checked.
    const auto original=data.original;const auto original_content=data.original_content;
    modal_detail::data_charge(data,budget.bytes);auto source=modal_detail::frozen(data,{});
    modal_detail::data_charge(source,budget.bytes,0,false);auto candidate=source;
    raster_history_detail::Resolver targets(candidate,budget,admitted_hints);
    auto selected=restore_selection_history(candidate,selection_history_snapshot(session->before(),session->selection_before()),region_selection,witness.flags());
    auto prior=session->prior_floating();
    if(prior){auto restored=std::make_shared<FloatingSelection>(*floating_history_snapshot(*prior));restored->same_document_source=floating_same_document_source_;
        budget.allocate(sizeof(FloatingSelection)+prior->sources->size()*sizeof(FloatingSource));auto sources=std::make_shared<std::vector<FloatingSource>>(*prior->sources);
        for(auto& mapping:*sources){const auto old_layer=mapping.layer;for(auto& index:mapping.planes){budget.allocate(sizeof(std::size_t));const auto target=raster_history_detail::target(session->before(),old_layer,index);mapping.layer=targets.owner(target);index=targets.plane(target);}}
        restored->sources=std::move(sources);prior=std::move(restored);
    }
    // Native fresh MultiUndo restores Select (including615390) before the
    // cut raster ROI. Prior-floating MultiUndo restores Float1 then Select.
    const bool mask=selected.state==SelectionLayerState::Mask||selected.state==SelectionLayerState::VectorIndicated;
    const double dpi=mask&&!(witness.flags()&1u)?selection_carrier_dpi(selected,candidate.base.dpi):0;
    const auto active=vector_indication_detail::refresh_marks(candidate,witness.active,selected,witness.flags(),dpi,budget.bytes,budget.work,{});
    if(const auto capture=session->capture_history())for(const auto& item:capture->planes){auto& plane=targets.get(item.target);
        if(!plane.raster||plane.opaque)throw std::logic_error("Transform cancellation raster target changed");
        // The opposite snapshot dies with this cancelled group; only the
        // primary offscreen -> live ROI copy contributes to the result.
        raster_history_detail::copy_roi(item.primary,*plane.raster,item.roi,budget);plane.pixels_changed=true;
    }
    candidate.original=original;candidate.original_content=original_content;
    if(floating_transform_!=session||!witness.matches(*this)||data.original!=original||data.original_content!=original_content||!modal_detail::data_same(data,source)||!raster_history_detail::same_identity_layout(data,source))throw std::logic_error("Transform owner changed during cancellation");
    data=std::move(candidate);region_selection=std::move(selected);floating_selection_=std::move(prior);vector_indication_active=active;floating_transform_.reset();
    ++state_generation_;++region_selection_generation_;++floating_selection_generation_;
}

void Document::commit_grid_move(std::array<int,2> before,const std::string& label){require_no_floating_transform();
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,GridMove{before}});
    trim_history();
    redo_.clear();undo_next_=true;++state_generation_;++grid_generation_;
}
void Document::commit_grid_settings(const GridState& before,const std::string& label){require_no_floating_transform();
    // RCLength::operator!= compares value, reference and unit, even when two
    // different representations describe the same physical coordinate.
    if(grid.position!=before.position)commit_grid_move(before.origin,label);
    else if(grid_settings(grid)!=grid_settings(before)||grid.origin!=before.origin){++state_generation_;++grid_generation_;}
}
void Document::commit_guide_move(std::array<int,2> before,const std::string& label){require_no_floating_transform();
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,GuideMove{before}});
    trim_history();
    redo_.clear();undo_next_=true;++state_generation_;++guide_generation_;
}
void Document::commit_guide_created(std::size_t index,const std::string& label){require_no_floating_transform();
    if(index>=guide.records.size())throw std::out_of_range("参考线索引无效。");
    GuideEdit edit{GuideEditMode::Created,false,false,{index},{},{}};
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(edit)});
    trim_history();
    redo_.clear();undo_next_=true;++document_modify_count_;++state_generation_;++guide_generation_;
}
void Document::commit_guide_edit(const std::vector<GuideSnapshot>& before,GuideEditMode mode,const std::string& label){require_no_floating_transform();
    if(mode!=GuideEditMode::Removed&&mode!=GuideEditMode::Modified)throw std::invalid_argument("参考线编辑模式无效。");
    if(before.empty())return;
    GuideEdit edit{mode,false,false,{},{},{}};
    edit.indices.reserve(before.size());edit.primary.reserve(before.size());
    for(const auto& entry:before){edit.indices.push_back(entry.index);edit.primary.push_back(entry.record);}
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(edit)});
    trim_history();
    redo_.clear();undo_next_=true;++document_modify_count_;++state_generation_;++guide_generation_;
}
void Document::set_text_plane(std::size_t layer,std::size_t plane,Bytes specific,Bytes name,Raster derived,const std::string& label){
    require_no_floating_transform();auto& l=data.layers.at(layer);auto& p=l.planes.at(plane);
    if(p.family!=0x40000004||p.opaque||l.opaque)throw std::invalid_argument("Not a text plane");
    (void)decode_text_parameters(specific);
    if((p.common.width>0&&derived.width!=std::trunc(p.common.width*data.base.dpi/p.common.resolution))||(p.common.height>0&&derived.height!=std::trunc(p.common.height*data.base.dpi/p.common.resolution))||derived.depth!=32||derived.resolution!=data.base.dpi)throw std::invalid_argument("Text glyph cache geometry mismatch");
    auto cache=std::make_shared<const TextRasterCache>(TextRasterCache{specific,p.common.width,p.common.height,p.common.resolution,std::move(derived)});
    TextEdit edit{l.common.runtime_identity,p.common.runtime_identity,p.specific,p.common.name};
    Revision item{DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(edit)};
    undo_.reserve(undo_.size()+1);undo_.push_back(std::move(item));
    p.specific.swap(specific);p.common.name.swap(name);p.runtime_text_cache=std::move(cache);
    trim_history();redo_.clear();undo_next_=true;revision_=next_++;++state_generation_;
}
bool Document::reset_plane_alpha_editing(){
    require_no_floating_transform();
    if(!retas::reset_plane_alpha_editing(data))return false;
    ++state_generation_;return true;
}
bool Document::edit_layer_plane(LayerPlaneOperation operation,const ElementConvertOptions& options,const std::string& label,bool batch){
    require_no_floating_transform();
    if(batch&&operation!=LayerPlaneOperation::ConvertLayer)throw std::invalid_argument("Batch layer adapter only accepts ConvertLayer");
    if(!batch&&data.active_plane()&&data.active_plane()->common.opacity_mode==4
       &&(operation!=LayerPlaneOperation::MergePlanes||!(data.layers[data.current_layer].family&0x40000000u)))reset_plane_alpha_editing();
    auto staged=*this;staged.collecting_history_=true;staged.undo_.clear();staged.redo_.clear();staged.undo_next_=true;
    staged.finalize_floating_selection(label);
    auto candidate=staged.data;bool changed{};
    switch(operation){
    case LayerPlaneOperation::ConvertLayer:changed=convert_layer(candidate,options,batch);break;
    case LayerPlaneOperation::ConvertPlane:changed=convert_planes(candidate,options);break;
    case LayerPlaneOperation::MergeLayers:changed=merge_same_layers(candidate);break;
    case LayerPlaneOperation::MergePlanes:changed=merge_same_planes(candidate);break;
    }
    if(changed){
        Revision item{staged.data,label,staged.revision_,HistorySelection::RestoreSnapshot,StackEdit{}};
        staged.undo_.push_back(std::move(item));staged.data=std::move(candidate);staged.revision_=staged.next_++;++staged.state_generation_;
    }
    if(staged.undo_.empty())return false;
    raster_history_detail::Budget local_budget;auto& budget=staged.selection_command_budget_?*staged.selection_command_budget_:local_budget;staged.paint_history_manager_=prepare_paint_manager(staged.paint_history_manager_,staged.data,true,budget);
    auto group=std::make_shared<const CompoundEdit>(CompoundEdit{std::move(staged.undo_),std::move(staged.redo_)});
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::RestoreSnapshot,std::move(group)});
    trim_history();
    staged.undo_=std::move(undo_);staged.state_generation_=state_generation_+1;
    static_assert(std::is_nothrow_swappable_v<Document>);staged.history_limit_=history_limit_;staged.collecting_history_=collecting_history_;staged.trim_history();std::swap(*this,staged);return true;
}
namespace {
bool same_adjust_selection(const RegionSelection& a,const RegionSelection& b){
    // These helpers consume carrier/bbox metadata only. Mask storage is immutable
    // and retaining its identity avoids visiting unrelated physical coverage.
    return a.state==b.state&&a.bounds==b.bounds&&a.origin==b.origin&&a.carrier_size==b.carrier_size
        &&modal_detail::bits(a.carrier_dpi,b.carrier_dpi)&&a.mask==b.mask&&a.vector_cache==b.vector_cache
        &&a.vector_cache_origin==b.vector_cache_origin&&a.vector_flags==b.vector_flags;
}
void adjust_packet_budget(const AdjustParameters& p,std::uint64_t& bytes){
    std::uint64_t wire=4096;
    const auto add=[&](std::uint64_t n){modal_detail::charge(wire,n,modal_detail::max_work/8,"Adjust parameter packet exceeds128Mi work");};
    add(p.payload_suffix.size());add(p.wrapper_suffix.size());for(const auto& suffix:p.channel_suffix)add(suffix.size());
    if(const auto* tone=std::get_if<ToneCurveSettings>(&p.settings))for(const auto& curve:tone->curves){if(curve.points.size()>32)throw std::invalid_argument("Adjust curve exceeds32 controls");add(curve.points.size()*sizeof(ToneCurvePoint));}
    // Request, writer scratch, candidate, table witness and parameter history
    // can coexist. Reserve their peak before copying any caller-owned bytes.
    modal_detail::charge(bytes,wire*8,modal_detail::max_bytes,"Adjust parameter staging exceeds512MiB");
}
AdjustParameters freeze_adjust_packet(const AdjustParameters& p){
    AdjustParameters result;result.kind=p.kind;result.parameterless_kind=p.parameterless_kind;
    result.settings=std::holds_alternative<ToneCurveSettings>(p.settings)?FilterSettings{filter_detail::freeze_tone_curve_settings(std::get<ToneCurveSettings>(p.settings))}:p.settings;
    // Opaque archive suffixes are borrowed immutable input until this call returns.
    result.payload_suffix=p.payload_suffix;result.wrapper_suffix=p.wrapper_suffix;result.channel_suffix=p.channel_suffix;return result;
}
}
DustFilterResult Document::execute_adjust_filter(const AdjustParameters& parameters,AdjustFilterExecution mode,std::stop_token stop,const std::string& label){
    try{
        modal_detail::poll(stop);require_no_floating_transform();
        if(has_floating_selection())throw std::logic_error("Adjust filter requires settled artwork");
        if(data.current_layer<0||std::size_t(data.current_layer)>=data.layers.size())throw std::invalid_argument("Adjust filter has no owner");
        const auto layer=std::size_t(data.current_layer);const auto generation=state_generation_;const auto selection=region_selection;
        const auto identity=data.layers[layer].common.runtime_identity;
        const auto& source=data.layers[layer];
        if(source.opaque||source.family!=0x40000200||source.planes.size()>=100000)throw std::invalid_argument("Adjust filter requires an editable Adjust owner");
        if(mode!=AdjustFilterExecution::Direct&&mode!=AdjustFilterExecution::AcceptedSettings)throw std::invalid_argument("Invalid Adjust execution mode");
        if(mode==AdjustFilterExecution::AcceptedSettings&&parameters.parameterless_kind)throw std::invalid_argument("Parameterless helper has no interactive filter action");
        std::uint64_t bytes{};adjust_packet_budget(parameters,bytes);const auto request=freeze_adjust_packet(parameters);modal_detail::data_charge(data,bytes);
        // The snapshot also protects against allocator callbacks changing a live source.
        modal_detail::data_charge(data,bytes);const auto baseline=modal_detail::frozen(data,stop);
        const auto& captured=baseline.layers.at(layer);
        if(state_generation_!=generation||captured.family!=0x40000200||captured.common.runtime_identity!=identity)throw std::logic_error("Adjust source changed during capture");
        modal_detail::charge(bytes,captured.common.name.size()+captured.common.suffix.size(),modal_detail::max_bytes,"Adjust child common metadata exceeds512MiB");
        auto p=make_plane(captured,0x40000010,{},captured.common.flags[4]?captured.common.color:0,request);p.common.resolution=baseline.base.dpi;p.common.flags[2]=0;p.common.runtime_operable=true;p.common.field_14=-1;p.common.flags[4]=captured.common.flags[4]?1:0;
        const bool retained=mode==AdjustFilterExecution::AcceptedSettings
            &&(request.kind==FilterKind::GaussianBlur||request.kind==FilterKind::Dust);
        if(mode==AdjustFilterExecution::AcceptedSettings&&!retained)return {};
        if(retained)p.runtime_adjust_cache=adjust_render_detail::fresh_cache(p);
        else{
            const auto binding=adjust_render_detail::bind(baseline,layer,p,selection,stop);
            // The native dispatcher frees the helper but leaves its plane behind
            // on false. Refuse that dangling-pointer topology atomically.
            if(!binding.succeeded)throw std::invalid_argument("Adjust dispatcher false would retain a released helper");
            p.runtime_adjust_cache=binding.cache;p.pixels_changed=true;p.raster_modified=true;
        }
        modal_detail::charge(bytes,captured.planes.size()*sizeof(Plane)+sizeof(Plane)+adjust_render_detail::cache_storage(p),modal_detail::max_bytes,"Adjust append exceeds512MiB");
        for(const auto& sibling:captured.planes)modal_detail::charge(bytes,sibling.specific.size()+sibling.suffix.size()+sibling.opaque_record.size()+sibling.common.name.size()+sibling.common.suffix.size(),modal_detail::max_bytes,"Adjust sibling staging exceeds512MiB");
        auto planes=baseline.layers[layer].planes;planes.push_back(std::move(p));
        std::optional<Revision> item;
        if(!retained)item.emplace(Revision{DocumentData{},label,revision_,HistorySelection::PreserveCurrent,AddPlaneEdit{layer,captured.planes.size(),{}}});
        std::vector<Revision> history;
        if(item){modal_detail::charge(bytes,(undo_.size()+1)*sizeof(Revision)+label.size(),modal_detail::max_bytes,"Adjust history exceeds512MiB");history.reserve(undo_.size()+1);}
        modal_detail::poll(stop);
        if(state_generation_!=generation||data.current_layer!=int(layer)||data.layers.at(layer).common.runtime_identity!=identity||!modal_detail::data_same(data,baseline)||!raster_history_detail::same_identity_layout(data,baseline)||!same_adjust_selection(region_selection,selection))throw std::logic_error("Adjust source changed before publication");
        require_no_floating_transform();
        modal_detail::poll(stop);
        if(item){for(auto& old:undo_)history.push_back(std::move(old));history.push_back(std::move(*item));undo_.swap(history);}
        data.layers[layer].planes.swap(planes);
        if(!retained){trim_history();redo_.clear();undo_next_=true;++document_modify_count_;}
        ++state_generation_;return {DustFilterStatus::Applied,0,false,{}};
    }catch(const std::exception& error){return {stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected,0,false,stop.stop_requested()?std::string{}:error.what()};}
}
bool Document::bind_adjust_plane(std::size_t layer,std::size_t plane,std::stop_token stop){
    modal_detail::poll(stop);
    require_no_floating_transform();const auto generation=state_generation_;const auto selection=region_selection;
    const auto owner_id=data.layers.at(layer).common.runtime_identity,plane_id=data.layers.at(layer).planes.at(plane).common.runtime_identity;
    std::uint64_t bytes{};modal_detail::data_charge(data,bytes);modal_detail::data_charge(data,bytes);
    const auto baseline=modal_detail::frozen(data,stop);
    const auto& p=baseline.layers.at(layer).planes.at(plane);modal_detail::charge(bytes,2*(sizeof(adjust_render_detail::TableCache)+p.specific.size()+p.common.name.size()+p.common.suffix.size()),modal_detail::max_bytes,"Adjust binding staging exceeds512MiB");
    const auto binding=adjust_render_detail::bind(baseline,layer,p,selection,stop);
    modal_detail::poll(stop);
    if(state_generation_!=generation||data.layers.at(layer).common.runtime_identity!=owner_id||data.layers.at(layer).planes.at(plane).common.runtime_identity!=plane_id||!modal_detail::data_same(data,baseline)||!raster_history_detail::same_identity_layout(data,baseline)||!same_adjust_selection(region_selection,selection))throw std::logic_error("Adjust source changed during binding");
    require_no_floating_transform();modal_detail::poll(stop);
    if(binding.succeeded){data.layers[layer].planes[plane].runtime_adjust_cache=binding.cache;++state_generation_;}
    return binding.succeeded;
}
bool Document::accept_adjust_settings(std::size_t layer,std::size_t plane,const AdjustParameters& parameters,const std::string& label){
    require_no_floating_transform();const auto generation=state_generation_;const auto selection=region_selection;
    const auto owner_id=data.layers.at(layer).common.runtime_identity,plane_id=data.layers.at(layer).planes.at(plane).common.runtime_identity;
    std::uint64_t storage{};adjust_packet_budget(parameters,storage);const auto request=freeze_adjust_packet(parameters);modal_detail::data_charge(data,storage);modal_detail::data_charge(data,storage);
    const auto baseline=modal_detail::frozen(data,{});const auto& owner=baseline.layers.at(layer);const auto& old=owner.planes.at(plane);
    if(owner.opaque||owner.family!=0x40000200||old.opaque||old.family!=0x40000010||old.raster)throw std::invalid_argument("Adjust settings target is not editable");
    modal_detail::charge(storage,8*(old.specific.size()+old.common.name.size()+old.common.suffix.size()+sizeof(adjust_render_detail::TableCache)),modal_detail::max_bytes,"Adjust settings staging exceeds512MiB");
    if(old.specific.size()>modal_detail::max_work/8)throw std::length_error("Adjust settings source exceeds128Mi work");
    if(!editable_adjust_plane(old))throw std::invalid_argument("Adjust settings target is not editable");
    const auto previous=decode_adjust_parameters(old.specific);
    if(previous.kind!=request.kind||previous.parameterless_kind!=request.parameterless_kind)throw std::invalid_argument("Adjust settings cannot replace the helper kind");
    auto value=request;value.payload_suffix=previous.payload_suffix;value.wrapper_suffix=previous.wrapper_suffix;value.channel_suffix=previous.channel_suffix;
    Plane candidate;candidate.family=old.family;candidate.common=old.common;candidate.specific=encode_adjust_parameters(value);
    const bool preserve=!value.parameterless_kind&&(value.kind==FilterKind::GaussianBlur||value.kind==FilterKind::Dust);
    std::optional<Revision> item;
    if(preserve)candidate.runtime_adjust_cache=adjust_render_detail::retain_cache(old,candidate.specific);
    else{
        const auto bound=adjust_render_detail::bind(baseline,layer,candidate,selection);
        candidate.runtime_adjust_cache=bound.cache;
        item.emplace(Revision{DocumentData{},label,revision_,HistorySelection::PreserveCurrent,PlaneSettingsEdit{owner.common.runtime_identity,old.common.runtime_identity,old.specific,true}});
    }
    std::vector<Revision> history;
    if(item){modal_detail::charge(storage,(undo_.size()+1)*sizeof(Revision)+label.size(),modal_detail::max_bytes,"Adjust history exceeds512MiB");history.reserve(undo_.size()+1);}
    if(state_generation_!=generation||data.layers.at(layer).common.runtime_identity!=owner_id||data.layers.at(layer).planes.at(plane).common.runtime_identity!=plane_id||!modal_detail::data_same(data,baseline)||!raster_history_detail::same_identity_layout(data,baseline)||!same_adjust_selection(region_selection,selection))throw std::logic_error("Adjust source changed during settings acceptance");
    require_no_floating_transform();
    if(item){for(auto& old:undo_)history.push_back(std::move(old));history.push_back(std::move(*item));undo_.swap(history);}
    auto& target=data.layers[layer].planes[plane];const bool changed=target.specific!=candidate.specific;
    target.specific.swap(candidate.specific);target.runtime_adjust_cache=std::move(candidate.runtime_adjust_cache);target.pixels_changed=true;target.raster_modified=true;
    if(!preserve){trim_history();redo_.clear();undo_next_=true;revision_=next_++;}
    ++state_generation_;return changed;
}
bool Document::set_adjust_parameters(std::size_t layer,std::size_t plane,const AdjustParameters& parameters,const std::string& label){
    require_no_floating_transform();auto& l=data.layers.at(layer);auto& p=l.planes.at(plane);
    if(l.opaque||!editable_adjust_plane(p))throw std::invalid_argument("Adjust plane is not editable");
    const auto old=decode_adjust_parameters(p.specific);
    if(old.kind!=parameters.kind||old.parameterless_kind!=parameters.parameterless_kind)throw std::invalid_argument("Adjust settings cannot replace the helper kind");
    if(old.settings==parameters.settings)return false;
    auto value=parameters;value.payload_suffix=old.payload_suffix;value.wrapper_suffix=old.wrapper_suffix;value.channel_suffix=old.channel_suffix;
    auto bytes=encode_adjust_parameters(value);
    if(bytes==p.specific)return false;
    auto cache=adjust_render_detail::retain_cache(p,bytes);
    if(filter_detail::table_filter(value.kind))cache.reset();
    PlaneSettingsEdit edit{l.common.runtime_identity,p.common.runtime_identity,p.specific};
    undo_.push_back({DocumentData{},label,revision_,HistorySelection::PreserveCurrent,std::move(edit)});
    p.specific.swap(bytes);p.runtime_adjust_cache=std::move(cache);trim_history();redo_.clear();undo_next_=true;revision_=next_++;++state_generation_;return true;
}
std::shared_ptr<const Document::CaptureSourceEdit> Document::replay_capture_source(const CaptureSourceEdit& edit,raster_history_detail::Budget& budget){
    raster_history_detail::Edit identity;identity.layout=edit.layout;const bool hints=identity.layout_matches(data);
    raster_history_detail::Resolver targets(data,budget,hints);raster_history_detail::Target key{edit.owner,0,edit.hint,0,edit.family,0};
    const auto index=targets.owner(key);auto& owner=data.layers[index];budget.allocate(sizeof(CaptureSourceEdit));auto opposite=std::make_shared<CaptureSourceEdit>(edit);
    if(edit.paint){
        if(paint_history_manager_&&paint_manager_owner(*paint_history_manager_,data)==index){auto result=with_paint_budget(budget,[&](auto& paint){return paint_history_manager_->manager->replay(*edit.paint,owner,paint);});
            owner=std::move(result.layer);opposite->paint=std::move(result.opposite);paint_history_manager_=replace_paint_manager(paint_history_manager_,std::move(result.manager),budget);
        }else{auto result=with_paint_budget(budget,[&](auto& paint){return edit.paint->replay(owner,paint);});owner=std::move(result.layer);opposite->paint=std::move(result.opposite);}
        const auto flags=application_vector_selection_flags.value_or(region_selection.vector_flags);
        const bool mask=region_selection.state==SelectionLayerState::Mask||region_selection.state==SelectionLayerState::VectorIndicated;
        const double dpi=mask&&!(flags&1u)?selection_carrier_dpi(region_selection,data.base.dpi):0;
        if(edit.family!=0x40002000u&&edit.family!=0x40004000u)vector_indication_active=vector_indication_detail::refresh_marks(data,vector_indication_active,region_selection,flags,dpi,budget.bytes,budget.work,budget.stop);
    }else{
        if(!edit.storage)throw std::logic_error("Capture source history has no stored content");opposite->storage=capture_source_storage(owner,budget);
        opposite->layout=raster_history_detail::Edit::identity_layout(data,budget);opposite->hint=index;opposite->planes.clear();
        auto restored=owner;
        if(edit.whole_owner){restored.specific=edit.storage->specific;restored.runtime_vector_high_water=edit.storage->runtime_vector_high_water;restored.runtime_vector_face_colors=edit.storage->runtime_vector_face_colors;}
        for(auto slot:edit.planes){const auto& saved=edit.storage->planes.at(slot);
            raster_history_detail::Target plane_key{edit.owner,saved.common.runtime_identity,edit.hint,slot,edit.family,saved.family};const auto p=targets.plane(plane_key);
            opposite->planes.push_back(p);auto& live=restored.planes[p];const auto common=live.common;const bool visible=live.visibility_modified,frame=live.frame_modified,vanish=live.vanishing_modified;
            live=saved;if(live.raster)live.raster=raster_history_detail::copy_all(*live.raster,budget);live.common=common;live.visibility_modified=visible;live.frame_modified=frame;live.vanishing_modified=vanish;if(live.raster)live.pixels_changed=true;
            if(propagates_plane_line_color(live.family)&&saved.common.color!=common.color)set_vector_plane_line_color(restored,p,common.color);
        }
        owner=std::move(restored);
        invalidate_select_paint_manager(paint_history_manager_,owner);
        if(ordinary_pure_history_owner(owner))paint_history_manager_=ensure_paint_manager(paint_history_manager_,data,index,true,budget,{},true);
    }
    if(!edit.paint&&(edit.family==0x22100000u||edit.family==0x24400000u||edit.family==0x20000200u||edit.family==0x20800000u)){
        const auto flags=application_vector_selection_flags.value_or(region_selection.vector_flags);const bool mask=region_selection.state==SelectionLayerState::Mask||region_selection.state==SelectionLayerState::VectorIndicated;
        vector_indication_active=vector_indication_detail::refresh_marks(data,vector_indication_active,region_selection,flags,mask&&!(flags&1u)?selection_carrier_dpi(region_selection,data.base.dpi):0,budget.bytes,budget.work,budget.stop);
    }
    runtime_dirty_=true;budget.poll();return opposite;
}
void Document::replay(std::vector<Revision>& source,std::vector<Revision>& destination,raster_history_detail::Budget* shared_budget){
    if(source.empty())return;
    const auto selection=source.back().selection;const bool frame_content=source.back().frame_content;
    const auto& runtime=source.back().runtime;
    auto mapped_manager_owner=paint_history_manager_?paint_manager_owner(*paint_history_manager_,data):std::nullopt;
    if(mapped_manager_owner){auto& hint=*mapped_manager_owner;
        if(const auto* order=std::get_if<OrderEdit>(&runtime);order&&!order->layer)hint=std::size_t(std::ranges::find(order->order,hint)-order->order.begin());
        else if(const auto* added=std::get_if<AddLayerEdit>(&runtime)){if(added->detached)hint+=hint>=added->index;else if(hint==added->index)mapped_manager_owner.reset();else hint-=hint>added->index;}
        else if(const auto* removed=std::get_if<DeleteLayersEdit>(&runtime)){
            if(!removed->detached.empty()){for(auto i:removed->indices)if(hint>=i)++hint;}
            else{const auto at=std::lower_bound(removed->indices.begin(),removed->indices.end(),hint);if(at!=removed->indices.end()&&*at==hint)mapped_manager_owner.reset();else hint-=std::size_t(at-removed->indices.begin());}
        }
    }
    const auto* adjust_settings=std::get_if<PlaneSettingsEdit>(&runtime);
    const auto* adjust_addition=std::get_if<AddPlaneEdit>(&runtime);
    const bool adjust_history=(adjust_settings&&adjust_settings->native_binding)||(adjust_addition&&adjust_addition->layer<data.layers.size()&&data.layers[adjust_addition->layer].family==0x40000200);
    const auto* layer_move=std::get_if<AttachedLayerMoveEdit>(&runtime);
    if((paint_history_manager_||adjust_history||(layer_move&&layer_move->refresh_indications))&&!shared_budget&&!std::holds_alternative<std::shared_ptr<const CompoundEdit>>(runtime)
        &&!std::holds_alternative<std::shared_ptr<const raster_history_detail::Edit>>(runtime)&&!std::holds_alternative<std::shared_ptr<const CaptureSourceEdit>>(runtime)){
        // Stabilize a legacy node without copying borrowed live history. All
        // allocations precede a no-throw move into its immutable singleton.
        raster_history_detail::Budget budget;budget.allocate(sizeof(CompoundEdit)+sizeof(Revision)+source.back().label.size());
        const SelectionIndicationWitness witness(*this);const auto id=source.back().id;const auto generation=state_generation_;
        const auto from_size=source.size(),to_size=destination.size();const auto revision=revision_,next=next_,saved=saved_;const bool direction=undo_next_;const auto manager=paint_history_manager_;
        const auto unchanged=[&]{return witness.matches(*this)&&state_generation_==generation&&revision_==revision&&next_==next&&saved_==saved&&undo_next_==direction&&paint_history_manager_==manager
            &&source.size()==from_size&&destination.size()==to_size&&!source.empty()&&source.back().id==id;};
        std::string label;label.reserve(source.back().label.size());if(!unchanged())throw std::logic_error("History changed while stabilizing its label");label=source.back().label;
        auto group=std::make_shared<CompoundEdit>();if(!unchanged())throw std::logic_error("History changed while allocating its stable node");
        auto& items=&source==&undo_?group->past:group->future;items.reserve(1);if(!unchanged())throw std::logic_error("History changed before stable-node publication");
        Revision replacement{DocumentData{},std::move(label),id,selection,std::shared_ptr<const CompoundEdit>(group),frame_content};
        static_assert(std::is_nothrow_move_constructible_v<Revision>&&std::is_nothrow_move_assignable_v<Revision>);
        items.push_back(std::move(source.back()));source.back()=std::move(replacement);replay(source,destination,&budget);return;
    }
    if(std::holds_alternative<std::shared_ptr<const CompoundEdit>>(runtime)||std::holds_alternative<std::shared_ptr<const raster_history_detail::Edit>>(runtime)||std::holds_alternative<std::shared_ptr<const CaptureSourceEdit>>(runtime)){
        //Keep owning payloads and stack witnesses before any allocation; a
        //callback can legally change the live receiver during preparation.
        const bool undoing=&source==&undo_;const auto restored_revision=source.back().id;
        const auto revision=revision_,next=next_,saved=saved_;const bool direction=undo_next_;
        const auto grid_generation=grid_generation_,guide_generation=guide_generation_,attached_generation=attached_temporary_generation_;const auto attached=attached_temporary_layers_;
        const auto source_size=source.size(),destination_size=destination.size();
        const auto destination_id=destination.empty()?0:destination.back().id;
        const auto compound=std::get_if<std::shared_ptr<const CompoundEdit>>(&runtime)?std::get<std::shared_ptr<const CompoundEdit>>(runtime):nullptr;
        const auto raster=std::get_if<std::shared_ptr<const raster_history_detail::Edit>>(&runtime)?std::get<std::shared_ptr<const raster_history_detail::Edit>>(runtime):nullptr;
        const auto capture_source=std::get_if<std::shared_ptr<const CaptureSourceEdit>>(&runtime)?std::get<std::shared_ptr<const CaptureSourceEdit>>(runtime):nullptr;
        const auto manager=paint_history_manager_;const SelectionIndicationWitness witness(*this);
        const auto unchanged=[&]{return witness.matches(*this)&&revision_==revision&&next_==next&&saved_==saved&&undo_next_==direction
            &&grid_generation_==grid_generation&&guide_generation_==guide_generation&&attached_temporary_generation_==attached_generation&&attached_temporary_layers_==attached
            &&paint_history_manager_==manager&&source.size()==source_size&&destination.size()==destination_size&&!source.empty()&&source.back().id==restored_revision
            &&(destination.empty()||destination.back().id==destination_id)
            &&(compound?(std::get_if<std::shared_ptr<const CompoundEdit>>(&source.back().runtime)&&std::get<std::shared_ptr<const CompoundEdit>>(source.back().runtime)==compound)
                :capture_source?(std::get_if<std::shared_ptr<const CaptureSourceEdit>>(&source.back().runtime)&&std::get<std::shared_ptr<const CaptureSourceEdit>>(source.back().runtime)==capture_source)
                :(std::get_if<std::shared_ptr<const raster_history_detail::Edit>>(&source.back().runtime)&&std::get<std::shared_ptr<const raster_history_detail::Edit>>(source.back().runtime)==raster));};
        std::string label;label.reserve(source.back().label.size());
        if(!unchanged())throw std::logic_error("History changed while capturing replay witness");label=source.back().label;
        raster_history_detail::Budget local;auto& budget=shared_budget?*shared_budget:local;
        const auto original=data.original;const auto content=data.original_content;const auto baseline=raw_baseline_;
        modal_detail::data_charge(data,budget.bytes);const auto frozen=modal_detail::frozen(data,{});
        modal_detail::data_charge(frozen,budget.bytes,0,false);modal_detail::data_charge(frozen,budget.bytes,0,false);
        auto frozen_selection=witness.selection;
        if(frozen_selection.mask){const auto& mask=*frozen_selection.mask;budget.allocate(((std::uint64_t(mask.width())+31)/32)*mask.height()*4+((std::uint64_t(mask.width())+255)/256)*((std::uint64_t(mask.height())+255)/256));frozen_selection.mask=std::make_shared<const BinaryMask>(mask);}
        //Never copy live history vectors across an allocator callback. The
        //compound's immutable past/future are copied separately below.
        budget.allocate(guide.records.size()*sizeof(GuideRecord)+path.native().size()*sizeof(std::filesystem::path::value_type));
        Document staged(frozen);staged.history_limit_=history_limit_;staged.collecting_history_=true;staged.data.original=original;staged.data.original_content=content;
        staged.grid=grid;staged.guide=guide;staged.path=path;staged.region_selection=witness.selection;
        staged.application_vector_selection_flags=witness.application_flags;staged.vector_indication_active=witness.active;
        staged.revision_=revision;staged.next_=next;staged.saved_=saved;staged.undo_next_=direction;staged.state_generation_=witness.generation;
        staged.grid_generation_=grid_generation_;staged.guide_generation_=guide_generation_;
        staged.region_selection_generation_=witness.selection_generation;staged.floating_selection_generation_=witness.floating_generation;
        staged.floating_selection_=witness.floating;staged.floating_same_document_source_=floating_same_document_source_;
        staged.attached_temporary_layers_=attached_temporary_layers_;staged.attached_temporary_generation_=attached_temporary_generation_;
        staged.runtime_dirty_=runtime_dirty_;staged.document_modify_count_=document_modify_count_;staged.raw_baseline_=baseline;staged.paint_history_manager_=manager;
        staged.vanishing_move_preparing_=vanishing_move_preparing_;staged.frame_move_preparing_=frame_move_preparing_;staged.floating_transform_preparing_=floating_transform_preparing_;
        budget.allocate(guide.records.size()*sizeof(GuideRecord)+path.native().size()*sizeof(std::filesystem::path::value_type));
        const auto initial_grid=staged.grid;const auto initial_guide=staged.guide;const auto initial_path=staged.path;
        if(!unchanged()||!modal_detail::data_same(data,frozen)||!raster_history_detail::same_identity_layout(data,frozen))throw std::logic_error("History source changed while preparing its candidate");
        //The frozen graph is the comparison witness. The candidate retains
        //live COW block identities, which object-transfer history owns. Typed
        //ROI writes detach their bound blocks before modifying any bytes.
        for(std::size_t l=0;l<staged.data.layers.size();++l)for(std::size_t p=0;p<staged.data.layers[l].planes.size();++p)if(auto& image=staged.data.layers[l].planes[p].raster){
            const auto& tiles=data.layers[l].planes[p].raster->tiles;auto live=tiles.begin();for(auto& [key,bytes]:image->tiles){(void)key;bytes=live->second;++live;}}
        RuntimeHistory opposite;
        if(compound){staged.undo_=compound->past;staged.redo_=compound->future;
            if(undoing){while(!staged.undo_.empty())staged.replay(staged.undo_,staged.redo_,&budget);}
            else{while(!staged.redo_.empty())staged.replay(staged.redo_,staged.undo_,&budget);}
            opposite=std::make_shared<const CompoundEdit>(CompoundEdit{std::move(staged.undo_),std::move(staged.redo_)});
        }else if(capture_source){opposite=staged.replay_capture_source(*capture_source,budget);}
        else{if(!raster)throw std::logic_error("Missing raster history");opposite=raster->replay(staged.data,budget);if(!raster->native_raster_dirty)staged.runtime_dirty_=true;}
        Revision prepared{DocumentData{},label,revision,selection,std::move(opposite),frame_content};
        //Reserve a detached destination. A callback may push to the live
        //history; reserving that same vector reentrantly corrupts its storage
        //before a later currency guard can reject the stale outer replay.
        std::vector<Revision> prepared_destination;prepared_destination.reserve(destination_size+1);
        if(!unchanged()||grid!=initial_grid||guide!=initial_guide||path!=initial_path||data.original!=original||data.original_content!=content||raw_baseline_!=baseline||!modal_detail::data_same(data,frozen)||!raster_history_detail::same_identity_layout(data,frozen)||!modal_detail::selection_same(region_selection,frozen_selection))throw std::logic_error("History source changed before replay publication");
        require_no_floating_transform();
        static_assert(std::is_nothrow_move_constructible_v<Revision>);
        for(auto& item:destination)prepared_destination.push_back(std::move(item));prepared_destination.push_back(std::move(prepared));destination.swap(prepared_destination);
        source.pop_back();staged.undo_=std::move(undo_);staged.redo_=std::move(redo_);
        staged.revision_=restored_revision;staged.state_generation_=state_generation_+1;staged.history_limit_=history_limit_;staged.collecting_history_=collecting_history_;staged.trim_history();std::swap(*this,staged);return;
    }else if(const auto* settings=std::get_if<PlaneSettingsEdit>(&runtime)){
        Plane* target{};std::size_t owner{};for(std::size_t i=0;i<data.layers.size();++i)if(data.layers[i].common.runtime_identity==settings->layer)for(auto& p:data.layers[i].planes)if(p.common.runtime_identity==settings->plane&&p.family==0x40000010){target=&p;owner=i;}
        if(!target)throw std::logic_error("Adjust history target changed");
        auto bytes=settings->specific;PlaneSettingsEdit opposite{settings->layer,settings->plane,target->specific,settings->native_binding};
        auto cache=adjust_render_detail::retain_cache(*target,bytes);
        if(settings->native_binding){auto candidate=*target;candidate.specific=bytes;cache=adjust_render_detail::bind(data,owner,candidate,region_selection).cache;}
        else if(const auto kind=archived_adjust_filter_kind(*target);kind&&(*kind==1||*kind==2||*kind==4))cache.reset();
        destination.push_back({DocumentData{},source.back().label,revision_,selection,std::move(opposite)});target->specific.swap(bytes);target->runtime_adjust_cache=std::move(cache);if(settings->native_binding){target->pixels_changed=true;target->raster_modified=true;}
    }else if(const auto* text=std::get_if<TextEdit>(&runtime)){
        Plane* target{};for(auto& l:data.layers)if(l.common.runtime_identity==text->layer)for(auto& p:l.planes)if(p.common.runtime_identity==text->plane&&p.family==0x40000004)target=&p;
        if(!target)throw std::logic_error("Text history target changed");
        auto specific=text->specific;auto name=text->name;
        TextEdit opposite{text->layer,text->plane,target->specific,target->common.name};
        destination.push_back({DocumentData{},source.back().label,revision_,selection,std::move(opposite)});
        target->specific.swap(specific);target->common.name.swap(name);target->runtime_text_cache.reset();
    }else if(const auto* page=std::get_if<PageTransformEdit>(&runtime)){
        raster_history_detail::Budget local;auto& budget=shared_budget?*shared_budget:local;
        modal_detail::data_charge(data,budget.bytes);auto candidate=data;page_geometry_detail::apply(candidate,page->kind,budget.bytes,budget.work,budget.stop);
        destination.push_back({DocumentData{},source.back().label,revision_,selection,PageTransformEdit{page_geometry_detail::inverse(page->kind)}});
        data=std::move(candidate);region_selection=cleared_selection({}, {data.base.width(),data.base.height()});region_selection.carrier_dpi=data.base.dpi;vector_indication_active=false;++region_selection_generation_;runtime_dirty_=true;
    }else if(std::holds_alternative<FrameEdit>(runtime)){replay_frame(source,destination);
    }else if(std::holds_alternative<VanishingEdit>(runtime)){replay_vanishing(source,destination);
    }else if(const auto* move=std::get_if<GridMove>(&runtime)){
        auto candidate=grid;move_grid_to(candidate,move->origin);
        destination.push_back({DocumentData{},source.back().label,revision_,selection,GridMove{grid.origin}});
        grid=candidate;++grid_generation_;
    }else if(const auto* move=std::get_if<GuideMove>(&runtime)){
        auto candidate=guide;move_guide_to(candidate,move->origin);
        destination.push_back({DocumentData{},source.back().label,revision_,selection,GuideMove{guide.origin}});
        guide=std::move(candidate);++guide_generation_;
    }else if(const auto* guide_edit=std::get_if<GuideEdit>(&runtime)){
        auto edit=*guide_edit;auto candidate=guide;
        if(!edit.reverse&&edit.mode!=GuideEditMode::Removed&&!edit.captured){
            edit.secondary.reserve(edit.indices.size());
            for(const auto index:edit.indices)edit.secondary.push_back(candidate.records.at(index));
            edit.captured=true;
        }
        const bool insert=edit.reverse?edit.mode==GuideEditMode::Created:edit.mode==GuideEditMode::Removed;
        const bool remove=edit.reverse?edit.mode==GuideEditMode::Removed:edit.mode==GuideEditMode::Created;
        const auto& records=edit.reverse?edit.secondary:edit.primary;
        // Native replay processes saved indices in their captured order.
        // Stage the whole array so an invalid slot or allocation failure
        // cannot publish a partially replayed list or consume either stack.
        for(std::size_t i=0;i<edit.indices.size();++i){
            const auto index=edit.indices[i];
            if(insert){
                candidate.records.insert(candidate.records.begin()+std::min(index,candidate.records.size()),records.at(i));
            }else{
                if(index>=candidate.records.size())continue;
                if(remove)candidate.records.erase(candidate.records.begin()+index);
                // sub_46C120 overwrites only the 24-byte Length payload;
                // orientation and the current interaction marker survive.
                else candidate.records[index].position=records.at(i).position;
            }
        }
        candidate.enabled=true;edit.reverse=!edit.reverse;
        destination.push_back({DocumentData{},source.back().label,revision_,selection,std::move(edit),frame_content});
        guide=std::move(candidate);++guide_generation_;
    }else if(auto* property=std::get_if<PlanePropertyEdit>(&source.back().runtime)){
        auto& owner=data.layers.at(property->layer);auto& node=owner.planes.at(property->plane);auto& common=node.common;
        std::optional<Layer> candidate;
        if(property->color&&propagates_plane_line_color(node.family)){
            candidate=owner;set_vector_plane_line_color(*candidate,property->plane,std::get<std::uint64_t>(property->saved));
        }
        PlanePropertyEdit opposite;opposite.layer=property->layer;opposite.plane=property->plane;opposite.color=property->color;
        if(property->color)opposite.saved=common.color;
        destination.push_back({DocumentData{},source.back().label,revision_,selection,std::move(opposite)});
        if(candidate)owner.specific.swap(candidate->specific);
        if(property->color)common.color=std::get<std::uint64_t>(property->saved);
        else {auto& saved=std::get<Bytes>(std::get<PlanePropertyEdit>(destination.back().runtime).saved);
            saved.swap(common.name);common.name.swap(std::get<Bytes>(property->saved));}
    }else if(auto* property=std::get_if<LayerPropertyEdit>(&source.back().runtime)){
        auto& common=data.layers.at(property->layer).common;
        LayerPropertyEdit opposite;opposite.layer=property->layer;opposite.field=property->field;
        if(property->field==LayerPropertyEdit::Field::Opacity)opposite.saved=common.opacity;
        else if(property->field==LayerPropertyEdit::Field::OpacityMode)opposite.saved=common.opacity_mode;
        destination.push_back({DocumentData{},source.back().label,revision_,selection,std::move(opposite)});
        if(property->field==LayerPropertyEdit::Field::Name){
            auto& saved=std::get<Bytes>(std::get<LayerPropertyEdit>(destination.back().runtime).saved);
            saved.swap(common.name);common.name.swap(std::get<Bytes>(property->saved));
        }else if(property->field==LayerPropertyEdit::Field::Opacity)common.opacity=std::get<int>(property->saved);
        else common.opacity_mode=std::get<int>(property->saved);
    }else if(auto* removed=std::get_if<DeleteLayersEdit>(&source.back().runtime)){
        if(has_floating_selection())throw std::logic_error("删除历史回放前仍有未撤销的浮动编辑。");
        const bool inserting=!removed->detached.empty();const auto count=removed->indices.size();
        const auto total=data.layers.size()+(inserting?count:0);
        if(!count||(!inserting&&count>=data.layers.size())||removed->indices.back()>=total
            ||(inserting&&removed->detached.size()!=count))throw std::logic_error("删除历史的图层集合已改变。");
        DeleteLayersEdit opposite{removed->indices,inserting?std::vector<Layer>{}:std::vector<Layer>(count)};
        std::vector<Layer> layers;if(inserting)layers.resize(data.layers.size()+count);
        destination.push_back({DocumentData{},source.back().label,revision_,selection,std::move(opposite)});
        if(inserting){
            std::size_t live=0,restored=0;
            for(std::size_t i=0;i<layers.size();++i){
                if(restored<count&&removed->indices[restored]==i){layers[i]=std::move(removed->detached[restored++]);layers[i].common.flags[1]=1;}
                else layers[i]=std::move(data.layers[live++]);
            }
            data.layers.swap(layers);select_inserted_layer(data,removed->indices.back());
        }else{
            auto& detached=std::get<DeleteLayersEdit>(destination.back().runtime).detached;
            for(std::size_t i=0;i<count;++i){const auto index=removed->indices[i]-i;
                detached[i]=std::move(data.layers[index]);data.layers.erase(data.layers.begin()+index);remove_layer_selection(data,index);}
        }
        if(&source==&undo_){if(document_modify_count_)--document_modify_count_;}else ++document_modify_count_;
    }else if(auto* removed=std::get_if<DeletePlanesEdit>(&source.back().runtime)){
        if(has_floating_selection())throw std::logic_error("平面删除子历史前仍有未写回的浮动内容。");
        if(removed->layer>=data.layers.size())throw std::logic_error("平面删除历史的所属图层已改变。");
        auto& owner=data.layers[removed->layer];const bool inserting=!removed->detached.empty();const auto count=removed->indices.size();
        const auto total=owner.planes.size()+(inserting?count:0);
        if(!count||(!inserting&&count>=owner.planes.size())||removed->indices.back()>=total
            ||(inserting&&removed->detached.size()!=count))throw std::logic_error("平面删除历史的集合已改变。");
        DeletePlanesEdit opposite{removed->layer,removed->indices,inserting?std::vector<Plane>{}:std::vector<Plane>(count)};
        std::vector<Plane> planes;
        if(inserting){
            planes.resize(total);std::size_t live{},restored{};
            for(std::size_t i=0;i<planes.size();++i){
                if(restored<count&&removed->indices[restored]==i)prepare_plane_transfer(planes[i],removed->detached[restored++]);
                else prepare_plane_transfer(planes[i],owner.planes[live++]);
            }
        }else prepare_plane_removal(owner,removed->indices,planes,opposite.detached);
        destination.push_back({DocumentData{},source.back().label,revision_,selection,std::move(opposite)});
        if(inserting){std::size_t live=0,restored=0;
            for(std::size_t i=0;i<planes.size();++i){if(restored<count&&removed->indices[restored]==i)transfer_plane(planes[i],removed->detached[restored++]);else transfer_plane(planes[i],owner.planes[live++]);}
            owner.planes.swap(planes);select_inserted_plane(owner,removed->indices.back());
        }else{auto& detached=std::get<DeletePlanesEdit>(destination.back().runtime).detached;
            remove_prepared_planes(owner,removed->indices,planes,detached);
        }
        if(&source==&undo_){if(document_modify_count_)--document_modify_count_;}else ++document_modify_count_;
    }else if(auto* added=std::get_if<AddPlaneEdit>(&source.back().runtime)){
        const bool inserting=!added->detached.empty();const auto index=added->index;
        if(added->detached.size()>1)throw std::logic_error("Invalid single-plane history storage");
        if(added->layer>=data.layers.size())throw std::logic_error("新平面历史的所属图层已改变。");
        auto& owner=data.layers[added->layer];
        if(index>owner.planes.size()||(!inserting&&index==owner.planes.size()))throw std::logic_error("新平面历史的目标位置已改变。");
        std::vector<Plane> planes(inserting?owner.planes.size()+1:owner.planes.size()-1);
        AddPlaneEdit opposite{added->layer,index,{}};
        if(inserting)prepare_plane_transfer(planes[index],added->detached[0]);
        else{opposite.detached.resize(1);prepare_plane_transfer(opposite.detached[0],owner.planes[index]);}
        for(std::size_t i=0;i<owner.planes.size();++i){
            if(inserting)prepare_plane_transfer(planes[i+(i>=index)],owner.planes[i]);
            else if(i!=index)prepare_plane_transfer(planes[i-(i>index)],owner.planes[i]);
        }
        auto floating=remap_inserted_plane(data,floating_selection_,added->layer,index,inserting);
        destination.push_back({DocumentData{},source.back().label,revision_,selection,std::move(opposite)});
        if(inserting){
            for(std::size_t i=0;i<owner.planes.size();++i)transfer_plane(planes[i+(i>=index)],owner.planes[i]);
            transfer_plane(planes[index],added->detached[0]);owner.planes.swap(planes);select_inserted_plane(owner,index);++document_modify_count_;
        }else{
            transfer_plane(std::get<AddPlaneEdit>(destination.back().runtime).detached[0],owner.planes[index]);
            for(std::size_t i=0;i<owner.planes.size();++i)if(i!=index)transfer_plane(planes[i-(i>index)],owner.planes[i]);
            owner.planes.swap(planes);remove_plane_selection(owner,index);if(document_modify_count_)--document_modify_count_;
        }
        if(floating_selection_!=floating){floating_selection_=std::move(floating);++floating_selection_generation_;}
    }else if(auto* added=std::get_if<AddLayerEdit>(&source.back().runtime)){
        const bool inserting=added->detached.has_value();const auto index=added->index;
        if(index>data.layers.size()||(!inserting&&index==data.layers.size()))throw std::logic_error("新图层历史的目标位置已改变。");
        std::vector<Layer> layers(inserting?data.layers.size()+1:data.layers.size()-1);
        auto floating=remap_inserted_layer(data,floating_selection_,index,inserting);
        destination.push_back({DocumentData{},source.back().label,revision_,selection,AddLayerEdit{index,{}}});
        static_assert(std::is_nothrow_move_constructible_v<Layer>&&std::is_nothrow_move_assignable_v<Layer>);
        if(inserting){
            for(std::size_t i=0;i<data.layers.size();++i)layers[i+(i>=index)]=std::move(data.layers[i]);
            layers[index]=std::move(*added->detached);layers[index].common.flags[1]=1;
            data.layers.swap(layers);select_inserted_layer(data,index);++document_modify_count_;
        }else{
            std::get<AddLayerEdit>(destination.back().runtime).detached.emplace(std::move(data.layers[index]));
            for(std::size_t i=0;i<data.layers.size();++i)if(i!=index)layers[i-(i>index)]=std::move(data.layers[i]);
            data.layers.swap(layers);remove_layer_selection(data,index);if(document_modify_count_)--document_modify_count_;
        }
        if(floating_selection_!=floating){floating_selection_=std::move(floating);++floating_selection_generation_;}
    }else if(const auto* ordered=std::get_if<OrderEdit>(&runtime)){
        auto prepared=prepare_order(data,floating_selection_,ordered->layer,ordered->order);
        destination.push_back({DocumentData{},source.back().label,revision_,selection,OrderEdit{ordered->layer,std::move(prepared.inverse)}});
        apply_order(data,ordered->layer,ordered->order,prepared);
        if(floating_selection_!=prepared.floating){floating_selection_=std::move(prepared.floating);++floating_selection_generation_;}
    }else if(const auto* floating_edit=std::get_if<FloatingSelectionEdit>(&runtime)){
        const SelectionIndicationWitness witness(*this);
        auto edit=*floating_edit;
        std::optional<RegionSelection> selected;
        if(edit.selection){
            if(!edit.selection->secondary)edit.selection->secondary=selection_history_snapshot(data,region_selection);
            selected=restore_selection_history(data,edit.reverse?*edit.selection->secondary:edit.selection->primary,
                region_selection,application_vector_selection_flags.value_or(region_selection.vector_flags));
        }
        std::shared_ptr<const FloatingSelection> floating;
        const bool capture=edit.mode==FloatingSelectionEdit::Mode::Capture;
        const bool exchange_data=edit.exchange_data&&edit.mode!=FloatingSelectionEdit::Mode::Offset;
        PreservedVectorColors vector_properties;std::optional<DocumentData> scoped_data;
        if(exchange_data){
            require_selection_layout(data,source.back().data,selection);
            if(edit.raster_scope){
                raster_history_detail::Budget local_budget;auto& budget=shared_budget?*shared_budget:local_budget;
                modal_detail::data_charge(data,budget.bytes,0,false);scoped_data=data;
                for(const auto& scope:*edit.raster_scope){
                    budget.visit(2*(source.back().data.layers.size()+scoped_data->layers.size()));
                    if(std::ranges::count_if(source.back().data.layers,[&](const auto& owner){return owner.common.runtime_identity==scope.owner;})!=1
                       ||std::ranges::count_if(scoped_data->layers,[&](const auto& owner){return owner.common.runtime_identity==scope.owner;})!=1)throw std::logic_error("光栅浮动历史owner标识丢失或不唯一。");
                    const auto saved=std::ranges::find_if(source.back().data.layers,[&](const auto& owner){return owner.common.runtime_identity==scope.owner;});
                    auto target=std::ranges::find_if(scoped_data->layers,[&](const auto& owner){return owner.common.runtime_identity==scope.owner;});
                    if(saved==source.back().data.layers.end()||target==scoped_data->layers.end()||saved->family!=target->family)throw std::logic_error("光栅浮动历史owner绑定无效。");
                    const auto primary_planes=raster_scope_planes(*saved,budget.bytes,budget.work,budget.stop);
                    const auto current_planes=raster_scope_planes(*target,budget.bytes,budget.work,budget.stop);
                    for(auto id:scope.planes){budget.visit(4+4*std::bit_width(primary_planes.size()+current_planes.size()+1));
                        const auto primary=primary_planes.find(id),current=current_planes.find(id);
                        if(primary==primary_planes.end()||current==current_planes.end())throw std::logic_error("光栅浮动历史plane标识丢失。");
                        const auto plane=saved->planes.begin()+primary->second;auto live=target->planes.begin()+current->second;
                        if(!plane->raster||!live->raster)throw std::logic_error("光栅浮动历史plane绑定无效。");
                        live->raster=raster_history_detail::copy_all(*plane->raster,budget);live->pixels_changed=true;
                        if(scope.geometry){live->common.width=plane->common.width;live->common.height=plane->common.height;live->common.depth=plane->common.depth;live->common.resolution=plane->common.resolution;}
                    }
                    if(scope.geometry){target->common.x=saved->common.x;target->common.y=saved->common.y;target->common.width=saved->common.width;target->common.height=saved->common.height;}
                }
            }else vector_properties=prepare_vector_properties(data,source.back().data);
        }
        if(edit.mode!=FloatingSelectionEdit::Mode::Offset){
            if(capture&&!edit.opposite&&floating_selection_)edit.opposite=edit.foreign_payload
                ?std::make_shared<FloatingSelection>(*floating_selection_):floating_history_snapshot(*floating_selection_);
            if((capture?edit.reverse:!edit.reverse)&&edit.opposite){
                raster_history_detail::Budget local_budget;auto& budget=shared_budget?*shared_budget:local_budget;
                auto restored=edit.binding?edit.binding->restore(data,*edit.opposite,budget):std::make_shared<FloatingSelection>(*edit.opposite);
                // The snapshot omits +0x08; applying it leaves the document's
                // live receiver provenance intact instead of resetting it.
                restored->same_document_source=edit.foreign_payload?false:floating_same_document_source_;
                if(edit.foreign_payload){if(restored->target)validate_targeted_floating(data,*restored);else validate_foreign_floating(data,*restored);}
                floating=std::move(restored);
            }
        }else{
            if(!floating_selection_)throw std::logic_error("浮动图像位移历史缺少对应载荷。");
            if(!edit.secondary_offset)edit.secondary_offset=floating_selection_->delta;
            auto moved=std::make_shared<FloatingSelection>(*floating_selection_);
            moved->delta=edit.reverse?*edit.secondary_offset:edit.primary_offset;floating=std::move(moved);
        }
        const bool offset=edit.mode==FloatingSelectionEdit::Mode::Offset;
        const auto indication_data=indication_source(data,offset&&selected&&needs_selection_indications(witness.active,*selected,witness.flags()));
        auto prepared=prepare_selection_indications(indication_data,witness.active,selected?*selected:region_selection,witness.flags());
        const bool content_history=edit.source_history;edit.reverse=!edit.reverse;
        if(offset){
            Revision next{DocumentData{},source.back().label,revision_,selection,std::move(edit),frame_content};
            destination.reserve(destination.size()+1);
            if(!witness.matches(*this)||(indication_data&&!modal_detail::data_same(data,*indication_data)))throw std::logic_error("重放浮动位移选区指示期间画稿已改变。");
            require_no_floating_transform();
            destination.push_back(std::move(next));
        }else destination.push_back({DocumentData{},source.back().label,revision_,selection,std::move(edit),frame_content});
        if(exchange_data){
            std::swap(destination.back().data,data);
            if(scoped_data)data=std::move(*scoped_data);else std::swap(data,source.back().data);
            data.original=destination.back().data.original;data.original_content=destination.back().data.original_content;
            publish_vector_properties(data,vector_properties);
            preserve_selection(destination.back().data,data);
            preserve_layer_properties(destination.back().data,data,!frame_content);
        }
        if(prepared){if(prepared->redraw)std::swap(data,prepared->data);vector_indication_active=prepared->active;}
        floating_selection_=std::move(floating);++floating_selection_generation_;if(selected)set_region_selection(std::move(*selected));
        if(content_history)runtime_dirty_=true;
    }else if(const auto* selected=std::get_if<RegionSelectionEdit>(&runtime)){
        auto edit=*selected;const bool paired=!source.back().data.layers.empty();
        const SelectionIndicationWitness witness(*this);
        PreservedVectorColors vector_properties;
        if(paired){require_selection_layout(data,source.back().data,selection);
            if(!edit.secondary)throw std::logic_error("Paired raster/selection history lacks its prepared opposite");
            vector_properties=prepare_vector_properties(data,source.back().data);}
        if(!edit.secondary)edit.secondary=selection_history_snapshot(data,region_selection);
        auto candidate=edit.reverse?*edit.secondary:edit.primary;
        // Selection-only native commands normalize saved states. Paired
        // raster-storage history instead preserves the exact immutable bitmap
        // topology/state; it is an explicit modern atomic replay policy.
        if(!paired)candidate=restore_selection_history(data,candidate,region_selection,
            application_vector_selection_flags.value_or(region_selection.vector_flags));
        std::optional<DocumentData> indication_data;std::optional<VectorIndicationUpdate> prepared;
        if(shared_budget&&!paired){
            modal_detail::data_charge(data,shared_budget->bytes,0,false);auto refreshed=data;
            const bool mask=candidate.state==SelectionLayerState::Mask||candidate.state==SelectionLayerState::VectorIndicated;
            const double dpi=mask&&!(witness.flags()&1u)?selection_carrier_dpi(candidate,data.base.dpi):0;
            const bool active=vector_indication_detail::refresh_marks(refreshed,witness.active,candidate,witness.flags(),dpi,shared_budget->bytes,shared_budget->work,shared_budget->stop);
            prepared=VectorIndicationUpdate{std::move(refreshed),active,witness.active||active};
        }else{indication_data=indication_source(data,!paired&&needs_selection_indications(witness.active,candidate,witness.flags()));
            prepared=prepare_selection_indications(indication_data,witness.active,candidate,witness.flags());}
        edit.reverse=!edit.reverse;
        Revision next{DocumentData{},source.back().label,revision_,selection,std::move(edit),frame_content};
        destination.reserve(destination.size()+1);
        if(!witness.matches(*this)||(indication_data&&!modal_detail::data_same(data,*indication_data)))throw std::logic_error("重放选区指示期间画稿已改变。");
        require_no_floating_transform();
        destination.push_back(std::move(next));
        if(paired){std::swap(destination.back().data,data);std::swap(data,source.back().data);
            data.original=destination.back().data.original;data.original_content=destination.back().data.original_content;
            publish_vector_properties(data,vector_properties);preserve_selection(destination.back().data,data);preserve_layer_properties(destination.back().data,data,!frame_content);}
        if(prepared){if(prepared->redraw)std::swap(data,prepared->data);vector_indication_active=prepared->active;}
        set_region_selection(std::move(candidate));
    }else{
        require_selection_layout(data,source.back().data,selection);
        auto vector_properties=selection==HistorySelection::PreserveCurrent?prepare_vector_properties(data,source.back().data):PreservedVectorColors{};
        const auto* attached=std::get_if<AttachedLayerMoveEdit>(&runtime);
        RuntimeHistory opposite;if(std::holds_alternative<StackEdit>(runtime))opposite=StackEdit{};if(attached)opposite=AttachedLayerMoveEdit{attached_temporary_layers_,attached->refresh_indications,attached->restore_attached};
        destination.push_back({DocumentData{},source.back().label,revision_,selection,std::move(opposite),frame_content});
        std::swap(destination.back().data,data);
        std::swap(data,source.back().data);
        data.original=destination.back().data.original;data.original_content=destination.back().data.original_content;
        publish_vector_properties(data,vector_properties);
        if(attached&&attached->restore_attached)publish_attached_temporary_layers(attached->opposite);
        if(selection==HistorySelection::PreserveCurrent){preserve_selection(destination.back().data,data);preserve_layer_properties(destination.back().data,data,!frame_content);}
        else if(std::holds_alternative<StackEdit>(runtime)){
            // Common properties belong to surviving objects, independently of topology/payload history.
            for(auto& layer:data.layers)for(auto& live:destination.back().data.layers)if(layer.common.runtime_identity==live.common.runtime_identity){
                layer.common.name.swap(live.common.name);layer.common.opacity=live.common.opacity;layer.common.opacity_mode=live.common.opacity_mode;layer.common.flags[1]=live.common.flags[1];layer.visibility_modified=live.visibility_modified;
                for(auto& p:layer.planes)for(auto& q:live.planes)if(p.common.runtime_identity==q.common.runtime_identity){p.common.name.swap(q.common.name);p.common.color=q.common.color;p.common.opacity=q.common.opacity;p.common.opacity_mode=q.common.opacity_mode;p.common.flags[1]=q.common.flags[1];p.visibility_modified=q.visibility_modified;}
            }
        }
        else if(!frame_content)preserve_frame_properties(destination.back().data,data);
        else {
            const auto& live=destination.back().data;
            if(live.layers.size()==data.layers.size())for(std::size_t i=0;i<data.layers.size();++i)
                if(live.layers[i].family==data.layers[i].family&&live.layers[i].planes.size()==data.layers[i].planes.size())
                    for(std::size_t j=0;j<data.layers[i].planes.size();++j){auto& p=data.layers[i].planes[j];const auto& q=live.layers[i].planes[j];
                        if(p.family==q.family&&(p.family==0x40001008||p.family==0x40002008||p.family==0x40000080)){p.frame_modified=q.frame_modified;p.vanishing_modified=q.vanishing_modified;}}
        }
    }
    if(std::holds_alternative<std::monostate>(runtime)){
        const auto& live=destination.back().data;
        for(std::size_t l=0;l<std::min(live.layers.size(),data.layers.size());++l)if(live.layers[l].common.runtime_identity==data.layers[l].common.runtime_identity)
            for(std::size_t p=0;p<std::min(live.layers[l].planes.size(),data.layers[l].planes.size());++p)if(live.layers[l].planes[p].common.runtime_identity==data.layers[l].planes[p].common.runtime_identity)
                data.layers[l].planes[p].raster_modified=live.layers[l].planes[p].raster_modified;
    }
    if(paint_history_manager_){
        bool update=false,force=false;
        if(std::holds_alternative<AddLayerEdit>(runtime)||std::holds_alternative<DeleteLayersEdit>(runtime)){update=true;}
        else if(std::holds_alternative<AddPlaneEdit>(runtime)||std::holds_alternative<DeletePlanesEdit>(runtime)||std::holds_alternative<StackEdit>(runtime)||std::holds_alternative<PageTransformEdit>(runtime)||std::holds_alternative<AttachedLayerMoveEdit>(runtime)||std::holds_alternative<std::monostate>(runtime)){update=true;force=true;}
        else if(const auto* order=std::get_if<OrderEdit>(&runtime)){update=true;force=bool(order->layer);}
        else if(const auto* floating=std::get_if<FloatingSelectionEdit>(&runtime)){update=floating->exchange_data&&floating->mode!=FloatingSelectionEdit::Mode::Offset;force=update;}
        else if(std::holds_alternative<RegionSelectionEdit>(runtime)&&!destination.back().data.layers.empty()){update=true;force=true;}
        if(update){raster_history_detail::Budget local;auto& budget=shared_budget?*shared_budget:local;
            if(!force&&mapped_manager_owner)paint_history_manager_=relocate_paint_manager(paint_history_manager_,data,*mapped_manager_owner,budget);
            if(!mapped_manager_owner)force=true;paint_history_manager_=prepare_paint_manager(paint_history_manager_,data,force,budget);}
    }
    if(const auto* move=std::get_if<AttachedLayerMoveEdit>(&runtime);move&&move->refresh_indications){raster_history_detail::Budget local;auto& budget=shared_budget?*shared_budget:local;const auto flags=application_vector_selection_flags.value_or(region_selection.vector_flags);const bool mask=region_selection.state==SelectionLayerState::Mask||region_selection.state==SelectionLayerState::VectorIndicated;vector_indication_active=vector_indication_detail::refresh_marks(data,vector_indication_active,region_selection,flags,mask&&!(flags&1u)?selection_carrier_dpi(region_selection,data.base.dpi):0,budget.bytes,budget.work,budget.stop);}

    revision_=source.back().id;source.pop_back();++state_generation_;
}
void Document::undo_or_redo(){if(undo_next_)undo();else redo();}
void Document::undo(){require_no_floating_transform();const auto generation=state_generation_;replay(undo_,redo_);if(state_generation_!=generation)undo_next_=false;}
void Document::trim_history(){
    if(!collecting_history_&&!building_clipboard_group_&&undo_.size()>history_limit_)
        undo_.erase(undo_.begin(),undo_.end()-history_limit_);
}
void Document::set_history_limit(std::size_t count){
    if(count>10000)throw std::invalid_argument("History limit exceeds 10000");
    if(count==history_limit_)return;
    history_limit_=count;undo_.clear();redo_.clear();undo_next_=true;
}
void Document::redo(){require_no_floating_transform();const auto generation=state_generation_;replay(redo_,undo_);if(state_generation_!=generation)undo_next_=true;}
void Document::save(const std::filesystem::path& destination,SavePolicy policy,std::optional<int> compression_level,bool v5_compatible){require_no_floating_transform();
    if(policy!=SavePolicy::ExactRaw&&compression_level&&(*compression_level < -1||*compression_level > 9))throw std::invalid_argument("Invalid native compression level");
    if(policy==SavePolicy::ExactRaw&&has_floating_selection())throw std::logic_error("原始字节另存不能包含未保存的浮动图像。");
    if(policy==SavePolicy::ExactRaw){
        // Raw export does not inspect/normalize decoded storage or impose the
        // canonical snapshot admission on an otherwise eligible original.
        const auto generation=state_generation_;const auto original=data.original;
        const auto output=encode_cel(data,policy,can_save_exact());const auto target=destination;
        storage_detail::write_checked(target,output,[&]{if(state_generation_!=generation||data.original!=original||!original||*original!=output||!can_save_exact())throw std::logic_error("Document changed during raw save preparation");});
        return;
    }
    // Check the requested quality route before settlement can change live state.
    if(v5_compatible)(void)encode_cel(data,policy,false,compression_level,true);
    finalize_floating_selection();
    // Save owns a private backing snapshot; canonical Encode remains pure,
    // while successful live Save installs the optimized candidate as native
    // sub_63E790 does. All candidate allocations precede the file replacement.
    const auto generation=state_generation_;const auto old_path=path;
    std::uint64_t allocation{};modal_detail::data_charge(data,allocation);
    const auto before=modal_detail::frozen(data,{});auto staged=before;
    if(policy==SavePolicy::LegacyCanonical){
        auto extension=destination.extension().string();
        std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
        staged.dga=extension==".dga";
        storage_detail::optimize(staged);
    }
    auto output=std::make_shared<const Bytes>(encode_cel(staged,policy,policy==SavePolicy::ExactRaw&&can_save_exact(),compression_level,v5_compatible));
    auto prepared_path=destination;
    auto live=staged;
    staged.original.reset();staged.original_content.reset();
    auto baseline_data=modal_detail::frozen(staged,{});baseline_data.original=output;
    auto baseline=std::make_shared<const DocumentData>(std::move(baseline_data));
    storage_detail::write_checked(prepared_path,*output,[&]{
        require_no_floating_transform();
        if(state_generation_!=generation||path!=old_path||!modal_detail::data_same(data,before))throw std::logic_error("Document changed during save preparation");
    });
    if(policy==SavePolicy::LegacyCanonical){
        static_assert(std::is_nothrow_swappable_v<DocumentData>);std::swap(data,live);
        mark_saved(std::move(prepared_path),std::move(output),std::move(baseline));
    }
}
void Document::mark_saved(std::filesystem::path destination,std::shared_ptr<const Bytes> original,std::shared_ptr<const DocumentData> baseline){require_no_floating_transform();
    path.swap(destination);saved_=revision_;runtime_dirty_=false;document_modify_count_=0;
    for(auto& layer:data.layers){layer.visibility_modified=false;for(auto& plane:layer.planes){plane.visibility_modified=false;plane.frame_modified=false;plane.vanishing_modified=false;plane.raster_modified=false;}}
    data.original.swap(original);data.original_content=baseline;raw_baseline_.swap(baseline);++state_generation_;
}
}
