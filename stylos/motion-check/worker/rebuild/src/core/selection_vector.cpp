#include "selection_vector.hpp"
#include "vector_graph_edit.hpp"
#include "vector_eraser_point_trim.hpp"
#include "vector_boundary_source_copy.hpp"
#include "vector_line_selection.hpp"
#include "retas/vector_color_metadata.hpp"
#include "vector_tool_output_admission.hpp"
#include "vector_tool_selection.hpp"
#include "vector_grouped_face_delete.hpp"
#include "vector_combined_clear.hpp"
#include "vector_material_geometry.hpp"
#include "vector_region_probe.hpp"
#include "vector_line_paint_convert.hpp"
#include "retas/vector_extension_archive.hpp"

namespace retas::selection_detail {
namespace {
using namespace vector_graph_detail;
bool vertex(const Plane& p){return !p.opaque&&!p.raster&&(p.family==0x21210001||p.family==0x21220001);}
bool paint(const Layer& l){return l.family==0x21200000||l.family==0x20800000;}
bool pure(const Plane& p){return !p.opaque&&!p.raster&&(p.family==0x21110001||p.family==0x21120001);}
bool paint_plane(const Plane& p){return !p.opaque&&!p.raster&&(p.family==0x22100001||p.family==0x22100002);}
std::uint64_t count(const VectorLineArchive& a){return a.vertices.size()+a.scales.size()+a.subcurves.size()+a.models.size()+a.crosspoints.size()+a.lines.size();}
std::uint64_t references(const Layer& l){
    VectorPaintArchive p;VectorLineArchive a;if(paint(l)){p=decode_vector_paint_archive(l);a=p.base;}else a=decode_vector_line_archive(l);
    std::uint64_t refs{};for(const auto& v:a.vertices)refs+=v.value.subcurves.size();for(const auto& s:a.subcurves)refs+=s.value.scales.size();
    for(const auto& m:a.models)refs+=m.value.subcurves.size()+std::ranges::count_if(m.value.crosspoints,[](int id){return id>=0;});for(const auto& line:a.lines)refs+=line.value.members.size();
    if(paint(l)){refs+=p.border.vertices.size()+p.border.models.size()+p.border.regions.size();for(const auto& loop:p.loops)refs+=loop.value.members.size();
        for(const auto& r:p.regions)refs+=r.value.outer.members.size()+r.value.holes.size()+r.value.faces.size();
        for(const auto& g:p.gradations){const auto leaf=decode_vector_gradation(g.value);refs+=leaf.faces.size()+leaf.stops.size();}
        for(const auto& t:p.textures)refs+=decode_vector_texture(t.value).faces.size();}
    for(const auto& child:l.planes){if(paint(l)){if(pure(child))refs+=vector_line_plane_references(child).size();else{const vector_material_detail::PaintLists list(child);for(const auto& ids:list.ids)refs+=ids.size();}}
        else{const auto list=vector_line_plane_topology(child);refs+=list.vertices.size()+list.models.size()+list.lines.size();}}
    return refs;
}
struct Ledger {
    std::uint64_t storage{},work{},wire{},nodes{},refs{};
    std::uint64_t initial_storage{},initial_work{};
    std::uint64_t& remaining_bytes;std::uint64_t& remaining_work;
    Ledger(std::uint64_t& b,std::uint64_t& w):storage(512ull*1024*1024-b),work(128ull*1024*1024-w),initial_storage(storage),initial_work(work),remaining_bytes(b),remaining_work(w){}
    void finish(){remaining_bytes-=storage-initial_storage;remaining_work-=work-initial_work;}
};
void admission(const Layer& l,double dpi,Ledger& b){
    if(!vector_move_owner(l)||l.opaque||!std::isfinite(dpi)||dpi<=0||!std::isfinite(l.common.resolution)||l.common.resolution<=0||(l.planes.empty()&&l.family!=0x21200000)
        ||l.common.width<=0||l.common.height<=0||l.common.width>100000||l.common.height>100000||(l.common.depth!=32&&l.common.depth!=64))
        throw std::invalid_argument("Vector MoveImage requires an ordinary native16 Vert owner");
    vector_tool_output_detail::admit_metadata(l);
    std::uint64_t wire=l.specific.size();if(l.planes.size()>1024)throw std::length_error("Vector floating child limit");
    for(const auto& p:l.planes){if(!(paint(l)?(paint_plane(p)||pure(p)):(vertex(p)||pure(p)))||(!std::isfinite(p.common.resolution)||p.common.resolution<=0)||p.common.opacity_mode==4)throw std::invalid_argument("Vector floating requires supported finite-DPI children");wire+=p.specific.size();}
    if(wire>8ull*1024*1024||wire*16>b.remaining_bytes||wire*12>b.remaining_work)throw std::length_error("Vector floating source admission exceeds resources");
    charge(b.storage,wire*16,512ull*1024*1024,"Vector floating source storage limit");b.work+=wire*12;b.wire+=wire;
    charge(b.refs,references(l),8192,"Vector floating input reference limit");
    vector_geometry_detail::validate(l,b.storage,b.work,{});
}
void copy_line(Editor& out,const VectorLineArchive& input,int source,std::optional<vector_copy_detail::Matrix> matrix={}){
    const auto found=std::ranges::find(input.lines,source,&VectorPoolRecord<VectorLine>::index);if(found==input.lines.end())throw std::invalid_argument("Vector floating has a dead source LC");const auto old=found->value;
    auto style=old;style.half_width_bits=vector_copy_detail::width(old.half_width_bits,matrix);const int id=out.new_line(style);int carry=-1;
    for(const auto member:old.members){const auto model=vector_boundary_detail::copy_directed_model(out,input,member.model,member.selector!=0,carry,{},matrix);
        out.line(id).members.push_back({model,1});out.model(model).line=id;out.reference();}
    out.cache(id,true);
}
void save(Layer& layer,const VectorLineArchive& a){layer.specific=encode_vector_line_archive(a);}
std::uint64_t paint_count(const VectorPaintArchive& a){return count(a.base)+a.loops.size()+a.regions.size()+a.faces.size()+a.gradnodes.size()+a.gradations.size()+a.textures.size();}
VectorPaintArchive empty_paint(const VectorPaintArchive& source){
    VectorPaintArchive out;out.base.high_water.fill(-1);out.face_high_water.fill(-1);out.base.header_suffix=source.base.header_suffix;out.base.body_suffix=source.base.body_suffix;
    out.face_header_suffix=source.face_header_suffix;out.face_body_suffix=source.face_body_suffix;out.core_suffix=source.core_suffix;out.outer_suffix=source.outer_suffix;
    out.border=source.border;out.border.vertices.clear();out.border.models.clear();out.border.regions.clear();return out;
}
Rect payload_frame(const Layer& layer,VectorLineArchive& base,VectorPaintArchive* paint,Ledger& b){
    VectorBounds raw;
    for(const auto& plane:layer.planes){if(!plane.common.flags[1])continue;
        if(vertex(plane)||pure(plane)){for(int id:vector_line_plane_references(plane)){vector_material_detail::charge(b.work,1,128ull*1024*1024,"Floating frame work limit");unite(raw,record(base.lines,id).value.archived_bounds);}}
        else if(paint&&paint_plane(plane)){const vector_material_detail::PaintLists links(plane);for(int id:links.ids[0]){vector_material_detail::charge(b.work,1,128ull*1024*1024,"Floating frame work limit");const auto& face=record(paint->faces,id).value;unite(raw,record(paint->regions,face.region).value.outer.archived_bounds);}}
    }
    if(raw.left>=raw.right||raw.top>=raw.bottom)return {};
    //61B790 unions one owner's cached raw rectangles before signed idiv16.
    const int left=integer(double(raw.left/16)+layer.common.x),top=integer(double(raw.top/16)+layer.common.y);
    const int right=integer(double(raw.right/16)+layer.common.x),bottom=integer(double(raw.bottom/16)+layer.common.y);
    return {left,top,right-left,bottom-top};
}
void capture_pure(vector_boundary_detail::Integrator& cut,vector_boundary_detail::Integrator& copier,
    const std::vector<std::size_t>& planes,const std::map<std::size_t,std::size_t>& mappings,
    const RegionSelection& selection,double dpi,bool& captured,VectorBounds& history_damage){
    auto& from=cut.geometry();const bool spatial=selection.vector_flags&1u;
    std::optional<FrozenVectorLineSelection> packet;
    std::optional<vector_probe_detail::Scope> scope;
    if(spatial){
        packet=prepare_vector_line_selection(selection,dpi,selection_carrier_dpi(selection,dpi));scope=vector_line_selection_detail::reserve_line_selection_scope(*packet,{from.layer.common.x,from.layer.common.y},from.storage,from.budget.work);}
    std::vector<int> lines,pending;
    for(auto plane:planes){if(!pure(from.layer.planes.at(plane)))continue;const auto original=vector_line_plane_references(from.layer.planes[plane]);
        for(int id:original){const auto& line=from.line(id);if(line.members.empty())continue;Graph graph(from.a,from.budget);
            const bool admitted=spatial?graph.line_matches(line,*scope,bool(selection.vector_flags&4u),from.budget):std::ranges::any_of(line.members,[&](auto m){return bool(from.model(m.model).flags&0x10000u);});
            if(!admitted)continue;captured=true;vector_eraser_detail::PointSelectionResult extracted;
            if(spatial)extracted=vector_eraser_detail::extract_border_point_selection(cut,plane,id,*scope);
            else if(selection.vector_flags&0x10u)extracted=vector_eraser_detail::extract_border_marked_trim(cut,plane,id);
            else{extracted.context_dirty=line.archived_bounds;extracted.selected_lines.push_back(id);
                for(auto member:line.members)for(unsigned side=0;side<2;++side){from.budget.charge();const auto& model=from.model(member.model);
                    if(model.targets[side]<0)throw std::invalid_argument("Pure capture requires live MC side ownership");
                    extracted.pending_regions.push_back(model.target_kinds[side]?model.targets[side]:record(from.paint->loops,model.targets[side]).value.region);}}
            unite(history_damage,extracted.context_dirty);
            charge(from.storage,4*(extracted.selected_lines.size()+extracted.pending_regions.size()),512ull*1024*1024,"Pure capture occurrence storage limit");
            if(lines.size()+extracted.selected_lines.size()>8192||pending.size()+extracted.pending_regions.size()>8192)throw std::length_error("Pure capture occurrence limit");
            lines.insert(lines.end(),extracted.selected_lines.begin(),extracted.selected_lines.end());pending.insert(pending.end(),extracted.pending_regions.begin(),extracted.pending_regions.end());
        }
    }
    if(!lines.empty()){
        //5D4F10 owns payload conversion. Its output damage is independent of
        //source context+78. Keep live MC backlinks until its source admission.
        vector_line_paint_detail::convert_lines(copier,from.a,from.layer,lines,{mappings,{},{}});
        const auto owners=cut.line_planes();
        for(int id:lines){from.budget.charge();const auto plane=owners.at(id);OwnerLists links(from.layer.planes[plane],from.paint);from.owner.lines=links.lines;
            auto& line=from.line(id);if(from.refs<line.members.size()+1)throw std::invalid_argument("Pure capture LC reference ledger underflow");
            for(auto member:line.members){from.budget.charge();auto& model=from.model(member.model);if(model.line!=id)throw std::invalid_argument("Pure capture lost MC parent");model.line=-1;}
            from.refs-=line.members.size()+1;line.members.clear();Editor::remove(from.owner.lines,id);from.erase(from.a.lines,5,id);
            links.lines=from.owner.lines;links.vertices=from.owner.vertices;links.models=from.owner.models;links.publish(from.layer.planes[plane],from.paint);
        }
    }
    unite(history_damage,cut.materialize_regions(pending));cut.publish();
}
Layer capture_paint(Layer& remainder,const Layer& source,const std::vector<std::size_t>& planes,const RegionSelection& selection,double dpi,Ledger& b,bool& captured,Rect& bounds,VectorBounds& history_damage,bool copy_only){
    auto input=decode_vector_paint_archive(source);b.nodes=paint_count(input);if(b.nodes>4096)throw std::length_error("Paint floating node limit");
    auto output=empty_paint(input);Layer payload=source;payload.planes.clear();payload.runtime_vector_high_water={};payload.current_plane=0;
    std::map<std::size_t,std::size_t> mappings;for(auto index:planes){auto p=source.planes.at(index);if(pure(p)){OwnerLists list(p,&output);list.lines.clear();list.publish(p,&output);}else{vector_material_detail::PaintLists list(p);for(auto& ids:list.ids)ids.clear();list.publish(p);}
        mappings.emplace(index,payload.planes.size());if(int(index)==source.current_plane)payload.current_plane=int(payload.planes.size());payload.planes.push_back(std::move(p));}
    Editor from(input.base,&input,remainder,BorderOwnerTag{},b.wire,b.nodes,b.refs,b.storage,b.work,{}),to(output.base,&output,payload,BorderOwnerTag{},b.wire,b.nodes,b.refs,b.storage,b.work,{});
    vector_boundary_detail::Integrator cut(from),copier(to);std::vector<int> selected;VectorBounds converter_dirty;
    const bool full=selection.state==SelectionLayerState::FullPlane,marked=!full&&!(selection.vector_flags&1u),line_only=!full&&!(selection.vector_flags&0x200u);
    if(line_only)capture_pure(cut,copier,planes,mappings,selection,dpi,captured,history_damage);
    else{
    if(full||marked){
        std::set<int> faces;for(auto index:planes){if(full)remainder.planes[index].common.flags[3]=1;if(pure(remainder.planes[index])){if(full)for(int line:vector_line_plane_references(remainder.planes[index]))for(auto member:record(input.base.lines,line).value.members)record(input.base.models,member.model).value.flags|=0x10000u;}else{const vector_material_detail::PaintLists list(remainder.planes[index]);for(int face:list.ids[0]){if(full)record(input.faces,face).value.flags|=0x10000u;if(record(input.faces,face).value.flags&0x10000u)faces.insert(face);}}}
        for(int id:input.border.regions){const auto& region=record(input.regions,id).value;if(std::ranges::any_of(region.faces,[&](int face){return faces.contains(face);}))selected.push_back(id);}
    }
    else{auto document=source.common;document.resolution=dpi;const auto frozen=vector_selection_detail::prepare(selection,document,selection_carrier_dpi(selection,dpi),b.storage,from.budget);
        const auto converted=vector_selection_detail::gradation_transfer(cut,frozen,{source.common.x,source.common.y});selected=converted.selected_regions;unite(history_damage,converted.converter_dirty);const auto line_packet=prepare_vector_line_selection(selection,dpi,selection_carrier_dpi(selection,dpi));converter_dirty=vector_line_selection_detail::reserve_line_selection_scope(line_packet,{source.common.x,source.common.y},b.storage,b.work).bounds;}
    unite(history_damage,vector_clear_detail::partition_capture_lines(cut,planes,selected,full||marked||line_only,converter_dirty));
    vector_boundary_detail::SourceCopySettings settings;settings.planes=mappings;settings.regions=selected;settings.filtered=full||marked||line_only;
    auto copied=vector_boundary_detail::copy_source(copier,input,remainder,settings);std::vector<int> missing;
    for(int id:vector_boundary_detail::source_pointer_ordered_models(copied)){const auto& model=to.model(id);if(model.targets[0]<0||model.targets[1]<0)missing.push_back(id);}
    copier.complete_missing_sides(missing,copied.new_regions);std::vector<int> repaired;copier.materialize_regions(copied.new_regions,&repaired);
    VectorBounds dirty;copier.merge_exteriors_once(dirty);copier.publish();
    if(copy_only)captured=true;else{const auto removed=vector_clear_detail::delete_captured_pure_faces(cut,planes,selected,full,!full&&!marked&&!line_only);captured=captured||removed.performed;unite(history_damage,removed.context);}
    }
    store_vector_paint_archive(remainder,input);store_vector_paint_archive(payload,output);
    vector_tool_output_detail::admit_output(remainder,&input,b.storage,b.work);vector_tool_output_detail::admit_output(payload,&output,b.storage,b.work);bounds=payload_frame(payload,output.base,&output,b);return payload;
}
void materialize_paint(Layer& destination,const Layer& payload,const std::vector<std::size_t>& planes,std::array<int,2> delta,double dpi,Ledger& b,bool targeted=false){
    Layer shifted=payload;shifted.common.x=integer(double(payload.common.x)+delta[0]);shifted.common.y=integer(double(payload.common.y)+delta[1]);
    auto input=decode_vector_paint_archive(payload);auto output=decode_vector_paint_archive(destination);b.nodes=paint_count(output)+paint_count(input);if(b.nodes>4096)throw std::length_error("Paint floating node limit");
    for(auto& p:shifted.planes){if(pure(p)){for(int line:vector_line_plane_references(p))for(auto member:record(input.base.lines,line).value.members)record(input.base.models,member.model).value.flags&=~0x10000u;}else{const vector_material_detail::PaintLists list(p);for(int face:list.ids[0])record(input.faces,face).value.flags&=~0x10000u;}p.common.flags[3]=0;}
    Editor to(output.base,&output,destination,BorderOwnerTag{},b.wire,b.nodes,b.refs,b.storage,b.work,{});vector_boundary_detail::Integrator context(to);
    (void)dpi;std::map<std::size_t,std::size_t> mappings;
    for(std::size_t i=0;i<planes.size();++i){const auto sf=shifted.planes[i].family,tf=destination.planes[planes[i]].family;
        if(!targeted||(pure(shifted.planes[i])&&(tf==0x21110001||tf==0x21120001))||(sf==0x22100001&&(tf==0x22100001||tf==0x22100002)))mappings.emplace(i,planes[i]);}vector_selection_detail::transfer_all(context,input,shifted,mappings);
    context.publish();store_vector_paint_archive(destination,output);vector_tool_output_detail::admit_output(destination,&output,b.storage,b.work);
}
}
bool vector_move_owner(const Layer& l) noexcept{return l.family==0x22100000||l.family==0x24400000||paint(l);}
void validate_vector_move(const Layer& l,double dpi,std::uint64_t& bytes,std::uint64_t& work){Ledger b(bytes,work);admission(l,dpi,b);
    if(paint(l)){auto a=decode_vector_paint_archive(l);vector_tool_output_detail::admit_output(l,&a,b.storage,b.work);}
    else vector_tool_output_detail::admit_output(l,nullptr,b.storage,b.work);b.finish();}
Layer capture_vector_move(Layer& remainder,const Layer& source,const std::vector<std::size_t>& planes,
    const RegionSelection& selection,double dpi,std::uint64_t& bytes,std::uint64_t& work,bool& captured,Rect& bounds,VectorBounds& history_damage,bool copy_only){
    history_damage={};Ledger b(bytes,work);admission(source,dpi,b);if(paint(source)){auto payload=capture_paint(remainder,source,planes,selection,dpi,b,captured,bounds,history_damage,copy_only);b.finish();return payload;}auto input=decode_vector_line_archive(source);b.nodes=count(input);if(b.nodes>4096)throw std::length_error("Vector floating node limit");
    Layer payload=source;payload.planes.clear();payload.current_plane=0;payload.runtime_vector_high_water={};VectorLineArchive output;output.header_suffix=input.header_suffix;output.body_suffix=input.body_suffix;output.layer_suffix=input.layer_suffix;
    for(auto index:planes){auto p=source.planes.at(index);OwnerLists empty(p,nullptr);empty.vertices.clear();empty.models.clear();empty.lines.clear();empty.publish(p,nullptr);if(int(index)==source.current_plane)payload.current_plane=int(payload.planes.size());payload.planes.push_back(std::move(p));}
    if(selection.mask){const auto width=std::uint64_t(selection.mask->width()),height=std::uint64_t(selection.mask->height());
        const auto retained=2*(sizeof(BinaryMask)+((width+31)/32)*height*4+((width+255)/256)*((height+255)/256));
        if(retained+2048>512ull*1024*1024-b.storage||retained+((width+31)/32)*height+2048>128ull*1024*1024-b.work)throw std::length_error("Vector floating mask snapshot exceeds aggregate resources");}
    const auto packet=prepare_vector_line_selection(selection,dpi,selection_carrier_dpi(selection,dpi));const auto scope=vector_line_selection_detail::reserve_line_selection_scope(packet,{source.common.x,source.common.y},b.storage,b.work);
    for(std::size_t i=0;i<planes.size();++i){const auto index=planes[i];Editor from(input,nullptr,remainder,index,b.wire,b.nodes,b.refs,b.storage,b.work,{}),to(output,nullptr,payload,i,b.wire,b.nodes,b.refs,b.storage,b.work,{});
        const auto lines=from.owner.lines;if(selection.state==SelectionLayerState::FullPlane)captured=true;
        if(copy_only){
            for(int id:lines){from.budget.charge();const auto& line=from.line(id);const auto r=line.archived_bounds;
                if(selection.state!=SelectionLayerState::FullPlane&&selection.active()&&(r.left>=scope.bounds.right||r.top>=scope.bounds.bottom||r.right<=scope.bounds.left||r.bottom<=scope.bounds.top))continue;
                copy_line(to,input,id);captured=true;const auto clone=to.owner.lines.back();
                if(selection.state!=SelectionLayerState::FullPlane){Graph graph(output,to.budget);const bool marked=!(selection.vector_flags&1u);const bool keep=marked?std::ranges::any_of(to.line(clone).members,[&](auto m){return bool(to.model(m.model).flags&0x10000u);}):graph.line_matches(to.line(clone),scope,true,to.budget);
                    if(!keep){if(selection.vector_flags&1u)vector_eraser_detail::retain_point_selection(to,i,clone,scope);else erase_line(to,clone);}
                }
            }to.owner.publish(payload.planes[i],nullptr);continue;
        }
        for(int id:lines){from.budget.charge();if(selection.state==SelectionLayerState::FullPlane){copy_line(to,input,id);erase_line(from,id);}
            else{Graph graph(input,from.budget);const bool marked=!(selection.vector_flags&1u);const bool admitted=marked?std::ranges::any_of(from.line(id).members,[&](auto member){return bool(from.model(member.model).flags&0x10000u);}):graph.line_matches(from.line(id),scope,bool(selection.vector_flags&4u),from.budget);
                if(admitted){captured=true;if(selection.vector_flags&1u)vector_eraser_detail::replace_point_pair(from,index,id,to,i,scope);
                    else if(selection.vector_flags&0x10u)vector_eraser_detail::replace_point_pair(from,index,id,to,i,scope,false,true);
                    else{copy_line(to,input,id);erase_line(from,id);}}
            }}
        from.owner.publish(remainder.planes[index],nullptr);to.owner.publish(payload.planes[i],nullptr);
    }
    save(remainder,input);save(payload,output);vector_tool_output_detail::admit_output(remainder,nullptr,b.storage,b.work);vector_tool_output_detail::admit_output(payload,nullptr,b.storage,b.work);bounds=payload_frame(payload,output,nullptr,b);b.finish();return payload;
}
bool materialize_vector_target(Layer& destination,std::size_t target,const Layer& payload,std::array<int,2> delta,double dpi,std::uint64_t& bytes,std::uint64_t& work){
    const auto family=destination.planes.at(target).family;const bool vert=family==0x21210001||family==0x21220001,pure_target=family==0x21110001||family==0x21120001,face=family==0x22100001||family==0x22100002;
    if(!vert&&!pure_target&&!face)return false;std::vector<std::size_t> eligible;
    for(std::size_t i=0;i<payload.planes.size();++i){const auto f=payload.planes[i].family;if((vert&&(f==0x21210001||f==0x21220001))||(pure_target&&(f==0x21110001||f==0x21120001))||(face&&f==0x22100001))eligible.push_back(i);}
    if(eligible.empty())return false;
    Ledger b(bytes,work);admission(destination,dpi,b);admission(payload,dpi,b);
    if(paint(destination)&&paint(payload)){materialize_paint(destination,payload,std::vector<std::size_t>(payload.planes.size(),target),delta,dpi,b,true);b.finish();return true;}
    if(!paint(destination)){
        auto output=decode_vector_line_archive(destination);auto input=paint(payload)?decode_vector_paint_archive(payload).base:decode_vector_line_archive(payload);b.nodes=count(input)+count(output);
        std::vector<int> vertices,models;for(const auto& v:output.vertices)vertices.push_back(v.index);for(const auto& m:output.models)models.push_back(m.index);
        const auto original=destination.planes[target];Reader reader(original.specific);std::vector<int> lines;if(pure_target)lines=OwnerLists::read(reader);else lines=vector_line_plane_references(original);const auto tail=pure_target?reader.rest():Bytes{};
        Editor to(output,destination,OwnerLists(vertices,models,lines),b.wire,b.nodes,b.refs,b.storage,b.work);
        auto from=payload.common;from.x=integer(double(from.x)+delta[0]);from.y=integer(double(from.y)+delta[1]);
        for(auto i:eligible){const auto matrix=vector_copy_detail::between(from,payload.planes[i].common.resolution,destination.common,destination.planes[target].common.resolution);
            for(int id:vector_line_plane_references(payload.planes[i])){auto& line=record(input.lines,id).value;if(vert)line.color=destination.planes[target].common.color;for(auto m:line.members)record(input.models,m.model).value.flags&=~0x10000u;copy_line(to,input,id,matrix);}}
        if(pure_target){Writer w;OwnerLists::write(w,to.owner.lines);w.raw(tail);destination.planes[target].specific=w.data;}
        else{OwnerLists actual(destination.planes[target],nullptr);const auto old_vertices=actual.vertices,old_models=actual.models;actual.lines=to.owner.lines;for(int id:to.owner.vertices)if(std::ranges::find(old_vertices,id)==old_vertices.end()&&std::ranges::find(vertices,id)==vertices.end())actual.vertices.push_back(id);for(int id:to.owner.models)if(std::ranges::find(old_models,id)==old_models.end()&&std::ranges::find(models,id)==models.end())actual.models.push_back(id);actual.publish(destination.planes[target],nullptr);}
        destination.specific=encode_vector_line_archive(output);vector_tool_output_detail::admit_output(destination,nullptr,b.storage,b.work);b.finish();return true;
    }
    //An ordinary Pure source has implicit topology. Clone it into a private
    //Border carrier, complete sides there, then use the same mapped all-copy.
    auto input=decode_vector_line_archive(payload);VectorPaintArchive carrier_archive;carrier_archive.base.high_water.fill(-1);carrier_archive.face_high_water.fill(-1);carrier_archive.border.common=payload.planes[eligible.front()].common;
    Layer carrier=payload;carrier.family=0x21200000;carrier.planes.clear();carrier.runtime_vector_high_water={};std::map<std::size_t,std::size_t> mappings;std::vector<int> lines;
    for(auto i:eligible){auto p=payload.planes[i];Writer w;w.i32(0);p.specific=w.data;mappings.emplace(i,carrier.planes.size());carrier.planes.push_back(p);const auto ids=vector_line_plane_references(payload.planes[i]);lines.insert(lines.end(),ids.begin(),ids.end());}
    b.nodes=count(input);Editor to(carrier_archive.base,&carrier_archive,carrier,BorderOwnerTag{},b.wire,b.nodes,b.refs,b.storage,b.work,{});vector_boundary_detail::Integrator context(to);
    const auto copied=vector_boundary_detail::copy_line_occurrences(to,input,payload,lines,mappings);std::vector<int> regions;context.complete_missing_sides(copied.copied_models,regions);context.materialize_regions(regions);context.publish();store_vector_paint_archive(carrier,carrier_archive);
    materialize_paint(destination,carrier,std::vector<std::size_t>(carrier.planes.size(),target),delta,dpi,b,true);b.finish();return true;
}
void materialize_vector_move(Layer& destination,const Layer& payload,const std::vector<std::size_t>& planes,
    std::array<int,2> delta,double dpi,std::uint64_t& bytes,std::uint64_t& work){
    Ledger b(bytes,work);admission(destination,dpi,b);admission(payload,dpi,b);if(planes.size()!=payload.planes.size())throw std::invalid_argument("Vector floating plane mapping changed");
    if(paint(payload)){materialize_paint(destination,payload,planes,delta,dpi,b);b.finish();return;}
    auto output=decode_vector_line_archive(destination);auto input=decode_vector_line_archive(payload);b.nodes=count(output)+count(input);if(b.nodes>4096)throw std::length_error("Vector floating node limit");
    auto from=payload.common;from.x=integer(double(from.x)+delta[0]);from.y=integer(double(from.y)+delta[1]);
    for(std::size_t i=0;i<planes.size();++i){const auto index=planes[i];if(index>=destination.planes.size()||destination.planes[index].family!=payload.planes[i].family)throw std::invalid_argument("Vector floating destination changed");
        Editor to(output,nullptr,destination,index,b.wire,b.nodes,b.refs,b.storage,b.work,{});const auto lines=vector_line_plane_references(payload.planes[i]);
        //645CA0 clears source MC selection bits and recolors every copied LC
        //to the actual destination plane before the directed geometry copy.
        for(int id:lines){auto& line=record(input.lines,id).value;line.color=destination.planes[index].common.color;
            for(const auto member:line.members)record(input.models,member.model).value.flags&=~0x10000u;}
        const auto matrix=vector_copy_detail::between(from,payload.planes[i].common.resolution,destination.common,destination.planes[index].common.resolution);
        for(int id:lines)copy_line(to,input,id,matrix);to.owner.publish(destination.planes[index],nullptr);
    }
    save(destination,output);vector_tool_output_detail::admit_output(destination,nullptr,b.storage,b.work);b.finish();
}
}
