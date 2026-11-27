#include "layer_plane_internal.hpp"
#include "retas/vector_color_metadata.hpp"
#include "retas/vector_face_archive.hpp"
#include "vector_document_geometry.hpp"
#include "retas/layer_move.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace retas::stack_detail {
namespace {
using Map=std::map<int,int>;
int scaled(double n){if(!std::isfinite(n)||n<INT32_MIN||n>INT32_MAX)throw std::length_error("Vector scaled geometry exceeds signed32");return int(std::trunc(n));}
void refs(Writer& w,const std::vector<int>& ids){w.i32(int(ids.size()));for(int id:ids)w.i32(id);}
std::vector<int> refs(Reader& r){const int n=r.i32();r.require_count(n,4,100000);std::vector<int> out;for(int i=0;i<n;++i)out.push_back(r.i32());return out;}
void append(std::vector<int>& a,const std::vector<int>& b){if(a.size()+b.size()>100000)throw std::length_error("Stack vector references exceed 100000");a.insert(a.end(),b.begin(),b.end());}
struct Lists {
    bool vertex{},paint{};std::vector<int> vertices,models,lines,faces,gradations,textures;Bytes topology_tail,line_tail,tail;
    explicit Lists(const Plane& p){Reader r(p.specific);paint=(p.family&0x01000000u)==0;vertex=(p.family&0x00200000u)!=0&&!paint;
        if(paint){faces=refs(r);gradations=refs(r);textures=refs(r);}
        else if(vertex){auto t=r.section();vertices=refs(t);models=refs(t);topology_tail=t.rest();auto l=r.section();lines=refs(l);line_tail=l.rest();}
        else lines=refs(r);tail=r.rest();}
    void publish(Plane& p)const{Writer w;if(paint){refs(w,faces);refs(w,gradations);refs(w,textures);}
        else if(vertex){Writer t,l;refs(t,vertices);refs(t,models);t.raw(topology_tail);refs(l,lines);l.raw(line_tail);w.section(t.data);w.section(l.data);}else refs(w,lines);w.raw(tail);p.specific=std::move(w.data);}
};
int remap(int id,const Map& m){if(id<0)return id;auto i=m.find(id);if(i==m.end())throw std::invalid_argument("Dangling vector merge reference");return i->second;}
void remap(std::vector<int>& ids,const Map& m){for(auto& id:ids)id=remap(id,m);}
template<class T> Map allocate(const std::vector<VectorPoolRecord<T>>& target,const std::vector<VectorPoolRecord<T>>& source){
    if(target.size()+source.size()>100000)throw std::length_error("Stack vector pool exceeds 100000");std::set<int> used;for(const auto& r:target)used.insert(r.index);
    Map out;int next{};for(const auto& r:source){while(used.contains(next)){if(next==INT32_MAX)throw std::length_error("Vector merge ID exhausted");++next;}out.emplace(r.index,next);used.insert(next);}return out;
}
template<class T> void append_pool(std::vector<VectorPoolRecord<T>>& target,std::vector<VectorPoolRecord<T>>& source,const Map& ids,int& high){
    for(auto& r:source){r.index=remap(r.index,ids);high=std::max(high,r.index);target.push_back(std::move(r));}}
using Maps=std::array<Map,12>;
void prepare_base(Maps& m,const VectorLineArchive& a,const VectorLineArchive& b){
    m[0]=allocate(a.vertices,b.vertices);m[1]=allocate(a.scales,b.scales);m[2]=allocate(a.subcurves,b.subcurves);m[3]=allocate(a.models,b.models);m[4]=allocate(a.crosspoints,b.crosspoints);m[5]=allocate(a.lines,b.lines);
}
void append_base(VectorLineArchive& a,VectorLineArchive& b,const Maps& m){
    for(auto& r:b.vertices)remap(r.value.subcurves,m[2]);
    for(auto& r:b.subcurves){auto& v=r.value;v.vertex_a=remap(v.vertex_a,m[0]);v.vertex_b=remap(v.vertex_b,m[0]);v.model=remap(v.model,m[3]);remap(v.scales,m[1]);}
    for(auto& r:b.models){auto& v=r.value;remap(v.subcurves,m[2]);v.line=remap(v.line,m[5]);for(int i=0;i<2;++i){v.crosspoints[i]=remap(v.crosspoints[i],m[4]);v.targets[i]=remap(v.targets[i],m[v.target_kinds[i]?7:6]);}}
    for(auto& r:b.lines)for(auto& member:r.value.members)member.model=remap(member.model,m[3]);
    append_pool(a.vertices,b.vertices,m[0],a.high_water[0]);append_pool(a.scales,b.scales,m[1],a.high_water[1]);append_pool(a.subcurves,b.subcurves,m[2],a.high_water[2]);append_pool(a.models,b.models,m[3],a.high_water[3]);append_pool(a.crosspoints,b.crosspoints,m[4],a.high_water[4]);append_pool(a.lines,b.lines,m[5],a.high_water[5]);
}
void remap_planes(Layer& b,const Maps& m){for(auto& p:b.planes){if(p.raster)continue;if(p.opaque||!(p.family&0x20000000u))throw std::invalid_argument("Unsupported vector merge plane");Lists lists(p);remap(lists.vertices,m[0]);remap(lists.models,m[3]);remap(lists.lines,m[5]);remap(lists.faces,m[8]);remap(lists.gradations,m[10]);remap(lists.textures,m[11]);lists.publish(p);}}
void remap_loop(VectorArchiveLoop& v,const Maps& m){for(auto& member:v.members)member.model=remap(member.model,m[3]);v.region=remap(v.region,m[7]);}
void append_paint(VectorPaintArchive& a,VectorPaintArchive& b,Maps& m){
    prepare_base(m,a.base,b.base);m[6]=allocate(a.loops,b.loops);m[7]=allocate(a.regions,b.regions);m[8]=allocate(a.faces,b.faces);m[9]=allocate(a.gradnodes,b.gradnodes);m[10]=allocate(a.gradations,b.gradations);m[11]=allocate(a.textures,b.textures);
    for(auto& r:b.loops)remap_loop(r.value,m);
    for(auto& r:b.regions){remap_loop(r.value.outer,m);remap(r.value.holes,m[6]);remap(r.value.faces,m[8]);}
    for(auto& r:b.faces){auto& f=r.value;if((f.flags&15)!=1)f.color=0;f.region=remap(f.region,m[7]);if((f.flags&15)==2)f.material=remap(f.material,m[10]);else if((f.flags&15)==4)f.material=remap(f.material,m[11]);}
    for(auto& r:b.gradations){auto g=decode_vector_gradation(r.value);remap(g.stops,m[9]);remap(g.faces,m[8]);r.value=encode_vector_gradation(g);}
    for(auto& r:b.textures){auto t=decode_vector_texture(r.value);remap(t.faces,m[8]);r.value=encode_vector_texture(t);}
    remap(b.border.vertices,m[0]);remap(b.border.models,m[3]);remap(b.border.regions,m[7]);append(a.border.vertices,b.border.vertices);append(a.border.models,b.border.models);append(a.border.regions,b.border.regions);
    append_base(a.base,b.base,m);append_pool(a.loops,b.loops,m[6],a.face_high_water[0]);append_pool(a.regions,b.regions,m[7],a.face_high_water[1]);append_pool(a.faces,b.faces,m[8],a.face_high_water[2]);append_pool(a.gradnodes,b.gradnodes,m[9],a.face_high_water[3]);append_pool(a.gradations,b.gradations,m[10],a.face_high_water[4]);append_pool(a.textures,b.textures,m[11],a.face_high_water[5]);
}
template<class T> T& record(std::vector<VectorPoolRecord<T>>& pool,int id){auto it=std::ranges::find(pool,id,&VectorPoolRecord<T>::index);if(it==pool.end())throw std::invalid_argument("Missing vector merge payload");return it->value;}
void clear_line_marks(VectorLineArchive& a,const Lists& list){for(auto id:list.models)record(a.models,id).flags&=~0x10000u;for(auto id:list.lines)for(auto member:record(a.lines,id).members)record(a.models,member.model).flags&=~0x10000u;}
}
void merge_vector_planes(Layer& layer,std::size_t target,std::size_t source){
    auto& to=layer.planes.at(target);const auto& from=layer.planes.at(source);Lists a(to),b(from);
    if(a.paint){
        auto graph=decode_vector_paint_archive(layer);
        for(int id:b.faces){auto& face=record(graph.faces,id);face.flags&=~0x10000u;
            if(face.region>=0){auto& region=record(graph.regions,face.region);const auto old=std::ranges::find_if(region.faces,[&](int f){return std::ranges::find(a.faces,f)!=a.faces.end();});
                if(old!=region.faces.end()){
                    const int removed=*old;region.faces.erase(old);std::erase(a.faces,removed);
                    for(auto& g:graph.gradations){auto value=decode_vector_gradation(g.value);std::erase(value.faces,removed);g.value=encode_vector_gradation(value);}
                    for(auto& t:graph.textures){auto value=decode_vector_texture(t.value);std::erase(value.faces,removed);t.value=encode_vector_texture(value);}
                    std::erase_if(graph.faces,[&](const auto& f){return f.index==removed;});
                }
            }
        }
        append(a.faces,b.faces);append(a.gradations,b.gradations);append(a.textures,b.textures);store_vector_paint_archive(layer,graph);
    }else{
        if(layer.family==0x21200000||layer.family==0x20800000){auto graph=decode_vector_paint_archive(layer);clear_line_marks(graph.base,b);store_vector_paint_archive(layer,graph);}
        else{auto graph=decode_vector_line_archive(layer);clear_line_marks(graph,b);layer.specific=encode_vector_line_archive(graph);}
        append(a.vertices,b.vertices);append(a.models,b.models);append(a.lines,b.lines);
    }
    a.publish(to);
}
void append_vector_layer(Layer& target,const Layer& source){
    const auto original=target.common;const auto extent=merged_owner_bounds(target.common,source);
    expand_raster_layer(target,extent,true);
    const auto first=std::ranges::find_if(source.planes,[](const Plane& p){return !p.raster;});
    const double dpi=first==source.planes.end()?source.common.resolution*16:first->common.resolution;
    const int from_scale=scaled(dpi/source.common.resolution),to_scale=scaled(dpi/original.resolution);
    //63B950 transforms the cloned graph before630270 expands the survivor.
    //The clone retains its plane DPI; expansion then shifts all raw pools16L.
    auto b=source;const auto dx=std::int64_t(source.common.x)*from_scale-std::int64_t(original.x)*to_scale-(std::int64_t(target.common.x)-original.x)*16;
    const auto dy=std::int64_t(source.common.y)*from_scale-std::int64_t(original.y)*to_scale-(std::int64_t(target.common.y)-original.y)*16;
    if(dx<INT32_MIN||dx>INT32_MAX||dy<INT32_MIN||dy>INT32_MAX)throw std::length_error("Vector merge translation exceeds signed32");
    if(dx||dy){std::uint64_t bytes{},work{};auto graph=b;std::erase_if(graph.planes,[](const Plane& p){return p.raster.has_value();});vector_geometry_detail::translate_cache(graph,int(dx),int(dy),bytes,work,{});b.specific=std::move(graph.specific);}
    Maps m;
    if(target.family==0x21200000||target.family==0x20800000){auto a=decode_vector_paint_archive(target),g=decode_vector_paint_archive(b);
        append_paint(a,g,m);store_vector_paint_archive(target,a);
    }else{auto a=decode_vector_line_archive(target),g=decode_vector_line_archive(b);prepare_base(m,a,g);append_base(a,g,m);target.specific=encode_vector_line_archive(a);}
    remap_planes(b,m);
    for(auto& p:b.planes){const auto original_plane=p;p.common.runtime_identity=next_element_identity();p.common.flags[2]=0;p.common.opacity_mode=plane_default_mode(p.family);
        if(p.raster){const double scale=p.raster->resolution/target.common.resolution;const int w=scaled(target.common.width*scale),h=scaled(target.common.height*scale);p.raster=Raster(w,h,p.raster->depth,p.raster->resolution);transfer_raster(p,target.common,original_plane,source.common,false);p.common.width=w;p.common.height=h;p.specific.clear();p.pixels_changed=true;}
        target.planes.push_back(std::move(p));}
    target.runtime_vector_high_water={};
    // Full decoded owner validation includes all child ownership domains.
    std::uint64_t allocation{},work{};auto graph=target;std::erase_if(graph.planes,[](const Plane& p){return p.raster.has_value();});vector_geometry_detail::validate(graph,allocation,work,{});
}
}
