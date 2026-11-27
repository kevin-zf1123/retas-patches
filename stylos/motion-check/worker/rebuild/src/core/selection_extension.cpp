#include "selection_extension.hpp"
#include "vector_graph_edit.hpp"
#include "selection_vector_render.hpp"
#include "vector_boundary_source_copy.hpp"
#include "vector_line_selection.hpp"
#include "vector_region_probe.hpp"
#include "vector_material_geometry.hpp"
#include "retas/vector_extension_archive.hpp"
#include "retas/vector_geometry.hpp"
#include "retas/vector_color_metadata.hpp"
#include <set>
namespace retas::selection_detail {
namespace {
using namespace vector_graph_detail;
struct Lists {
    VectorExtensionPlaneReferences ids;Bytes topology_tail,bundle_tail,line_tail,paint_tail,tail;
    explicit Lists(const Plane& p):ids(vector_extension_plane_references(p)){
        Reader r(p.specific);auto b=r.section(),t=b.section();OwnerLists::read(t);OwnerLists::read(t);topology_tail=t.rest();
        if(p.family==0x40000001){auto l=b.section();OwnerLists::read(l);line_tail=l.rest();bundle_tail=b.rest();}
        else{OwnerLists::read(b);bundle_tail=b.rest();auto l=r.section();OwnerLists::read(l);line_tail=l.rest();auto f=r.section();for(int i=0;i<3;++i)OwnerLists::read(f);paint_tail=f.rest();}
        OwnerLists::read(r);tail=r.rest();
    }
    void publish(Plane& p)const{Writer w,b,t,l;OwnerLists::write(t,ids.vertices);OwnerLists::write(t,ids.models);t.raw(topology_tail);b.section(t.data);
        OwnerLists::write(l,ids.lines);l.raw(line_tail);
        if(p.family==0x40000001){b.section(l.data);b.raw(bundle_tail);w.section(b.data);}
        else{OwnerLists::write(b,ids.regions);b.raw(bundle_tail);w.section(b.data);w.section(l.data);Writer f;for(const auto* list:{&ids.faces,&ids.gradations,&ids.textures})OwnerLists::write(f,*list);f.raw(paint_tail);w.section(f.data);}
        OwnerLists::write(w,ids.extensions);w.raw(tail);p.specific=std::move(w.data);
    }
    void clear(){ids={};}
};
std::uint64_t count(const VectorLineArchive& a){return a.vertices.size()+a.scales.size()+a.subcurves.size()+a.models.size()+a.crosspoints.size()+a.lines.size();}
struct Ledger {
    std::uint64_t wire{},nodes{},refs{},storage,work,initial_storage,initial_work;std::uint64_t& bytes;std::uint64_t& operations;
    Ledger(std::uint64_t& b,std::uint64_t& w):storage(512ull*1024*1024-b),work(128ull*1024*1024-w),initial_storage(storage),initial_work(work),bytes(b),operations(w){}
    void finish(){bytes-=storage-initial_storage;operations-=work-initial_work;}
};
void output(const Layer& l,Ledger& b){
    const auto& c=l.common;const VectorRenderContext context{c.width,c.height,c.depth,c.resolution,16,{},std::nullopt,2};
    const auto costs=l.family==0x40004000?vector_render_detail::ruler_output_costs(decode_vector_ruler_geometries(l),context,{b.storage,b.work}):vector_render_detail::shape_output_costs(decode_vector_shape_geometries(l),context,{b.storage,b.work});b.storage=costs.storage;b.work=costs.work;
}
void admit(const Layer& l,double dpi,Ledger& b){
    if(!extension_move_owner(l)||l.opaque||!std::isfinite(l.common.resolution)||l.common.resolution<=0||!std::isfinite(dpi)||dpi<=0||(l.common.depth!=32&&l.common.depth!=64)||l.planes.empty()||l.planes.size()>1024)throw std::invalid_argument("Extension floating requires a typed native16 owner");
    if(l.common.width<=0||l.common.height<=0||l.common.width>100000||l.common.height>100000||l.common.opacity!=100||l.common.opacity_mode!=1)throw std::invalid_argument("Extension consumer metadata unsupported");
    for(const auto& pos:l.common.position)if(pos.value!=0)throw std::invalid_argument("Extension consumer position unsupported");
    std::uint64_t wire=l.specific.size();for(const auto& p:l.planes){if(p.opaque||p.raster||p.family!=(l.family==0x40004000?0x40000001u:0x40000002u)||(!std::isfinite(p.common.resolution)||p.common.resolution<=0)||p.common.opacity_mode!=1)throw std::invalid_argument("Extension floating child profile unsupported");wire+=p.specific.size();}
    if(wire>8ull*1024*1024)throw std::length_error("Extension floating wire limit");charge(b.storage,wire*16,512ull*1024*1024,"Extension floating storage limit");b.work+=wire*8;b.wire+=wire;
    vector_geometry_detail::validate(l,b.storage,b.work,{});output(l,b);
}
template<class T> void erase_pool(std::vector<VectorPoolRecord<T>>& pool,int id,int& high,Layer& owner,unsigned kind,Ledger& b){
    auto it=std::ranges::find(pool,id,&VectorPoolRecord<T>::index);if(it==pool.end())throw std::invalid_argument("Dead extension identity");
    const auto previous=kind<12?owner.runtime_vector_high_water[kind].value_or(high):high;
    VectorSlots slots(pool,b.work,{},previous);slots.release(id);pool.erase(it);high=slots.high;if(kind<12)owner.runtime_vector_high_water[kind]=high;
}
template<class T> int append(std::vector<VectorPoolRecord<T>>& pool,int& high,Layer& owner,unsigned kind,T value,Ledger& b){
    VectorSlots slots(pool,b.work,{},kind<12?owner.runtime_vector_high_water[kind]:std::optional<int>{high});int id=slots.allocate(b.work,{});charge(b.nodes,1,4096,"Extension floating node limit");charge(b.refs,1,8192,"Extension floating reference limit");charge(b.wire,8,8ull*1024*1024,"Extension floating wire limit");charge(b.wire,128,8ull*1024*1024,"Extension floating wire limit");charge(b.storage,512,512ull*1024*1024,"Extension floating storage limit");pool.push_back({id,std::move(value),{}});std::ranges::sort(pool,{},&VectorPoolRecord<T>::index);high=slots.high;if(kind<12)owner.runtime_vector_high_water[kind]=high;return id;
}
int clone_line(Editor& e,const VectorLineArchive& source,int id,std::uint64_t color,std::optional<vector_copy_detail::Matrix> matrix={}){const auto old=record(const_cast<VectorLineArchive&>(source).lines,id).value;auto style=old;style.color=color;style.half_width_bits=vector_copy_detail::width(old.half_width_bits,matrix);const int fresh=e.new_line(style);int carry=-1;
    for(auto m:old.members){int copied=vector_boundary_detail::copy_directed_model(e,source,m.model,m.selector!=0,carry,{},matrix);e.line(fresh).members.push_back({copied,1});e.model(copied).line=fresh;e.reference();}e.cache(fresh,true);return fresh;
}
void close(Editor& e,const std::vector<VectorLineMember>& members){if(members.empty())return;const auto& first=e.model(members.front().model);const auto& last=e.model(members.back().model);int head=e.sub(first.subcurves.front()).vertex_a,tail=e.sub(last.subcurves.back()).vertex_b;if(head!=tail)weld(e,head,tail);}
//Only geometry created/released by this extension belongs to these lists.
void sync(Lists& lists,const Editor& e){lists.ids.vertices=e.owner.vertices;lists.ids.models=e.owner.models;lists.ids.lines=e.owner.lines;}
VectorLineArchive empty_line(const VectorLineArchive& in){VectorLineArchive a;a.header_suffix=in.header_suffix;a.body_suffix=in.body_suffix;a.layer_suffix=in.layer_suffix;return a;}
void charge_base(const VectorLineArchive& a,Ledger& b){b.nodes+=count(a);for(const auto& v:a.vertices)b.refs+=v.value.subcurves.size();for(const auto& s:a.subcurves)b.refs+=s.value.scales.size();for(const auto& m:a.models)b.refs+=m.value.subcurves.size()+std::ranges::count_if(m.value.crosspoints,[](int id){return id>=0;});for(const auto& l:a.lines)b.refs+=l.value.members.size();}
void charge_archive(const Layer& owner,Ledger& b){
    if(owner.family==0x40004000){const auto a=decode_vector_ruler_archive(owner);charge_base(a.base,b);b.nodes+=a.rulers.size();}
    else{const auto a=decode_vector_shape_archive(owner);const auto& p=a.paint;charge_base(p.base,b);
        b.nodes+=a.shapes.size()+p.loops.size()+p.regions.size()+p.faces.size()+p.gradnodes.size()+p.gradations.size()+p.textures.size();
        b.refs+=p.border.vertices.size()+p.border.models.size()+p.border.regions.size();
        for(const auto& loop:p.loops)b.refs+=loop.value.members.size();
        for(const auto& r:p.regions)b.refs+=r.value.outer.members.size()+r.value.holes.size()+r.value.faces.size();
        for(const auto& g:p.gradations){const auto leaf=decode_vector_gradation(g.value);b.refs+=leaf.faces.size()+leaf.stops.size();}
        for(const auto& t:p.textures)b.refs+=decode_vector_texture(t.value).faces.size();
    }
    for(const auto& plane:owner.planes){const auto lists=vector_extension_plane_references(plane);
        b.refs+=lists.vertices.size()+lists.models.size()+lists.regions.size()+lists.lines.size()+lists.faces.size()+lists.gradations.size()+lists.textures.size()+lists.extensions.size();}
    charge(b.nodes,0,4096,"Extension floating node limit");charge(b.refs,0,8192,"Extension floating reference limit");
}
Layer ruler_capture(Layer& remainder,const Layer& source,const std::vector<std::size_t>& planes,Ledger& b,bool& captured){
    auto from=decode_vector_ruler_archive(source);auto to=from;to.base=empty_line(from.base);to.rulers.clear();to.high_water=-1;charge_archive(source,b);
    Layer payload=source;payload.planes.clear();payload.runtime_vector_high_water={};payload.current_plane=0;
    for(auto index:planes){Lists src(remainder.planes[index]),dst(source.planes[index]);dst.clear();auto p=source.planes[index];payload.planes.push_back(p);
        Editor cut(from.base,remainder,OwnerLists(src.ids.vertices,src.ids.models,src.ids.lines),b.wire,b.nodes,b.refs,b.storage,b.work),copy(to.base,payload,OwnerLists({}, {}, {}),b.wire,b.nodes,b.refs,b.storage,b.work);
        for(int id:src.ids.extensions){const auto old=record(from.rulers,id).value;if(!(old.flags&0x10000u))continue;captured=true;VectorArchiveRuler fresh;
            if(old.flags&4){fresh.flags=old.flags;fresh.x=old.x;fresh.y=old.y;fresh.length_20=old.length_20;fresh.field_2c=4;}
            else{fresh=old;if(old.line>=0)fresh.line=clone_line(copy,from.base,old.line,p.common.color);}
            const int created=append(to.rulers,to.high_water,payload,6,fresh,b);dst.ids.extensions.push_back(created);
            if(old.line>=0)erase_line(cut,old.line);erase_pool(from.rulers,id,from.high_water,remainder,6,b);--b.nodes;
        }
        sync(src,cut);std::erase_if(src.ids.extensions,[&](int id){return std::ranges::find(from.rulers,id,&VectorPoolRecord<VectorArchiveRuler>::index)==from.rulers.end();});src.publish(remainder.planes[index]);sync(dst,copy);dst.publish(payload.planes.back());
    }
    remainder.specific=encode_vector_ruler_archive(from);payload.specific=encode_vector_ruler_archive(to);return payload;
}
struct ShapeAdapter {
    Layer layer;VectorPaintArchive& a;std::vector<Lists> lists;VectorArchiveBorder original_border;
    ShapeAdapter(const Layer& source,VectorPaintArchive& graph):layer(source),a(graph),original_border(graph.border){layer.family=0x21200000;layer.planes.clear();
        for(const auto& p:source.planes){lists.emplace_back(p);const auto& ids=lists.back().ids;Plane line=p,paint=p;line.family=0x21110001;paint.family=0x22100001;Writer w;OwnerLists::write(w,ids.lines);line.specific=w.data;
            layer.planes.push_back(std::move(line));Writer f;for(const auto* v:{&ids.faces,&ids.gradations,&ids.textures})OwnerLists::write(f,*v);paint.specific=f.data;layer.planes.push_back(std::move(paint));
            a.border.vertices.insert(a.border.vertices.end(),ids.vertices.begin(),ids.vertices.end());a.border.models.insert(a.border.models.end(),ids.models.begin(),ids.models.end());a.border.regions.insert(a.border.regions.end(),ids.regions.begin(),ids.regions.end());
        }
    }
    void publish(Layer& target){for(std::size_t i=0;i<lists.size();++i){auto& list=lists[i];list.ids.lines=vector_line_plane_references(layer.planes[2*i]);const vector_material_detail::PaintLists p(layer.planes[2*i+1]);list.ids.faces=p.ids[0];list.ids.gradations=p.ids[1];list.ids.textures=p.ids[2];list.publish(target.planes[i]);}a.border=original_border;target.runtime_vector_high_water=layer.runtime_vector_high_water;}
};
void bare_release(Editor& e,int id){auto m=e.model(id);m.targets={-1,-1};e.model(id).targets={-1,-1};const int line=e.new_line(VectorLine{});e.line(line).members={{id,1}};e.model(id).line=line;erase_line(e,line);}
void shape_delete(vector_boundary_detail::Integrator& c,Lists& links,const VectorArchiveShape& old){auto& e=c.geometry();std::set<int> geometry;
    if(old.line>=0)for(auto m:e.line(old.line).members)geometry.insert(m.model);else if(old.region>=0)for(auto m:e.value(e.paint->regions,old.region).outer.members)geometry.insert(m.model);
    std::set<int> loops;if(old.region>=0){const auto r=e.value(e.paint->regions,old.region);loops.insert(r.holes.begin(),r.holes.end());}
    for(int id:geometry)for(unsigned side=0;side<2;++side){const auto& m=e.model(id);if(m.targets[side]<0)continue;if(m.target_kinds[side]?m.targets[side]!=old.region:!loops.contains(m.targets[side]))throw std::invalid_argument("Shape capture cannot delete geometry shared with another live Region");}
    if(old.region>=0){c.erase_extension_region(old.region);Editor::remove(links.ids.regions,old.region);}
    for(int id:geometry)e.model(id).targets={-1,-1};
    if(old.line>=0){e.owner.lines=links.ids.lines;erase_line(e,old.line);links.ids.lines=e.owner.lines;}
    else for(int id:geometry)bare_release(e,id);
    std::erase_if(links.ids.vertices,[&](int id){return std::ranges::find(e.a.vertices,id,&VectorPoolRecord<VectorVertex>::index)==e.a.vertices.end();});std::erase_if(links.ids.models,[&](int id){return std::ranges::find(e.a.models,id,&VectorPoolRecord<VectorModelCurve>::index)==e.a.models.end();});
}
int shape_clone(vector_boundary_detail::Integrator& c,const VectorPaintArchive& source,const VectorArchiveShape& old,std::size_t plane,Lists& links,std::optional<vector_copy_detail::Matrix> matrix={}){
    auto& e=c.geometry();const auto vertices=e.owner.vertices.size(),models=e.owner.models.size();e.owner.lines=links.ids.lines;int line=-1,region=-1;std::vector<VectorLineMember> members;
    if(old.line>=0){line=clone_line(e,source.base,old.line,record(const_cast<VectorLineArchive&>(source.base).lines,old.line).value.color,matrix);members=e.line(line).members;}
    else if(old.region>=0){int carry=-1;for(auto m:record(const_cast<VectorPaintArchive&>(source).regions,old.region).value.outer.members){const int id=vector_boundary_detail::copy_directed_model(e,source.base,m.model,true,carry,{},matrix);members.push_back({id,1});}}
    if(old.region>=0){close(e,members);vector_geometry_detail::rebuild_models(e.a,e.owner.models,e.budget.work,{});if(line>=0)e.cache(line,true);region=c.create_region();c.populate_loop(true,region,members);links.ids.regions.push_back(region);const auto& faces=record(const_cast<VectorPaintArchive&>(source).regions,old.region).value.faces;if(!faces.empty())vector_boundary_detail::copy_shape_face(c,source,faces.front(),2*plane+1,region,matrix);}
    links.ids.vertices.insert(links.ids.vertices.end(),e.owner.vertices.begin()+std::ptrdiff_t(vertices),e.owner.vertices.end());links.ids.models.insert(links.ids.models.end(),e.owner.models.begin()+std::ptrdiff_t(models),e.owner.models.end());links.ids.lines=e.owner.lines;
    Writer w;OwnerLists::write(w,links.ids.lines);e.layer.planes[2*plane].specific=w.data;c.publish();return region;
}
Layer shape_capture(Layer& remainder,const Layer& source,const std::vector<std::size_t>& planes,Ledger& b,bool& captured){
    auto from=decode_vector_shape_archive(source),to=from;to.paint.base=empty_line(from.paint.base);to.paint.base.high_water.fill(-1);to.paint.face_high_water.fill(-1);to.paint.loops.clear();to.paint.regions.clear();to.paint.faces.clear();to.paint.gradnodes.clear();to.paint.gradations.clear();to.paint.textures.clear();to.paint.border.vertices.clear();to.paint.border.models.clear();to.paint.border.regions.clear();to.shapes.clear();to.high_water=-1;
    charge_archive(source,b);
    Layer payload=source;payload.planes.clear();payload.current_plane=0;payload.runtime_vector_high_water={};for(auto i:planes){auto p=source.planes[i];Lists empty(p);empty.clear();empty.publish(p);payload.planes.push_back(p);}
    ShapeAdapter src(remainder,from.paint),dst(payload,to.paint);Editor cut(from.paint.base,&from.paint,src.layer,BorderOwnerTag{},b.wire,b.nodes,b.refs,b.storage,b.work,{}),copy(to.paint.base,&to.paint,dst.layer,BorderOwnerTag{},b.wire,b.nodes,b.refs,b.storage,b.work,{});vector_boundary_detail::Integrator removal(cut),clone(copy);
    for(std::size_t i=0;i<planes.size();++i){auto index=planes[i];auto& s=src.lists[index];auto& d=dst.lists[i];const auto ids=s.ids.extensions;for(int id:ids){const auto old=record(from.shapes,id).value;if(!(old.flags&0x10000u))continue;captured=true;auto fresh=old;fresh.region=shape_clone(clone,from.paint,old,i,d);fresh.line=old.line>=0?d.ids.lines.back():-1;const int created=append(to.shapes,to.high_water,dst.layer,12,fresh,b);d.ids.extensions.push_back(created);shape_delete(removal,s,old);erase_pool(from.shapes,id,from.high_water,src.layer,12,b);Editor::remove(s.ids.extensions,id);--b.nodes;}
        Writer w;OwnerLists::write(w,s.ids.lines);src.layer.planes[2*index].specific=w.data;}
    removal.publish();clone.publish();src.publish(remainder);dst.publish(payload);store_vector_shape_archive(remainder,from);store_vector_shape_archive(payload,to);return payload;
}
}
bool extension_move_owner(const Layer& l)noexcept{return l.family==0x40004000||l.family==0x40002000;}
void validate_extension_move(const Layer& l,double dpi,std::uint64_t& bytes,std::uint64_t& work){Ledger b(bytes,work);admit(l,dpi,b);b.finish();}
Layer capture_extension_move(Layer& remainder,const Layer& source,const std::vector<std::size_t>& planes,const RegionSelection&,double dpi,std::uint64_t& bytes,std::uint64_t& work,bool& captured){Ledger b(bytes,work);admit(source,dpi,b);auto payload=source.family==0x40004000?ruler_capture(remainder,source,planes,b,captured):shape_capture(remainder,source,planes,b,captured);vector_geometry_detail::validate(remainder,b.storage,b.work,{});vector_geometry_detail::validate(payload,b.storage,b.work,{});output(remainder,b);output(payload,b);b.finish();return payload;}
void materialize_extension_move(Layer& destination,const Layer& payload,const std::vector<std::size_t>& planes,std::array<int,2> delta,double dpi,std::uint64_t& bytes,std::uint64_t& work){
    Ledger b(bytes,work);admit(destination,dpi,b);admit(payload,dpi,b);if(destination.family!=payload.family||planes.size()!=payload.planes.size())throw std::invalid_argument("Extension floating mapping changed");
    Layer shifted=payload;auto source_common=payload.common;source_common.x=integer(double(source_common.x)+delta[0]);source_common.y=integer(double(source_common.y)+delta[1]);
    const auto transform=[&](std::size_t i){return vector_copy_detail::between(source_common,payload.planes[i].common.resolution,destination.common,destination.planes.at(planes[i]).common.resolution);};
    if(payload.family==0x40004000){auto from=decode_vector_ruler_archive(shifted),to=decode_vector_ruler_archive(destination);charge_archive(destination,b);charge_archive(shifted,b);
        for(std::size_t i=0;i<planes.size();++i){auto index=planes[i];const auto matrix=transform(i);Lists links(destination.planes.at(index));Editor copy(to.base,destination,OwnerLists(links.ids.vertices,links.ids.models,links.ids.lines),b.wire,b.nodes,b.refs,b.storage,b.work);const auto source=vector_extension_plane_references(shifted.planes[i]);for(int id:source.extensions){auto old=record(from.rulers,id).value;old.flags&=~0x10000u;VectorArchiveRuler fresh;if(old.flags&4){fresh.flags=old.flags;fresh.x=old.x;fresh.y=old.y;fresh.length_20=old.length_20;fresh.field_2c=4;if(matrix){const auto p=vector_copy_detail::point(fresh.x,fresh.y,*matrix);fresh.x=vector_copy_detail::integer(p[0]);fresh.y=vector_copy_detail::integer(p[1]);}}else{fresh=old;if(matrix){vector_copy_detail::extension(fresh.x,fresh.y,fresh.length_20,fresh.length_24,fresh.angle_bits,*matrix);const double scale=numeric_detail::binary64(std::sqrt(numeric_detail::binary64(matrix->sx*matrix->sx+matrix->yx*matrix->yx)));fresh.length_30=vector_copy_detail::integer(numeric_detail::binary64(double(fresh.length_30)*scale));fresh.length_34=vector_copy_detail::integer(numeric_detail::binary64(double(fresh.length_34)*scale));}if(old.line>=0)fresh.line=clone_line(copy,from.base,old.line,destination.planes[index].common.color,matrix);}links.ids.extensions.push_back(append(to.rulers,to.high_water,destination,6,fresh,b));}sync(links,copy);links.publish(destination.planes[index]);}destination.specific=encode_vector_ruler_archive(to);
    }else{auto from=decode_vector_shape_archive(shifted),to=decode_vector_shape_archive(destination);charge_archive(destination,b);charge_archive(shifted,b);ShapeAdapter dst(destination,to.paint);Editor copy(to.paint.base,&to.paint,dst.layer,BorderOwnerTag{},b.wire,b.nodes,b.refs,b.storage,b.work,{});vector_boundary_detail::Integrator context(copy);for(std::size_t i=0;i<planes.size();++i){const auto index=planes[i];const auto matrix=transform(i);auto& links=dst.lists.at(index);for(int id:vector_extension_plane_references(shifted.planes[i]).extensions){auto old=record(from.shapes,id).value;old.flags&=~0x10000u;auto fresh=old;if(matrix)vector_copy_detail::extension(fresh.x,fresh.y,fresh.length_24,fresh.length_28,fresh.angle_bits,*matrix);fresh.region=shape_clone(context,from.paint,old,index,links,matrix);fresh.line=old.line>=0?links.ids.lines.back():-1;links.ids.extensions.push_back(append(to.shapes,to.high_water,dst.layer,12,fresh,b));}}context.publish();dst.publish(destination);store_vector_shape_archive(destination,to);}
    vector_geometry_detail::validate(destination,b.storage,b.work,{});output(destination,b);b.finish();
}
}
