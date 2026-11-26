#include "retas/selection_color.hpp"
#include "selection_color_command.hpp"
#include "retas/document.hpp"
#include "retas/fill.hpp"
#include "retas/raster_stroke_pixels.hpp"
#include "selection_color_math.hpp"
#include "selection_mask_source.hpp"
#include "selection_vector_render.hpp"
#include "vector_material_edit.hpp"
#include "vector_region_probe.hpp"
#include "vector_indication.hpp"
#include "modal_document_guard.hpp"
#include "retas/vector_color_metadata.hpp"
#include "../io/vector_line_projection.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <optional>

namespace retas {
namespace {
using vector_render_detail::StoredLineCacheCosts;
using vector_probe_detail::Budget;
constexpr std::uint64_t storage_limit=512ull*1024*1024;
bool valid_command(SelectionColorCommand command){return command>=SelectionColorCommand::Color&&command<=SelectionColorCommand::ColorSpace;}
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Vector color selection cancelled");}
void reserve(std::uint64_t& storage,std::uint64_t n){if(storage>storage_limit||n>storage_limit-storage)throw std::length_error("Vector color selection exceeds512MiB storage");storage+=n;}
std::uint64_t mask_bytes(int width,int height){return sizeof(BinaryMask)+((std::uint64_t(width)+31)/32)*height*4+((std::uint64_t(width)+255)/256)*((std::uint64_t(height)+255)/256);}
int integer(std::int64_t value){if(value<INT32_MIN||value>INT32_MAX)throw std::invalid_argument("Vector color selection coordinate exceeds signed32");return int(value);}
bool line_plane(std::uint32_t family){return family==0x21110001||family==0x21120001||family==0x21210001||family==0x21220001;}
bool paint_owner(std::uint32_t family){return family==0x21200000||family==0x20800000;}
template<class T> auto index(const std::vector<VectorPoolRecord<T>>& pool){std::map<int,const T*> result;for(const auto& r:pool)result.emplace(r.index,&r.value);return result;}
template<class T> auto index(std::vector<VectorPoolRecord<T>>& pool){std::map<int,T*> result;for(auto& r:pool)result.emplace(r.index,&r.value);return result;}
template<class T> T& live(const std::map<int,T*>& pool,int id){const auto it=pool.find(id);if(it==pool.end())throw std::invalid_argument("Dead vector color selection reference");return *it->second;}
// Unlike a sampled Face material, this predicate needs neither a dense image
// nor a valid texture transform. Preflight sparse framing before decompression.
bool texture_space(const Bytes& bytes,StoredLineCacheCosts& costs,std::stop_token stop){
    const auto texture=decode_vector_texture(bytes);Reader image(texture.offscreen);auto outer=image.section();if(!outer.remaining())throw std::invalid_argument("Empty vector color texture image framing");
    auto header=outer.section();const auto version=header.u16();const auto width=header.u32(),height=header.u32(),depth=header.u32();
    const auto dpi=header.f64();const auto count=header.u32();const auto order=header.u8();
    if((version>>8)>1||!width||!height||width>100000||height>100000||std::uint64_t(width)*height>268435456
        ||(depth!=1&&depth!=2&&depth!=8&&depth!=16&&depth!=24&&depth!=32&&depth!=64)
        ||order!=(depth==24?4:depth>=32?3:0)||!std::isfinite(dpi)||dpi<=0||count>100000)throw std::invalid_argument("Invalid vector color texture image");
    auto temporary=costs.storage;reserve(temporary,texture.offscreen.size()+texture.metadata.size()+texture.faces.size()*4+texture.suffix.size());
    if(count){auto extent=outer.section();const auto left=extent.i32(),top=extent.i32(),right=extent.i32(),bottom=extent.i32();const bool compressed=extent.u8()!=0;
        if(left<0||top<0||left%256||top%256||right<=left||bottom<=top||right>int(width)+255||bottom>int(height)+255
            ||std::uint64_t((right-left+255)/256)*((bottom-top+255)/256)!=count)throw std::invalid_argument("Invalid vector color texture extent");
        auto blocks=outer.section();const auto tile_bytes=65536ull*depth/8;Budget budget{costs.work,stop};
        for(unsigned i=0;i<count;++i){budget.charge();const auto present=blocks.u8();if(!present)continue;if(present!=1||blocks.u32()!=tile_bytes)throw std::invalid_argument("Invalid vector color texture tile");
            reserve(temporary,tile_bytes+128);budget.charge(65536+tile_bytes);blocks.take(compressed?blocks.u32():std::size_t(tile_bytes));}
    }
    poll(stop);Reader reader(texture.offscreen);const auto raster=decode_offscreen(reader);if(reader.remaining())throw std::invalid_argument("Invalid vector color texture framing");
    for(const auto& [tile,bytes]:raster.tiles)for(int y=tile.y*256;y<std::min(raster.height,(tile.y+1)*256);++y){poll(stop);for(int x=tile.x*256;x<std::min(raster.width,(tile.x+1)*256);++x){
        auto p=raster_stroke_surface_pixel(raster,x,y);p=Pixel::rgba8(p.r>>8,p.g>>8,p.b>>8,p.a>>8);
        if(selection_color_detail::outside_space(p,32))return true;
    }}
    poll(stop);return false;
}
struct FacePredicates {
    const VectorPaintArchive& archive;
    std::map<int,const Bytes*> gradients,nodes,textures;
    std::map<int,bool> gradient_results,texture_results;
    StoredLineCacheCosts& costs;std::stop_token stop;bool stops_validated{};
    explicit FacePredicates(const VectorPaintArchive& a,StoredLineCacheCosts& c,std::stop_token s):archive(a),gradients(index(a.gradations)),nodes(index(a.gradnodes)),textures(index(a.textures)),costs(c),stop(s){}
    void validate_stops(){
        if(stops_validated)return;std::set<int> owned;Budget budget{costs.work,stop};
        //Stop next/prev links are installed during native load for every
        //gradation. Prove structural ownership without interpreting positions.
        for(const auto& record:archive.gradations){Reader reader(record.value);reader.u32();const auto n=reader.i32();reader.require_count(n,4);for(int i=0;i<n;++i){budget.charge();const int id=reader.i32();(void)live(nodes,id);if(!owned.insert(id).second)throw std::invalid_argument("Shared intrusive vector gradient stop");}}
        stops_validated=true;
    }
    bool outside(const VectorArchiveFace& face){
        Budget budget{costs.work,stop};budget.charge();
        if(face.flags&1u)return selection_color_detail::outside_space(selection_color_detail::unpack(face.color),64);
        //Unknown low nibbles leave the native pointer/color union zero at
        //load. Its default material integer0 represents NULL, not pool ID0.
        if((face.flags&15u)==2u){if(face.material<0)return false;validate_stops();const auto found=gradient_results.find(face.material);if(found!=gradient_results.end())return found->second;
            const auto gradient=decode_vector_gradation(live(gradients,face.material));bool hit{};for(int id:gradient.stops){budget.charge();if(selection_color_detail::outside_space(selection_color_detail::unpack(decode_vector_gradnode(live(nodes,id)).color),64)){hit=true;break;}}
            gradient_results.emplace(face.material,hit);return hit;
        }
        if((face.flags&15u)==4u){if(face.material<0)return false;const auto found=texture_results.find(face.material);if(found!=texture_results.end())return found->second;
            const auto hit=texture_space(live(textures,face.material),costs,stop);texture_results.emplace(face.material,hit);return hit;}
        return false;
    }
};
VectorBounds draw_clip(VectorBounds raw,double ratio,std::array<int,2> translation,const RegionSelection& selection,int width,int height){
    const auto edge=[&](int value,int axis,int limit){const auto projected=std::trunc(numeric_detail::binary64(double(value)*ratio));if(!std::isfinite(projected)||projected<INT32_MIN||projected>INT32_MAX)throw std::invalid_argument("Vector color selection saved bbox exceeds signed32");
        const auto n=std::int64_t(projected)+translation[axis];(void)integer(n);return int(std::clamp<std::int64_t>(n,std::clamp<std::int64_t>(-std::int64_t(selection.origin[axis]),0,limit),std::clamp<std::int64_t>(std::int64_t(limit)-selection.origin[axis],0,limit)));};
    VectorBounds result{edge(raw.left,0,width),edge(raw.top,1,height),edge(raw.right,0,width),edge(raw.bottom,1,height)};
    result.right=std::max(result.left,result.right);result.bottom=std::max(result.top,result.bottom);return result;
}
}
bool can_select_vector_color_bitmap(const DocumentData& data,SelectionColorCommand command) noexcept{
    if(!valid_command(command)||data.current_layer<0||std::size_t(data.current_layer)>=data.layers.size())return false;
    const auto& owner=data.layers[std::size_t(data.current_layer)];const auto* plane=data.active_plane();
    return owner.common.flags[1]&&!(owner.family&0x40000000u)&&plane&&plane->common.flags[1]&&(plane->family&0xf0000000u)==0x20000000u
        &&(command!=SelectionColorCommand::ColorSpace||plane->common.opacity_mode!=4);
}
bool can_select_vector_color_primitives(const DocumentData& data,SelectionColorCommand command) noexcept{
    return can_select_vector_color_bitmap(data,command);
}
static VectorColorPrimitiveSelection primitive_color_impl(const DocumentData& data,bool active,
    const RegionSelection& current,SelectionColorCommand command,Pixel reference,bool additive_space,std::stop_token stop,bool after_admission){
    if(!valid_command(command))throw std::invalid_argument("Invalid vector color selection command");poll(stop);
    std::uint64_t metadata{};modal_detail::data_charge(data,metadata);
    StoredLineCacheCosts costs{};reserve(costs.storage,metadata*3);Budget budget{costs.work,stop};budget.charge(metadata);
    const bool admitted=after_admission||can_select_vector_color_primitives(data,command);
    if(!admitted){poll(stop);return {{data,active,false},current};}
    const auto& source=data.layers[std::size_t(data.current_layer)];const auto& source_plane=*data.active_plane();
    if(source.opaque||source_plane.opaque||source_plane.raster||(!paint_owner(source.family)&&source.family!=0x22100000
        &&source.family!=0x24400000&&source.family!=0x20000200)||source.planes.size()>1024
        ||(!line_plane(source_plane.family)&&source_plane.family!=0x22100001&&source_plane.family!=0x22100002))
        throw std::invalid_argument("Unsupported vector color primitive source");
    std::uint64_t wire=source.specific.size();
    for(const auto& plane:source.planes){poll(stop);if(plane.specific.size()>8ull*1024*1024||wire>8ull*1024*1024-plane.specific.size())throw std::length_error("Vector color primitive source exceeds8MiB");wire+=plane.specific.size();}
    if(wire>8ull*1024*1024)throw std::length_error("Vector color primitive source exceeds8MiB");
    reserve(costs.storage,wire*32);budget.charge(wire*8);
    VectorColorPrimitiveSelection result{{data,true,true},current};
    const bool add=command==SelectionColorCommand::ColorAdd||(command==SelectionColorCommand::ColorSpace&&additive_space);
    if(!add){
        vector_indication_detail::clear_marks(result.indications.data,active,costs.storage,costs.work,stop);
        auto size=current.carrier_size;if(size==std::array<int,2>{}&&current.mask)size={current.mask->width(),current.mask->height()};
        result.selection=cleared_selection({},size);result.selection.carrier_dpi=current.carrier_dpi;result.selection.vector_flags=current.vector_flags;
    }
    auto& owner=result.indications.data.layers[std::size_t(data.current_layer)];auto& plane=owner.planes[std::size_t(owner.current_plane)];
    std::optional<VectorPaintArchive> paint;VectorLineArchive ordinary;
    if(paint_owner(owner.family))paint=decode_vector_paint_archive(owner);else ordinary=decode_vector_line_archive(owner);
    auto& base=paint?paint->base:ordinary;bool hit{};
    const auto packed=selection_color_detail::packed(reference);const bool space=command==SelectionColorCommand::ColorSpace;
    if(line_plane(plane.family)){
        const auto lines=index(base.lines);const auto models=index(base.models);
        for(int id:vector_line_plane_references(plane)){budget.charge();const auto& line=live(lines,id);
            const bool matches=space?selection_color_detail::outside_space(selection_color_detail::unpack(line.color),64)
                :command==SelectionColorCommand::NotColor?line.color!=packed:line.color==packed;
            if(!matches)continue;hit=true;
            for(auto member:line.members){budget.charge();live(models,member.model).flags|=0x10000u;}
        }
    }else{
        if(!paint)throw std::invalid_argument("Face color primitive lacks Paint archive");
        auto faces=index(paint->faces);FacePredicates predicates(*paint,costs,stop);
        Reader list(plane.specific);const int count=list.i32();list.require_count(count,4,100000);
        for(int i=0;i<count;++i){budget.charge();auto& face=live(faces,list.i32());
            const bool matches=space?predicates.outside(face):command==SelectionColorCommand::NotColor
                ?!(face.flags&1u)||face.color!=packed:(face.flags&1u)&&face.color==packed;
            if(matches){face.flags|=0x10000u;hit=true;}
        }
    }
    if(paint)store_vector_paint_archive(owner,*paint);else owner.specific=encode_vector_line_archive(ordinary);
    plane.common.flags[3]=hit?1:0;owner.common.flags[3]=0;
    for(const auto& child:owner.planes){budget.charge();if(child.common.flags[3]){owner.common.flags[3]=1;break;}}
    poll(stop);return result;
}
static RegionSelection bitmap_color_impl(const DocumentData& data,const RegionSelection& current,SelectionColorCommand command,Pixel reference,bool additive_space,bool minimum_width,std::stop_token stop,bool admitted,selection_color_detail::BitmapAction* action){
    if(!valid_command(command))throw std::invalid_argument("Invalid vector color selection command");poll(stop);if(!admitted&&!can_select_vector_color_bitmap(data,command))return current;
    const auto& owner=data.layers[std::size_t(data.current_layer)];const auto& plane=*data.active_plane();const auto plane_index=std::size_t(owner.current_plane);
    if(owner.opaque||plane.opaque||plane.raster||(!paint_owner(owner.family)&&owner.family!=0x22100000&&owner.family!=0x24400000&&owner.family!=0x20000200)
        ||(!line_plane(plane.family)&&plane.family!=0x22100001&&plane.family!=0x22100002)||owner.planes.size()>1024
        ||!std::isfinite(owner.common.resolution)||owner.common.resolution<=0||!std::isfinite(plane.common.resolution)||plane.common.resolution<=0
        ||!std::isfinite(data.base.dpi)||data.base.dpi<=0)throw std::invalid_argument("Unsupported vector color selection source");
    const int width=data.base.width(),height=data.base.height();const auto size=selection_carrier_size(current,width,height);
    (void)selection_carrier_size(cleared_selection(),width,height);
    const bool add=command==SelectionColorCommand::ColorAdd||(command==SelectionColorCommand::ColorSpace&&additive_space);
    auto previous=add?current:cleared_selection({},size);previous.vector_flags=current.vector_flags;previous.carrier_dpi=current.carrier_dpi;
    const std::array<int,2> translation{integer(std::int64_t(owner.common.x)-previous.origin[0]),integer(std::int64_t(owner.common.y)-previous.origin[1])};
    const std::array<int,2> raw_offset{integer(-std::int64_t(translation[0])*16),integer(-std::int64_t(translation[1])*16)};
    const auto ratio=numeric_detail::binary64(owner.common.resolution/plane.common.resolution);
    if(!std::isfinite(ratio)||ratio<=0)throw std::invalid_argument("Invalid vector color selection DPI ratio");
    std::uint64_t wire=owner.specific.size();for(const auto& child:owner.planes){poll(stop);if(child.specific.size()>8ull*1024*1024||wire>8ull*1024*1024-child.specific.size())throw std::length_error("Vector color selection source exceeds8MiB");wire+=child.specific.size();}
    if(wire>8ull*1024*1024)throw std::length_error("Vector color selection source exceeds8MiB");
    StoredLineCacheCosts costs{wire*32,wire};reserve(costs.storage,2*mask_bytes(width,height)+3*mask_bytes(size[0],size[1]));
    Budget budget{costs.work,stop};budget.charge(std::uint64_t(width)*height);
    BinaryMask scratch(width,height);bool matched{};const auto packed=selection_color_detail::packed(reference);const bool space=command==SelectionColorCommand::ColorSpace;
    const auto color_matches=[&](std::uint64_t c){return space?selection_color_detail::outside_space(selection_color_detail::unpack(c),64):(command==SelectionColorCommand::NotColor?c!=packed:c==packed);};
    if(line_plane(plane.family)){
        std::optional<VectorPaintArchive> paint;VectorLineArchive ordinary;
        if(paint_owner(owner.family))paint=decode_vector_paint_archive(owner);else ordinary=decode_vector_line_archive(owner);
        const auto& archive=paint?paint->base:ordinary;
        vector_archive_detail::validate_stored_line_attachments(archive);
        const auto refs=validated_vector_plane_line_references(owner,plane_index);const auto lines=index(archive.lines);const auto models=index(archive.models);
        std::vector<int> selected;std::vector<VectorBounds> clips;
        for(int id:refs){budget.charge();const auto& line=live(lines,id);if(!color_matches(line.color))continue;matched=true;
            const auto clip=draw_clip(line.archived_bounds,ratio,translation,previous,width,height);
            //676DB0 skips59C2F0 entirely when the saved bbox is offscreen.
            //Matched is still true, but width/SC/CP rendering is unreachable.
            if(clip.left<clip.right&&clip.top<clip.bottom){selected.push_back(id);clips.push_back(clip);}}
        std::uint64_t spans{},width_nodes{};const auto curves=index(archive.subcurves);
        reserve(costs.storage,selected.size()*(sizeof(VectorLineStroke)+sizeof(VectorBounds)));
        for(int id:selected)for(auto member:live(lines,id).members){budget.charge();const auto& model=live(models,member.model);
            if(model.subcurves.size()>100000-spans)throw std::length_error("Vector color selection exceeds100000 expanded spans");spans+=model.subcurves.size();
            for(int sub:model.subcurves){const auto n=live(curves,sub).scales.size();if(n>100000-width_nodes)throw std::length_error("Vector color selection exceeds100000 expanded width nodes");width_nodes+=n;}}
        reserve(costs.storage,2*(spans*(sizeof(VectorLineSpan)+sizeof(VectorSpanProfiles)+128)+width_nodes*sizeof(VectorWidthNode)));
        vector_render_detail::StoredLinePlane source;
        source.lines.strokes=vector_archive_detail::project_line_strokes(archive,selected,vector_archive_detail::LineClosurePolicy::NativeStored,&source.line_profiles);
        for(int id:selected){const auto& line=live(lines,id);source.line_bounds.push_back(line.archived_bounds);
            for(auto member:line.members){budget.charge();const auto& s=live(models,member.model).scalars;source.model_bounds.emplace(member.model,VectorBounds{s[4],s[5],s[6],s[7]});}}
        for(std::size_t i=0;i<source.lines.strokes.size();++i){budget.charge();if(!color_matches(source.lines.strokes[i].color))continue;matched=true;
            const auto clip=clips.at(i);
            const VectorRenderContext context{width,height,1,data.base.dpi,16,{-double(translation[0]),-double(translation[1])},clip,1};
            vector_render_detail::draw_selection_line_bits(source,i,context,scratch,minimum_width,costs,stop);
        }
    }else{
        const auto archive=decode_vector_paint_archive(owner);const auto faces=index(archive.faces);const auto regions=index(archive.regions);
        std::array<std::set<int>,3> owned;
        const auto gradients=index(archive.gradations),textures=index(archive.textures);
        for(const auto& child:owner.planes)if(child.family==0x22100001||child.family==0x22100002){
            if(child.opaque||child.raster)throw std::invalid_argument("Unsupported vector color Paint sibling");
            const vector_material_detail::PaintLists member_lists(child);
            for(std::size_t k=0;k<3;++k)for(int id:member_lists.ids[k]){budget.charge();if(k==0)(void)live(faces,id);else if(k==1)(void)live(gradients,id);else (void)live(textures,id);
                if(!owned[k].insert(id).second)throw std::invalid_argument("Shared vector color Paint plane member");}}
        const vector_material_detail::PaintLists lists(plane);vector_probe_detail::PaintGraph graph(archive,budget);FacePredicates predicates(archive,costs,stop);
        for(int id:lists.ids[0]){budget.charge();const auto& face=live(faces,id);const bool hit=space?predicates.outside(face):command==SelectionColorCommand::NotColor?!(face.flags&1u)||face.color!=packed:(face.flags&1u)&&face.color==packed;
            if(!hit)continue;matched=true;const auto& region=live(regions,face.region);const auto clip=draw_clip(region.outer.archived_bounds,ratio,translation,previous,width,height);
            if(clip.left>=clip.right||clip.top>=clip.bottom)continue;
            auto temporary_storage=costs.storage;graph.stencil_into(region,clip,16,raw_offset,numeric_detail::binary64(data.base.dpi*16),scratch,temporary_storage,budget);
        }
    }
    if(!matched){poll(stop);return previous;}
    const auto extent=scratch.extent({0,0,width,height});
    if(extent.width<=0||extent.height<=0){poll(stop);return previous;}
    if(action)*action=selection_color_detail::apply_action(previous,add,width,height,extent);
    //GetExtentRect is nested under the outer write lock: every page block is
    //present before the tight rectangle reaches Copy1/CArea. No Optimize.
    for(int ty=0;ty*256<height;++ty)for(int tx=0;tx*256<width;++tx){budget.charge();scratch.materialize_block(tx,ty);}
    budget.charge(std::uint64_t(extent.width)*extent.height+std::uint64_t(size[0])*size[1]);
    auto result=selection_detail::combine_mask_source(previous,add?SelectionCombine::Add:SelectionCombine::Replace,width,height,extent,scratch);
    result.vector_flags=current.vector_flags;poll(stop);return result;
}
VectorColorPrimitiveSelection prepare_vector_color_primitive_selection(const DocumentData& data,bool active,const RegionSelection& current,SelectionColorCommand command,Pixel reference,bool additive_space,std::stop_token stop){
    return primitive_color_impl(data,active,current,command,reference,additive_space,stop,false);
}
VectorColorPrimitiveSelection selection_color_detail::primitives_after_admission(const DocumentData& data,bool active,const RegionSelection& current,SelectionColorCommand command,Pixel reference,bool additive_space,std::stop_token stop){
    return primitive_color_impl(data,active,current,command,reference,additive_space,stop,true);
}
RegionSelection select_vector_color_bitmap(const DocumentData& data,const RegionSelection& current,SelectionColorCommand command,Pixel reference,bool additive_space,bool minimum_width,std::stop_token stop){
    return bitmap_color_impl(data,current,command,reference,additive_space,minimum_width,stop,false,nullptr);
}
selection_color_detail::BitmapSelection selection_color_detail::vector_after_admission(const DocumentData& data,const RegionSelection& current,SelectionColorCommand command,Pixel reference,bool additive_space,bool minimum_width,std::stop_token stop){
    BitmapSelection result;result.selection=bitmap_color_impl(data,current,command,reference,additive_space,minimum_width,stop,true,&result.action);return result;
}

}
