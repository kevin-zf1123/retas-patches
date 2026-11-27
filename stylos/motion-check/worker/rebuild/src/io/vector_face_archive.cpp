#include "retas/vector_face_archive.hpp"
#include "../core/vector_stored_line.hpp"
#include "vector_archive_budget.hpp"
#include "vector_face_projection.hpp"
#include "vector_texture_image.hpp"
#include "vector_exact_integer.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace retas {
namespace {
using vector_archive_detail::ExactInteger;
using vector_archive_detail::exact_product;
constexpr std::size_t count_limit=100000,byte_limit=64U*1024U*1024U;
constexpr std::uint64_t work_limit=8U*1024U*1024U;
[[noreturn]] void bad(const char* message){throw FormatError(0,message);}
std::vector<std::int32_t> refs(Reader& r,std::size_t tail=0){
    const auto n=r.i32();if(n<0||std::size_t(n)>count_limit||r.remaining()<tail||std::size_t(n)>(r.remaining()-tail)/4)bad("Invalid vector paint reference count");
    std::vector<std::int32_t> out;out.reserve(std::size_t(n));for(int i=0;i<n;++i)out.push_back(r.i32());return out;
}
std::vector<VectorLineMember> pairs(Reader& r,std::size_t tail){
    const auto n=r.i32();if(n<0||std::size_t(n)>count_limit||r.remaining()<tail||std::size_t(n)>(r.remaining()-tail)/8)bad("Invalid vector paint member count");
    std::vector<VectorLineMember> out;out.reserve(std::size_t(n));for(int i=0;i<n;++i)out.push_back({r.i32(),r.i32()});return out;
}
VectorBounds read_bounds(Reader& r){return {r.i32(),r.i32(),r.i32(),r.i32()};}
VectorArchiveLoop read_loop(Reader& r){VectorArchiveLoop v;v.members=pairs(r,20);v.region=r.i32();v.archived_bounds=read_bounds(r);return v;}
CommonElement read_common(Reader& r){
    CommonElement c;c.width=r.i32();c.height=r.i32();c.depth=r.i32();c.resolution=r.f64();c.name=r.string();
    for(auto& flag:c.flags)flag=r.i32();c.color=r.u64();c.x=r.i32();c.y=r.i32();
    for(auto& p:c.position){p.value=r.f64();p.unit=r.i32();p.reference=r.f64();}
    c.opacity=r.i32();c.opacity_mode=r.i32();c.field_14=r.i32();c.suffix=r.rest();return c;
}
template<class T,class Decode> std::vector<VectorPoolRecord<T>> read_pool(Reader r,int high,Decode decode){
    std::vector<VectorPoolRecord<T>> out;std::set<int> identities;
    while(r.remaining()){
        if(out.size()>=count_limit)bad("Vector paint pool record limit exceeded");
        const auto id=r.i32();auto body=r.section();if(id<0||id>high||!identities.insert(id).second)bad("Invalid vector paint pool identity");
        auto v=decode(body);out.push_back({id,std::move(v),body.rest()});}
    return out;
}
template<class T> auto index(const std::vector<VectorPoolRecord<T>>& records){
    std::map<int,const T*> out;if(records.size()>count_limit)bad("Vector paint pool record limit exceeded");
    for(const auto& record:records)if(record.index<0||!out.emplace(record.index,&record.value).second)bad("Invalid typed vector paint identity");return out;
}
template<class Map> void live(const Map& map,int id,bool nullable=false){if(id<0){if(!nullable)bad("Negative vector paint list member");}else if(!map.contains(id))bad("Dead typed vector paint reference");}
template<class Map> void unique_refs(const std::vector<int>& list,const Map& map){
    if(list.size()>count_limit)bad("Vector paint reference limit exceeded");std::set<int> seen;
    for(auto id:list){live(map,id);if(!seen.insert(id).second)bad("Duplicate intrusive vector paint member");}
}
struct LoopId {bool embedded{};int index{};auto operator<=>(const LoopId&)const=default;};
std::optional<LoopId> target(const VectorModelCurve& m,int side){if(m.targets[std::size_t(side)]<0)return {};return LoopId{m.target_kinds[std::size_t(side)]!=0,m.targets[std::size_t(side)]};}
struct Graph {
    decltype(index(std::vector<VectorPoolRecord<VectorVertex>>{})) vertices;
    decltype(index(std::vector<VectorPoolRecord<VectorLineScale>>{})) scales;
    decltype(index(std::vector<VectorPoolRecord<VectorSubcurve>>{})) subcurves;
    decltype(index(std::vector<VectorPoolRecord<VectorModelCurve>>{})) models;
    decltype(index(std::vector<VectorPoolRecord<VectorCrosspoint>>{})) crosspoints;
    decltype(index(std::vector<VectorPoolRecord<VectorLine>>{})) lines;
    decltype(index(std::vector<VectorPoolRecord<VectorArchiveLoop>>{})) loops;
    decltype(index(std::vector<VectorPoolRecord<VectorArchiveRegion>>{})) regions;
    decltype(index(std::vector<VectorPoolRecord<VectorArchiveFace>>{})) faces;
    std::map<LoopId,const VectorArchiveLoop*> all_loops;
};
Graph validate(const VectorPaintArchive& a){
    Graph g{index(a.base.vertices),index(a.base.scales),index(a.base.subcurves),index(a.base.models),index(a.base.crosspoints),index(a.base.lines),index(a.loops),index(a.regions),index(a.faces),{}};
    const auto gradnodes=index(a.gradnodes),gradations=index(a.gradations),textures=index(a.textures);(void)gradnodes;
    std::map<int,int> scale_owner,sub_owner,line_owner,hole_owner,face_owner;
    for(const auto& [id,v]:g.vertices){if(v->subcurves.size()>count_limit)bad("Vertex adjacency limit exceeded");for(auto sub:v->subcurves)live(g.subcurves,sub,true);}
    for(const auto& [id,s]:g.subcurves){live(g.vertices,s->vertex_a,true);live(g.vertices,s->vertex_b,true);live(g.models,s->model,true);unique_refs(s->scales,g.scales);
        for(auto scale:s->scales)if(!scale_owner.emplace(scale,id).second)bad("Shared intrusive vector width node");}
    for(const auto& [id,m]:g.models){live(g.lines,m->line,true);unique_refs(m->subcurves,g.subcurves);
        for(auto sub:m->subcurves){if(!sub_owner.emplace(sub,id).second||g.subcurves.at(sub)->model!=id)bad("Subcurve/model ownership disagreement");}
        for(int side=0;side<2;++side){const auto t=target(*m,side);if(t){if(t->embedded)live(g.regions,t->index);else live(g.loops,t->index);}live(g.crosspoints,m->crosspoints[std::size_t(side)],true);}}
    for(const auto& [id,s]:g.subcurves)if(s->model>=0&&(!sub_owner.contains(id)||sub_owner.at(id)!=s->model))bad("Subcurve parent has no reciprocal member");
    for(const auto& [id,l]:g.lines){if(l->members.size()>count_limit)bad("Vector line member limit exceeded");for(auto p:l->members){live(g.models,p.model);
        const auto [i,inserted]=line_owner.emplace(p.model,id);if((!inserted&&i->second!=id)||g.models.at(p.model)->line!=id)bad("Model/line ownership disagreement");}}
    for(const auto& [id,m]:g.models)if(m->line>=0&&(!line_owner.contains(id)||line_owner.at(id)!=m->line))bad("Model parent has no reciprocal line member");
    for(const auto& [id,l]:g.loops){live(g.regions,l->region,true);g.all_loops.emplace(LoopId{false,id},l);}
    for(const auto& [id,r]:g.regions){if(r->outer.region!=id)bad("Embedded outer loop is not self-associated");g.all_loops.emplace(LoopId{true,id},&r->outer);unique_refs(r->holes,g.loops);
        for(auto hole:r->holes)if(!hole_owner.emplace(hole,id).second||g.loops.at(hole)->region!=id)bad("Region/hole ownership disagreement");
        if(r->faces.size()>count_limit)bad("Region face list limit exceeded");for(auto face:r->faces){live(g.faces,face,true);if(face<0)continue;
            const auto [i,inserted]=face_owner.emplace(face,id);if((!inserted&&i->second!=id)||g.faces.at(face)->region!=id)bad("Region/face ownership disagreement");}}
    for(const auto& [id,l]:g.loops)if(l->region>=0&&(!hole_owner.contains(id)||hole_owner.at(id)!=l->region))bad("Hole parent lacks reciprocal region member");
    for(const auto& [id,f]:g.faces){live(g.regions,f->region,true);if(f->region>=0&&(!face_owner.contains(id)||face_owner.at(id)!=f->region))bad("Face parent lacks reciprocal region member");
        if((f->flags&15)==2)live(gradations,f->material,true);if((f->flags&15)==4)live(textures,f->material,true);}
    std::map<std::pair<int,int>,LoopId> lane_owners;
    for(const auto& [loop_id,l]:g.all_loops){if(l->members.size()>count_limit)bad("Loop member limit exceeded");for(auto member:l->members){live(g.models,member.model);const int side=member.selector!=0;
        const auto [owner,inserted]=lane_owners.emplace(std::pair{member.model,side},loop_id);
        if((!inserted&&owner->second!=loop_id)||target(*g.models.at(member.model),side)!=loop_id)bad("Loop/model side ownership disagreement");}}
    for(const auto& [id,m]:g.models)for(int side=0;side<2;++side){const auto t=target(*m,side);const auto i=lane_owners.find({id,side});
        if(bool(t)!=(i!=lane_owners.end())||(t&&i->second!=*t))bad("Model side lacks reciprocal loop member");}
    unique_refs(a.border.vertices,g.vertices);unique_refs(a.border.models,g.models);unique_refs(a.border.regions,g.regions);return g;
}
void write_refs(Writer& w,const std::vector<int>& list){if(list.size()>count_limit)bad("Vector paint reference limit exceeded");w.i32(int(list.size()));for(auto id:list)w.i32(id);}
void write_pairs(Writer& w,const std::vector<VectorLineMember>& list){if(list.size()>count_limit)bad("Vector paint member limit exceeded");w.i32(int(list.size()));for(auto m:list){w.i32(m.model);w.i32(m.selector);}}
void write_bounds(Writer& w,VectorBounds b){w.i32(b.left);w.i32(b.top);w.i32(b.right);w.i32(b.bottom);}
void write_loop(Writer& w,const VectorArchiveLoop& l){write_pairs(w,l.members);w.i32(l.region);write_bounds(w,l.archived_bounds);w.raw(l.suffix);}
void write_common(Writer& w,const CommonElement& c){w.i32(c.width);w.i32(c.height);w.i32(c.depth);w.f64(c.resolution);w.string(c.name);for(auto f:c.flags)w.i32(f);w.u64(c.color);w.i32(c.x);w.i32(c.y);
    for(auto p:c.position){w.f64(p.value);w.i32(p.unit);w.f64(p.reference);}w.i32(c.opacity);w.i32(c.opacity_mode);w.i32(c.field_14);w.raw(c.suffix);}
template<class T,class Encode> void write_pool(Writer& w,const std::vector<VectorPoolRecord<T>>& records,int high,Encode encode){Writer pool;
    for(const auto& r:records){if(r.index>high)bad("Vector paint identity exceeds high-water marker");Writer child;encode(child,r.value);child.raw(r.suffix);pool.i32(r.index);pool.section(child.data);}w.section(pool.data);}
void markers(Reader& r,std::array<int,6>& values){for(auto& m:values){m=r.i32();if(m<-1)bad("Invalid vector paint high-water marker");}}
void markers(Writer& w,const std::array<int,6>& values){for(auto m:values){if(m<-1)bad("Invalid vector paint high-water marker");w.i32(m);}}
std::size_t wire_budget(const VectorPaintArchive& a){
    struct Size {std::size_t total{};void add(std::size_t n){if(n>byte_limit-total)throw std::length_error("Vector paint archive exceeds64MiB");total+=n;}} size;
    const auto list=[&](std::size_t n,std::size_t stride){if(n>count_limit)bad("Vector paint collection limit exceeded");size.add(n*stride);};
    const auto loop=[&](const VectorArchiveLoop& l){size.add(24);list(l.members.size(),8);size.add(l.suffix.size());};
    const auto pools=[&](const auto& records,auto payload){if(records.size()>count_limit)bad("Vector paint pool record limit exceeded");
        for(const auto& r:records){size.add(8);size.add(r.suffix.size());payload(r.value);}};
    size.add(8+16+48+48); // outer/core/border, four children, two six-word headers and twelve pool children
    for(const auto* suffix:{&a.base.header_suffix,&a.base.body_suffix,&a.face_header_suffix,&a.face_body_suffix,&a.core_suffix,&a.outer_suffix})size.add(suffix->size());
    pools(a.base.vertices,[&](const VectorVertex& v){size.add(12);list(v.subcurves.size(),4);});
    pools(a.base.scales,[&](const auto&){size.add(8);});pools(a.base.subcurves,[&](const VectorSubcurve& v){size.add(24);list(v.scales.size(),4);});
    pools(a.base.models,[&](const VectorModelCurve& v){size.add(68);list(v.subcurves.size(),4);});pools(a.base.crosspoints,[&](const auto&){size.add(56);});
    pools(a.base.lines,[&](const VectorLine& v){size.add(32);list(v.members.size(),8);});pools(a.loops,loop);
    pools(a.regions,[&](const VectorArchiveRegion& v){size.add(12);loop(v.outer);list(v.holes.size(),4);list(v.faces.size(),4);});
    pools(a.faces,[&](const VectorArchiveFace& f){const auto tag=f.flags&15;size.add(tag==1?16:tag==2||tag==4?12:8);});
    const auto raw=[&](const Bytes& b){size.add(b.size());};pools(a.gradnodes,raw);pools(a.gradations,raw);pools(a.textures,raw);
    size.add(4+110+a.border.common.name.size());size.add(a.border.common.suffix.size());size.add(4+4+12);
    list(a.border.vertices.size(),4);list(a.border.models.size(),4);list(a.border.regions.size(),4);
    for(const auto* suffix:{&a.border.topology_suffix,&a.border.specific_suffix,&a.border.record_suffix})size.add(suffix->size());
    return size.total;
}
struct PaintRefs {std::vector<int> faces,gradations,textures;};
PaintRefs paint_refs(const Plane& p){Reader r(p.specific);return {refs(r),refs(r),refs(r)};}
void plane_ownership(const VectorPaintArchive& a,const Layer& layer,const Graph& g){
    const auto grads=index(a.gradations),textures=index(a.textures);std::set<int> faces,gradations,tex;
    for(const auto& p:layer.planes){if(p.opaque)throw std::invalid_argument("Opaque plane prevents paint ownership projection");
        if(p.family!=0x22100001&&p.family!=0x22100002)continue;const auto lists=paint_refs(p);
        unique_refs(lists.faces,g.faces);unique_refs(lists.gradations,grads);unique_refs(lists.textures,textures);
        for(auto id:lists.faces)if(!faces.insert(id).second)bad("Face shared across paint planes");
        for(auto id:lists.gradations)if(!gradations.insert(id).second)bad("Gradation shared across paint planes");
        for(auto id:lists.textures)if(!tex.insert(id).second)bad("Texture shared across paint planes");}
}
struct GradientProjection {
    std::map<int,std::shared_ptr<const VectorGradationMaterial>> materials;
};
struct TextureView {
    std::span<const std::uint8_t> offscreen,faces;
    VectorTextureMaterial material;
};
TextureView texture_view(std::span<const std::uint8_t> raw){
    if(raw.size()>byte_limit)throw std::length_error("Vector texture leaf exceeds64MiB");
    Reader r(raw);const auto image_size=r.u32();r.take(image_size);TextureView v;v.offscreen=raw.first(std::size_t(image_size)+4);
    const auto string_size=r.u16();r.take(string_size);v.material.flags=r.u32();
    const auto n=r.i32();r.require_count(n,4);if(r.remaining()<24||std::size_t(n)>(r.remaining()-24)/4)bad("Invalid vector texture reverse face count");v.faces=r.take(std::size_t(n)*4);
    v.material.origin_x=r.i32();v.material.origin_y=r.i32();v.material.scale_x_bits=r.u32();v.material.scale_y_bits=r.u32();v.material.rotation_bits=r.u32();v.material.shear_x_bits=r.u32();return v;
}
using TextureProjection=std::map<int,std::shared_ptr<const VectorTextureMaterial>>;
using TexturePreparation=std::map<int,TextureView>;
TexturePreparation prepare_textures(const VectorPaintArchive& a,const Layer& layer,const Graph& graph,std::uint64_t reserved){
    constexpr std::uint64_t limit=512ull*1024*1024;
    const auto raw=index(a.textures),raw_grad=index(a.gradations);std::map<int,int> texture_planes,face_planes;std::map<int,TextureView> views;
    std::size_t references{};std::uint64_t storage=reserved;
    const auto charge=[&](std::uint64_t n){if(storage>limit||n>limit-storage)throw std::length_error("Paint owner material storage exceeds512MiB");storage+=n;};
    charge(2*wire_budget(a));for(const auto& p:layer.planes)charge(p.specific.size());
    // Both source archive and typed raw archive are alive during production
    // projection. Bill before image decompression or any material expansion.
    for(std::size_t plane=0;plane<layer.planes.size();++plane){const auto& p=layer.planes[plane];if(p.family!=0x22100001&&p.family!=0x22100002)continue;
        const auto lists=paint_refs(p);for(auto face:lists.faces)face_planes.emplace(face,int(plane));
        for(auto grad:lists.gradations){Reader g(*raw_grad.at(grad));g.u32();const auto n=g.i32();g.require_count(n,4);charge(2*std::uint64_t(n)*sizeof(VectorGradationStop));}
        for(auto id:lists.textures){auto v=texture_view(*raw.at(id));const auto n=v.faces.size()/4;
            if(n>count_limit-references)throw std::length_error("Active texture aggregate face-reference limit exceeded");references+=n;
            if((v.material.flags&15)!=1&&(v.material.flags&15)!=2)throw std::invalid_argument("Unknown active texture cursor mode");
            (void)vector_texture_inverse_matrix(v.material);
            charge(2*vector_archive_detail::texture_image_storage(v.offscreen));texture_planes.emplace(id,int(plane));views.emplace(id,std::move(v));}
    }
    std::map<int,int> inverse;
    for(const auto& [id,v]:views){Reader faces(v.faces);while(faces.remaining()){const int face=faces.i32();live(graph.faces,face,true);if(face<0)continue;const auto& f=*graph.faces.at(face);
        if((f.flags&15)!=4||f.material!=id||!face_planes.contains(face)||face_planes.at(face)!=texture_planes.at(id))bad("Texture reverse face/plane disagreement");
        const auto [i,inserted]=inverse.emplace(face,id);if(!inserted&&i->second!=id)bad("Competing texture reverse face owners");}}
    for(const auto& [id,plane]:face_planes){const auto& f=*graph.faces.at(id);if((f.flags&15)!=4)continue;
        if(f.material<0||!texture_planes.contains(f.material)||texture_planes.at(f.material)!=plane||!inverse.contains(id)||inverse.at(id)!=f.material)
            bad("Texture face lacks reciprocal same-plane material owner");}
    return views;
}
TextureProjection decode_textures(TexturePreparation views,const Layer& layer,const Graph& graph,const std::vector<VectorPaintPlaneGeometry>& geometry){
    // All geometry/ownership/material caps are closed before image expansion.
    // These pure plans account for actual writer queries, including zero-alpha
    // or zero-coverage endpoints; no remote unpainted owner point is sampled.
    const VectorRenderContext context{layer.common.width,layer.common.height,32,layer.common.resolution,16,{},std::nullopt,2};
    for(const auto& plane:geometry)for(const auto& face:plane.faces)if((face.geometry.material_flags&15)==4)
        admit_vector_face_texture_queries(face.geometry,context,views.at(graph.faces.at(face.face_index)->material).material);
    TextureProjection result;
    for(auto& [id,v]:views){Reader image(v.offscreen);v.material.image=decode_offscreen(image);(void)vector_texture_storage(v.material);
        result.emplace(id,std::make_shared<VectorTextureMaterial>(std::move(v.material)));}
    return result;
}
GradientProjection gradients(const VectorPaintArchive& a,const Layer& layer,const Graph& graph){
    const auto raw_gradations=index(a.gradations),raw_nodes=index(a.gradnodes);
    std::map<int,int> grad_planes,face_planes,node_owners,face_owners;std::map<int,std::size_t> stop_counts;
    std::size_t nodes{},references{},occurrences{};
    GradientProjection result;
    // Preflight every active chain count before allocating typed leaf lists or
    // expanded stop vectors. Hidden siblings participate in this admission.
    for(std::size_t plane=0;plane<layer.planes.size();++plane){const auto& p=layer.planes[plane];
        if(p.family!=0x22100001&&p.family!=0x22100002)continue;const auto lists=paint_refs(p);
        for(auto face:lists.faces)face_planes.emplace(face,int(plane));
        for(auto grad:lists.gradations){grad_planes.emplace(grad,int(plane));Reader r(*raw_gradations.at(grad));r.u32();
            const auto n=r.i32();if(n<0||std::size_t(n)>count_limit-nodes||r.remaining()<24||std::size_t(n)>(r.remaining()-24)/4)
                throw std::length_error("Active gradation aggregate stop limit exceeded");
            nodes+=std::size_t(n);stop_counts.emplace(grad,std::size_t(n));
            r.bytes(std::size_t(n)*4);const auto f=r.i32();if(f<0||std::size_t(f)>count_limit-references||r.remaining()<20||std::size_t(f)>(r.remaining()-20)/4)
                throw std::length_error("Active gradation aggregate face-reference limit exceeded");
            references+=std::size_t(f);
        }
    }
    // Bill occurrences before any material stop expansion, even if thousands
    // of faces share one large immutable chain.
    for(const auto& [face,plane]:face_planes){const auto& f=*graph.faces.at(face);if((f.flags&15)!=2)continue;
        if(f.material<0||!grad_planes.contains(f.material)||grad_planes.at(f.material)!=plane)
            bad("Gradient face lacks same-plane material owner");
        const auto n=stop_counts.at(f.material);if(n>count_limit-occurrences)
            throw std::length_error("Active per-face gradation stop occurrence limit exceeded");
        occurrences+=n;
    }
    for(const auto& [id,plane]:grad_planes){const auto leaf=decode_vector_gradation(*raw_gradations.at(id));
        auto material=std::make_shared<VectorGradationMaterial>();material->flags=leaf.flags;
        material->start_x=leaf.start_x;material->start_y=leaf.start_y;material->end_x=leaf.end_x;material->end_y=leaf.end_y;material->minor_ratio_bits=leaf.minor_ratio_bits;
        // The aggregate count above precedes reserve/copy even for many faces
        // sharing one chain. Intrusive nodes cannot be shared between chains.
        material->stops.reserve(leaf.stops.size());
        for(auto stop:leaf.stops){live(raw_nodes,stop);if(!node_owners.emplace(stop,id).second)bad("Shared or duplicate active gradation stop node");
            const auto node=decode_vector_gradnode(*raw_nodes.at(stop));material->stops.push_back({node.color,node.position_bits});}
        for(auto face:leaf.faces){live(graph.faces,face,true);if(face<0)continue;const auto& f=*graph.faces.at(face);
            if((f.flags&15)!=2||f.material!=id||!face_planes.contains(face)||face_planes.at(face)!=plane)bad("Active gradation reverse face/plane disagreement");
            const auto [owner,inserted]=face_owners.emplace(face,id);if(!inserted&&owner->second!=id)bad("Competing active gradation face owners");}
        (void)VectorGradationSampler(material);result.materials.emplace(id,std::move(material));
    }
    for(const auto& [face,plane]:face_planes){const auto& f=*graph.faces.at(face);if((f.flags&15)!=2)continue;
        if(f.material<0||!grad_planes.contains(f.material)||grad_planes.at(f.material)!=plane||!face_owners.contains(face)||face_owners.at(face)!=f.material)
            bad("Gradient face lacks reciprocal same-plane material owner");
    }
    return result;
}
using P=VectorFaceVertex;
struct Certification {
    std::uint64_t work{};std::size_t vertices{},spans{},members{};
    void charge(std::uint64_t n=1){if(n>work_limit-work)throw std::length_error("Persistent face certification work limit exceeded");work+=n;}
    void expand(std::size_t& count,std::size_t n){if(n>count_limit-count)throw std::length_error("Persistent face aggregate projection limit exceeded");count+=n;}
};
std::int64_t cross(P a,P b,P c){return (std::int64_t(b.x)-a.x)*(std::int64_t(c.y)-a.y)-(std::int64_t(b.y)-a.y)*(std::int64_t(c.x)-a.x);}
bool on(P a,P b,P p){return cross(a,b,p)==0&&p.x>=std::min(a.x,b.x)&&p.x<=std::max(a.x,b.x)&&p.y>=std::min(a.y,b.y)&&p.y<=std::max(a.y,b.y);}
int sign(std::int64_t n){return (n>0)-(n<0);}
bool intersects(P a,P b,P c,P d){const auto u=cross(a,b,c),v=cross(a,b,d),w=cross(c,d,a),z=cross(c,d,b);
    return (!u&&on(a,b,c))||(!v&&on(a,b,d))||(!w&&on(c,d,a))||(!z&&on(c,d,b))||(sign(u)*sign(v)<0&&sign(w)*sign(z)<0);}
bool inside(const std::vector<P>& p,P q){bool odd=false;for(std::size_t i=0;i<p.size();++i){const auto a=p[i],b=p[(i+1)%p.size()];if(on(a,b,q))return false;
    if((a.y>q.y)!=(b.y>q.y)){const auto c=cross(a,b,q);if((b.y>a.y&&c>0)||(b.y<a.y&&c<0))odd=!odd;}}return odd;}
struct ContourEdge {P a,c,b;std::uint32_t start{},end{};bool quadratic{};};
struct Hull {std::array<P,3> points{};std::size_t count{};};
std::vector<ContourEdge> contour(const VectorFaceGeometry& g,std::uint32_t loop,bool simple,std::size_t minimum=2){
    std::vector<ContourEdge> out;std::set<std::pair<std::uint32_t,bool>> traversed;
    const auto add=[&](const VectorFaceModel& m,const VectorFaceSpan& span,bool forward){const auto a=forward?span.start:span.end,b=forward?span.end:span.start;
        if(!out.empty()&&out.back().end!=a)throw std::invalid_argument("Disconnected face contour");out.push_back({g.vertices.at(a),{span.control_x,span.control_y},g.vertices.at(b),a,b,!(m.flags&1)});};
    for(const auto& member:g.loops.at(loop).members){if(!traversed.emplace(member.model,member.selector!=0).second)throw std::invalid_argument("Repeated model-side traversal in persistent face closure");
        const auto& model=g.models.at(member.model);if(simple&&model.outside_loop==model.inside_loop)throw std::invalid_argument("Internal face edges require topology normalization");
        if(member.selector)for(const auto& span:model.spans)add(model,span,true);else for(auto i=model.spans.rbegin();i!=model.spans.rend();++i)add(model,*i,false);}
    if(out.size()<minimum||out.front().start!=out.back().end)throw std::invalid_argument("Face contour must be identity-closed");
    //5BEC60 closes walks by visited model-side, not by unique Vertex.
    //Drawn contours receive their bounded simple/pinched certificate below;
    //undrawn adjacent closures still require ordered identity closure.
    return out;
}
Hull hull(const ContourEdge& e){
    if(e.a==e.b)throw std::invalid_argument("Collapsed persistent face edge");
    if(!e.quadratic)return {{{e.a,e.b,{}}},2};
    const auto d=cross(e.a,e.c,e.b);if(!d){if(!on(e.a,e.b,e.c))throw std::invalid_argument("Backtracking quadratic face edge");return {{{e.a,e.b,{}}},2};}
    return d>0?Hull{{e.a,e.c,e.b},3}:Hull{{e.a,e.b,e.c},3};
}
bool separated(const Hull& a,const Hull& b,Certification& budget,bool strict=false){
    const auto axes=[&](const Hull& axis){for(std::size_t i=0;i<axis.count;++i){budget.charge(a.count+b.count);const auto p=axis.points[i],q=axis.points[(i+1)%axis.count];auto amin=cross(p,q,a.points[0]),amax=amin,bmin=cross(p,q,b.points[0]),bmax=bmin;
        for(std::size_t j=1;j<a.count;++j){const auto v=cross(p,q,a.points[j]);amin=std::min(amin,v);amax=std::max(amax,v);}
        for(std::size_t j=1;j<b.count;++j){const auto v=cross(p,q,b.points[j]);bmin=std::min(bmin,v);bmax=std::max(bmax,v);}
        if(strict?(amax<bmin||bmax<amin):(amax<=bmin||bmax<=amin))return true;}return false;};return axes(a)||axes(b);
}
// Fixed dyadic units make every split exact without changing stored geometry.
// |raw| < 2^27, scale = 4^16, so all convex-combination coordinates are
// < 2^59. Differences are < 2^60 and each determinant is < 2^121; signed
// Signed 128-bit arithmetic covers every SAT/containment predicate. A depth-d node
// remains divisible by 4^(16-d), so /2 and /4 never round at a permitted split.
constexpr unsigned subdivision_depth=16;
constexpr std::int64_t subdivision_scale=std::int64_t{1}<<(2*subdivision_depth);
struct ExactPoint {std::int64_t x{},y{};bool operator==(const ExactPoint&)const=default;};
struct ExactCurve {ExactPoint a,c,b;bool quadratic{};unsigned depth{};};
struct ExactHull {std::array<ExactPoint,3> points;std::size_t count;};
ExactInteger cross(ExactPoint a,ExactPoint b,ExactPoint c){return exact_product(b.x-a.x,c.y-a.y)-exact_product(b.y-a.y,c.x-a.x);}
ExactPoint exact_point(P p){return {std::int64_t(p.x)*subdivision_scale,std::int64_t(p.y)*subdivision_scale};}
ExactCurve exact_curve(const ContourEdge& e){return {exact_point(e.a),exact_point(e.c),exact_point(e.b),e.quadratic,0};}
ExactHull exact_hull(const ExactCurve& e){
    if(!e.quadratic)return {{{e.a,e.b,{}}},2};
    const auto d=cross(e.a,e.c,e.b);
    if(!d){
        // A quadratic's convex hull includes its control even when collinear.
        // The public contour gate rejects backtracking originals, but this
        // exact helper also covers folded children without dropping controls.
        const auto less=[](ExactPoint a,ExactPoint b){return a.x<b.x||(a.x==b.x&&a.y<b.y);};
        const auto points=std::array{e.a,e.c,e.b};const auto ends=std::minmax_element(points.begin(),points.end(),less);
        return {{{*ends.first,*ends.second,{}}},2};
    }
    return d>0?ExactHull{{e.a,e.c,e.b},3}:ExactHull{{e.a,e.b,e.c},3};
}
std::array<ExactCurve,2> split_exact(const ExactCurve& e,Certification& budget){
    budget.charge(18);
    const ExactPoint l{(e.a.x+e.c.x)/2,(e.a.y+e.c.y)/2},r{(e.c.x+e.b.x)/2,(e.c.y+e.b.y)/2};
    const ExactPoint m{(e.a.x+2*e.c.x+e.b.x)/4,(e.a.y+2*e.c.y+e.b.y)/4};
    return {{{e.a,l,m,true,e.depth+1},{m,r,e.b,true,e.depth+1}}};
}
bool strictly_separated(const ExactHull& a,const ExactHull& b,Certification& budget,bool strict=true){
    const auto axes=[&](const ExactHull& axis){for(std::size_t i=0;i<axis.count;++i){budget.charge(a.count+b.count);
        const auto p=axis.points[i],q=axis.points[(i+1)%axis.count];auto amin=cross(p,q,a.points[0]),amax=amin,bmin=cross(p,q,b.points[0]),bmax=bmin;
        for(std::size_t j=1;j<a.count;++j){const auto v=cross(p,q,a.points[j]);amin=std::min(amin,v);amax=std::max(amax,v);}
        for(std::size_t j=1;j<b.count;++j){const auto v=cross(p,q,b.points[j]);bmin=std::min(bmin,v);bmax=std::max(bmax,v);}
        if(strict?(amax<bmin||bmax<amin):(amax<=bmin||bmax<=amin))return true;}return false;};
    return axes(a)||axes(b);
}
bool joint_hulls_separated(const ExactCurve& a,const ExactCurve& b,ExactPoint p,Certification& budget){
    budget.charge(8);if((a.a!=p&&a.b!=p)||(b.a!=p&&b.b!=p))return false;
    const auto ha=exact_hull(a),hb=exact_hull(b);
    const auto on_segment=[](ExactPoint a,ExactPoint b,ExactPoint q){return cross(a,b,q)==0&&q.x>=std::min(a.x,b.x)&&q.x<=std::max(a.x,b.x)&&q.y>=std::min(a.y,b.y)&&q.y<=std::max(a.y,b.y);};
    // Nondegenerate quadratic interiors are strictly inside their triangles.
    // A segment or another triangle can touch them only at actual endpoints
    // under weak SAT. Every endpoint except the approved pinch stays illegal.
    for(auto x:{a.a,a.b})for(auto y:{b.a,b.b})if(x==y&&x!=p)return false;
    if((ha.count==2&&ha.points[0]!=p&&ha.points[1]!=p)||(hb.count==2&&hb.points[0]!=p&&hb.points[1]!=p))return false;
    if(ha.count==2&&hb.count==2){const auto x=ha.points[0]==p?ha.points[1]:ha.points[0],y=hb.points[0]==p?hb.points[1]:hb.points[0];
        if(cross(p,x,y))return true;
        return exact_product(x.x-p.x,y.x-p.x)+exact_product(x.y-p.y,y.y-p.y)<0;}
    if(ha.count==2)for(auto x:{b.a,b.b})if(x!=p&&on_segment(ha.points[0],ha.points[1],x))return false;
    if(hb.count==2)for(auto x:{a.a,a.b})if(x!=p&&on_segment(hb.points[0],hb.points[1],x))return false;
    return strictly_separated(ha,hb,budget,false);
}
bool curves_strictly_separated(const ExactCurve& a,const ExactCurve& b,Certification& budget,std::optional<ExactPoint> joint={}){
    budget.charge();if(joint&&joint_hulls_separated(a,b,*joint,budget))return true;
    if(strictly_separated(exact_hull(a),exact_hull(b),budget))return true;
    // Split only an unresolved pair, visiting each child against the entire
    // other arc. The call stack has at most32 splits; the shared work cap
    // bounds all branching, including genuine contact and overlapping arcs.
    const bool split_a=a.quadratic&&a.depth<subdivision_depth;
    const bool split_b=b.quadratic&&b.depth<subdivision_depth;
    if(!split_a&&!split_b)return false;
    if(split_a&&(!split_b||a.depth<=b.depth)){const auto children=split_exact(a,budget);
        return curves_strictly_separated(children[0],b,budget,joint)&&curves_strictly_separated(children[1],b,budget,joint);}
    const auto children=split_exact(b,budget);
    return curves_strictly_separated(a,children[0],budget,joint)&&curves_strictly_separated(a,children[1],budget,joint);
}
bool adaptive_strictly_separated(const ContourEdge& a,const ContourEdge& b,Certification& budget,std::optional<P> joint={}){
    budget.charge(6);return curves_strictly_separated(exact_curve(a),exact_curve(b),budget,joint?std::optional{exact_point(*joint)}:std::nullopt);
}
bool exact_hull_contains(const ExactHull& h,ExactPoint p,Certification& budget){
    budget.charge(3);if(h.count==2)return cross(h.points[0],h.points[1],p)==0&&p.x>=std::min(h.points[0].x,h.points[1].x)&&p.x<=std::max(h.points[0].x,h.points[1].x)&&p.y>=std::min(h.points[0].y,h.points[1].y)&&p.y<=std::max(h.points[0].y,h.points[1].y);
    return cross(h.points[0],h.points[1],p)>=0&&cross(h.points[1],h.points[2],p)>=0&&cross(h.points[2],h.points[0],p)>=0;
}
std::optional<bool> certified_chord_parity(const ExactCurve& e,ExactPoint p,Certification& budget){
    budget.charge();if(!exact_hull_contains(exact_hull(e),p,budget)){
        // The arc-to-chord homotopy stays in its closed child control hull.
        // Since p is outside, the chord's ray parity equals the arc's parity.
        if((e.a.y>p.y)==(e.b.y>p.y))return false;
        const auto c=cross(e.a,e.b,p);return (e.b.y>e.a.y&&c>0)||(e.b.y<e.a.y&&c<0);
    }
    if(!e.quadratic||e.depth==subdivision_depth)return {};
    const auto children=split_exact(e,budget);const auto l=certified_chord_parity(children[0],p,budget);
    if(!l)return {};const auto r=certified_chord_parity(children[1],p,budget);
    if(!r)return {};return *l!=*r;
}
bool hull_contains(const Hull& h,P q){if(h.count==2)return on(h.points[0],h.points[1],q);
    return cross(h.points[0],h.points[1],q)>=0&&cross(h.points[1],h.points[2],q)>=0&&cross(h.points[2],h.points[0],q)>=0;
}
void certify_pinch_routing(const std::vector<ContourEdge>& edges,std::size_t first,std::size_t second,Certification& budget){
    const auto p=edges[first].a;
    const auto away=[&](const ContourEdge& edge,bool outgoing){const auto q=edge.quadratic&&edge.c!=p?edge.c:outgoing?edge.b:edge.a;return P{q.x-p.x,q.y-p.y};};
    std::array<std::pair<P,int>,4> rays{{{away(edges[(first+edges.size()-1)%edges.size()],false),0},{away(edges[first],true),0},
        {away(edges[(second+edges.size()-1)%edges.size()],false),1},{away(edges[second],true),1}}};
    budget.charge(16);
    for(const auto& ray:rays)if(ray.first==P{})throw std::invalid_argument("Collapsed persistent face pinch ray");
    for(std::size_t i=0;i<rays.size();++i)for(std::size_t j=i+1;j<rays.size();++j){const auto a=rays[i].first,b=rays[j].first;
        if(!cross(P{},a,b)&&std::int64_t(a.x)*b.x+std::int64_t(a.y)*b.y>0)throw std::invalid_argument("Ambiguous persistent face pinch rays");}
    const auto upper=[](P p){return p.y>0||(p.y==0&&p.x>0);};
    std::sort(rays.begin(),rays.end(),[&](const auto& a,const auto& b){const auto u=upper(a.first),v=upper(b.first);return u!=v?u:cross(P{},a.first,b.first)>0;});
    if(rays[0].second==rays[2].second&&rays[1].second==rays[3].second)throw std::invalid_argument("Crossing persistent face pinch routing");
}
// A native hole walk may contain a real boundary with doubled internal
// excursions. Factor only the proof: emission skips internal Models, while
// Region queries must retain their original sides and physical walk order.
struct AttachedTree {std::vector<ContourEdge> edges;std::uint32_t root{};};
struct MixedHole {std::vector<ContourEdge> boundary;std::vector<AttachedTree> trees;};
std::optional<MixedHole> mixed_hole(const VectorFaceGeometry& g,std::uint32_t loop,Certification& budget){
    bool internal=false,boundary=false;
    for(const auto& member:g.loops.at(loop).members){budget.charge();const auto& m=g.models.at(member.model);
        if(m.outside_loop==m.inside_loop)internal=true;else boundary=true;}
    if(!internal||!boundary)return {};
    MixedHole proof;std::map<std::uint32_t,unsigned> lanes;std::vector<ContourEdge> physical;
    std::map<std::uint32_t,std::vector<std::size_t>> incident;
    for(const auto& member:g.loops.at(loop).members){budget.charge();const auto& m=g.models.at(member.model);
        if(m.outside_loop!=m.inside_loop){
            const auto add=[&](const VectorFaceSpan& span,bool forward){budget.charge();
                const auto a=forward?span.start:span.end,b=forward?span.end:span.start;
                if(!proof.boundary.empty()&&proof.boundary.back().end!=a)throw std::invalid_argument("Disconnected mixed face hole boundary");
                proof.boundary.push_back({g.vertices.at(a),{span.control_x,span.control_y},g.vertices.at(b),a,b,!(m.flags&1)});};
            if(member.selector)for(const auto& span:m.spans)add(span,true);
            else for(auto i=m.spans.rbegin();i!=m.spans.rend();++i)add(*i,false);
            continue;
        }
        if(m.outside_loop!=int(loop)||m.inside_loop!=int(loop)||!(m.flags&1))
            throw std::invalid_argument("Mixed face hole requires same-loop linear branches");
        const unsigned lane=member.selector?2u:1u;auto& used=lanes[member.model];
        if(used&lane)throw std::invalid_argument("Repeated mixed face hole model side");
        if(!used)for(const auto& span:m.spans){budget.charge(3);const auto edge=physical.size();
            physical.push_back({g.vertices.at(span.start),{span.control_x,span.control_y},g.vertices.at(span.end),span.start,span.end,false});
            incident[span.start].push_back(edge);incident[span.end].push_back(edge);}
        used|=lane;
    }
    for(const auto& [model,used]:lanes){budget.charge();(void)model;
        if(used!=3)throw std::invalid_argument("Incomplete mixed face hole model sides");}
    if(proof.boundary.size()<2||proof.boundary.front().start!=proof.boundary.back().end)
        throw std::invalid_argument("Mixed face hole boundary must be identity-closed");
    std::set<std::uint32_t> boundary_vertices;
    for(const auto& edge:proof.boundary){budget.charge();
        if(!boundary_vertices.insert(edge.start).second)throw std::invalid_argument("Repeated mixed face hole boundary vertex");}
    std::set<std::uint32_t> reached;
    for(const auto& [first,unused]:incident){budget.charge();(void)unused;if(reached.contains(first))continue;
        AttachedTree tree;std::set<std::size_t> edges;std::vector<std::uint32_t> pending{first};std::size_t vertices{},roots{};
        while(!pending.empty()){budget.charge();const auto vertex=pending.back();pending.pop_back();if(!reached.insert(vertex).second)continue;
            ++vertices;if(boundary_vertices.contains(vertex)){tree.root=vertex;++roots;}
            for(auto id:incident.at(vertex)){budget.charge();edges.insert(id);const auto& edge=physical[id];
                const auto peer=edge.start==vertex?edge.end:edge.start;if(!reached.contains(peer))pending.push_back(peer);}}
        if(roots!=1||edges.size()+1!=vertices)throw std::invalid_argument("Mixed face hole branch must be a singly-rooted tree");
        for(auto id:edges){budget.charge();tree.edges.push_back(physical[id]);}proof.trees.push_back(std::move(tree));
    }
    return proof;
}
struct InternalHole {std::vector<ContourEdge> edges;bool linear_tree{};};
std::optional<InternalHole> internal_hole(const VectorFaceGeometry& g,std::uint32_t loop,Certification& budget){
    const auto& members=g.loops.at(loop).members;bool internal=false;
    for(const auto& member:members){budget.charge();const auto& m=g.models.at(member.model);internal|=m.outside_loop==m.inside_loop;}
    if(!internal)return {};
    std::map<std::uint32_t,unsigned> lanes;std::vector<ContourEdge> physical;
    std::map<std::uint32_t,std::vector<std::size_t>> incident;
    for(const auto& member:members){budget.charge();const auto& m=g.models.at(member.model);
        if(m.outside_loop!=int(loop)||m.inside_loop!=int(loop))throw std::invalid_argument("Mixed internal and boundary face hole");
        const unsigned lane=member.selector?2u:1u;auto& used=lanes[member.model];
        if(used&lane)throw std::invalid_argument("Repeated internal face hole model side");
        if(!used)for(const auto& span:m.spans){budget.charge(3);const auto edge=physical.size();
            physical.push_back({g.vertices.at(span.start),{span.control_x,span.control_y},g.vertices.at(span.end),span.start,span.end,!(m.flags&1)});
            incident[span.start].push_back(edge);incident[span.end].push_back(edge);}
        used|=lane;
    }
    for(const auto& [model,used]:lanes){budget.charge();(void)model;if(used!=3)throw std::invalid_argument("Incomplete internal face hole model sides");}
    bool linear=true;for(const auto& edge:physical){budget.charge();linear&=!edge.quadratic;}
    if(linear){
        if(physical.size()+1!=incident.size())throw std::invalid_argument("Internal linear face hole must be acyclic");
        std::set<std::uint32_t> reached;std::vector<std::uint32_t> pending{incident.begin()->first};
        while(!pending.empty()){budget.charge();const auto vertex=pending.back();pending.pop_back();if(!reached.insert(vertex).second)continue;
            for(auto id:incident.at(vertex)){budget.charge();const auto& edge=physical[id];const auto peer=edge.start==vertex?edge.end:edge.start;
                if(!reached.contains(peer))pending.push_back(peer);}}
        if(reached.size()!=incident.size())throw std::invalid_argument("Disconnected internal linear face hole");
        // 5BEC60/5BEB40 can return through a higher-degree identity fan. The
        // modern certificate admits only embedded linear trees: connected
        // E=V-1 plus the original pairwise geometry gates below. Keep the
        // physical proof order separate from the untouched doubled walk.
        return InternalHole{std::move(physical),true};
    }
    std::optional<std::uint32_t> first;std::size_t ends{};
    for(const auto& [vertex,edges]:incident){budget.charge();if(edges.size()==1){if(!first)first=vertex;++ends;}
        else if(edges.size()!=2)throw std::invalid_argument("Branched quadratic internal face hole is unsupported");}
    if(ends!=2)throw std::invalid_argument("Internal face hole must have an open spine");
    // Native doubled walks can start at an interior Model (half ellipse:
    // MC4+,MC4-,MC5-,MC5+). Only the proof starts at a degree-one Vertex;
    // neither the persistent nor the lowered member chain is reordered.
    std::vector<ContourEdge> chain;chain.reserve(physical.size());std::vector<bool> visited(physical.size());auto vertex=*first;
    for(;;){budget.charge();std::optional<std::size_t> next;
        for(auto edge:incident.at(vertex)){budget.charge();if(!visited[edge]){next=edge;break;}}
        if(!next)break;visited[*next]=true;auto edge=physical[*next];
        if(edge.start!=vertex){std::swap(edge.a,edge.b);std::swap(edge.start,edge.end);}chain.push_back(edge);vertex=edge.end;
    }
    if(chain.size()!=physical.size())throw std::invalid_argument("Disconnected internal face hole spine");
    return InternalHole{std::move(chain),false};
}
void certify(const VectorFaceGeometry& g,Certification& budget){
    //5987E0 returns before hole emission for a canonical empty main bbox.
    //Its secondary walks may be open MC chains traversed forward then back,
    //or selftouching exterior walks. lower() still certifies their ordered
    //identity closure and unique reciprocal model-sides; no simple-polygon,
    //area or separation condition belongs to these undrawn contours.
    if(g.loops.at(g.outer_loop).members.empty())return;
    struct Loop {std::vector<ContourEdge> edges;std::vector<Hull> hulls;std::vector<P> chords;ExactInteger area{};bool open{},linear_tree{};std::optional<std::uint32_t> root;std::optional<std::size_t> boundary;};std::vector<Loop> loops,rods;
    std::optional<std::uint32_t> pinch;
    const auto append=[&](std::vector<ContourEdge> edges){Loop l;l.edges=std::move(edges);
        const auto wedge=[](P a,P b){return exact_product(a.x,b.y)-exact_product(a.y,b.x);};
        if(l.edges.size()<2)throw std::invalid_argument("Collapsed persistent face pinch cycle");
        for(const auto& e:l.edges){budget.charge();l.hulls.push_back(hull(e));l.chords.push_back(e.a);
            const auto end=wedge(e.a,e.b),left=wedge(e.a,e.c),right=wedge(e.c,e.b);
            l.area+=e.quadratic?end+left+left+right+right:end+end+end;}
        if(!l.area)throw std::invalid_argument("Zero-area persistent face contour");loops.push_back(std::move(l));};
    const auto add=[&](std::uint32_t id){auto edges=contour(g,id,true);std::map<std::uint32_t,std::size_t> seen;std::optional<std::pair<std::size_t,std::size_t>> repeat;
        for(std::size_t i=0;i<edges.size();++i){budget.charge();const auto [at,inserted]=seen.emplace(edges[i].start,i);
            if(!inserted){if(repeat||id!=g.outer_loop||!g.holes.empty())throw std::invalid_argument("Repeated face contour vertex");repeat=std::pair{at->second,i};}}
        if(!repeat){append(std::move(edges));return;}
        // A single degree4 pinch can encode an outer cycle and a contained
        // hole in one native model-side walk. This proof is transient: every
        // original span/cache/order is still passed unchanged to the writer.
        const auto [first,second]=*repeat;certify_pinch_routing(edges,first,second,budget);pinch=edges[first].start;budget.charge(edges.size());
        std::vector<ContourEdge> inner(edges.begin()+std::ptrdiff_t(first),edges.begin()+std::ptrdiff_t(second));
        std::vector<ContourEdge> outer(edges.begin()+std::ptrdiff_t(second),edges.end());outer.insert(outer.end(),edges.begin(),edges.begin()+std::ptrdiff_t(first));
        append(std::move(inner));append(std::move(outer));
        if((loops[0].area>0)==(loops[1].area>0))throw std::invalid_argument("Persistent face pinch cycles lack opposite orientation");};
    add(g.outer_loop);for(auto h:g.holes){
        if(auto mixed=mixed_hole(g,h,budget)){const auto boundary=loops.size();append(std::move(mixed->boundary));
            for(auto& tree:mixed->trees){Loop rod;rod.open=rod.linear_tree=true;rod.root=tree.root;rod.boundary=boundary;rod.edges=std::move(tree.edges);
                for(const auto& e:rod.edges){budget.charge();rod.hulls.push_back(hull(e));}rods.push_back(std::move(rod));}continue;}
        auto proof=internal_hole(g,h,budget);
        if(!proof){add(h);continue;}Loop rod;rod.open=true;rod.linear_tree=proof->linear_tree;rod.edges=std::move(proof->edges);
        for(const auto& e:rod.edges){budget.charge();rod.hulls.push_back(hull(e));}rods.push_back(std::move(rod));}
    const auto boundary_count=loops.size();for(auto& rod:rods)loops.push_back(std::move(rod));
    // Noncollinear quadratic interiors lie strictly inside their control
    // triangles. Weak SAT separation proves they can meet only at actual
    // endpoints. Degenerate/linear spans are monotone segments and need
    // explicit endpoint-on-segment and positive-overlap checks instead.
    for(std::size_t u=0;u<loops.size();++u)for(std::size_t v=u;v<loops.size();++v){const auto& a=loops[u];const auto& b=loops[v];
        for(std::size_t i=0;i<a.edges.size();++i)for(std::size_t j=u==v?i+1:0;j<b.edges.size();++j){budget.charge();const auto& x=a.edges[i];const auto& y=b.edges[j];const auto& hx=a.hulls[i];const auto& hy=b.hulls[j];
            const bool pinch_pair=pinch&&u!=v&&(x.start==*pinch||x.end==*pinch)&&(y.start==*pinch||y.end==*pinch);
            const bool adjacent=u==v&&(a.open?i+1==j:((i+1)%a.edges.size()==j||(j+1)%a.edges.size()==i));
            const auto joint=[&](P p){
                const auto attached=[&](const Loop& tree,std::size_t boundary){if(!tree.root||tree.boundary!=boundary)return false;
                    const auto root=*tree.root;return (x.start==root||x.end==root)&&(y.start==root||y.end==root)&&p==g.vertices[root];};
                if(u!=v&&(attached(a,v)||attached(b,u)))return true;
                if(u==v&&a.linear_tree)return ((x.start==y.start||x.start==y.end)&&p==x.a)||((x.end==y.start||x.end==y.end)&&p==x.b);
                return (pinch_pair&&p==g.vertices[*pinch])||(adjacent&&((x.end==y.start&&p==x.b)||(y.end==x.start&&p==y.b)));};
            for(auto p:{x.a,x.b})for(auto q:{y.a,y.b})if(p==q&&!joint(p))throw std::invalid_argument("Illegal persistent face endpoint contact");
            if(hy.count==2)for(auto p:{x.a,x.b})if(on(y.a,y.b,p)&&!joint(p))throw std::invalid_argument("Face endpoint touches another segment");
            if(hx.count==2)for(auto p:{y.a,y.b})if(on(x.a,x.b,p)&&!joint(p))throw std::invalid_argument("Face endpoint touches another segment");
            if(hx.count==2&&hy.count==2){if(!intersects(x.a,x.b,y.a,y.b))continue;
                if(!cross(x.a,x.b,y.a)&&!cross(x.a,x.b,y.b)){const bool horizontal=x.a.x!=x.b.x;const auto lane=[&](P p){return horizontal?p.x:p.y;};
                    if(std::max(std::min(lane(x.a),lane(x.b)),std::min(lane(y.a),lane(y.b)))<std::min(std::max(lane(x.a),lane(x.b)),std::max(lane(y.a),lane(y.b))))throw std::invalid_argument("Overlapping persistent face segments");}
                if(!joint(x.a)&&!joint(x.b))throw std::invalid_argument("Intersecting persistent face segments");
            }else if(a.open||b.open){
                std::optional<P> allowed;if(adjacent)allowed=x.b;
                else for(auto p:{x.a,x.b})if(joint(p))allowed=p;
                if(!adaptive_strictly_separated(x,y,budget,allowed))throw std::invalid_argument("Internal face hole curves are not certified disjoint");
            }else if(!separated(hx,hy,budget)){
                // Original nonadjacent arcs may have overlapping coarse hulls.
                // Exact bounded refinement certifies only positive separation:
                // a new split endpoint is a curve interior, never a legal joint.
                if(adjacent||!adaptive_strictly_separated(x,y,budget,pinch_pair?std::optional{g.vertices[*pinch]}:std::nullopt))throw std::invalid_argument("Persistent face curve hulls are not certified disjoint");
            }
        }}
    // Arc-to-chord homotopies remain in the closed control hull. At a true
    // endpoint outside every queried hull, chord odd-even equals continuous
    // curve containment. Ambiguous hull contact is conservatively rejected;
    // this does not substitute the native sampled/original-span draw route.
    const auto contained=[&](const Loop& query,const Loop& boundary){for(const auto& e:query.edges){bool safe=true;for(const auto& h:boundary.hulls){budget.charge();if(hull_contains(h,e.a)){safe=false;break;}}
            if(safe){budget.charge(boundary.chords.size());return inside(boundary.chords,e.a);}}
        // Thin nested curves can cover all query endpoints with their coarse
        // hulls. Refine only this ambiguous containment proof, retaining exact
        // closed-hull exclusion at every accepted child chord.
        for(const auto& e:query.edges){bool odd=false,safe=true;const auto p=exact_point(e.a);
            for(const auto& edge:boundary.edges){const auto parity=certified_chord_parity(exact_curve(edge),p,budget);
                if(!parity){safe=false;break;}odd=odd!=*parity;}
            if(safe)return odd;}
        throw std::invalid_argument("Face containment has no certified hull-exterior endpoint");};
    if(pinch){const bool first_inside=contained(loops[0],loops[1]),second_inside=contained(loops[1],loops[0]);
        if(first_inside==second_inside)throw std::invalid_argument("Persistent face pinch is not a contained outer-hole pair");return;}
    for(std::size_t i=1;i<boundary_count;++i){if(!contained(loops[i],loops[0]))throw std::invalid_argument("Face hole is outside outer boundary");
        for(std::size_t j=1;j<i;++j)if(contained(loops[i],loops[j])||contained(loops[j],loops[i]))throw std::invalid_argument("Nested face holes are unsupported");}
    for(std::size_t i=boundary_count;i<loops.size();++i){const auto& rod=loops[i];
        const auto endpoint=[&](P p){budget.charge();Loop query;query.edges.push_back({p,p,p,0,0,false});
            if(!contained(query,loops[0]))throw std::invalid_argument("Internal face hole is outside outer boundary");
            for(std::size_t j=1;j<boundary_count;++j)if(contained(query,loops[j]))throw std::invalid_argument("Internal face hole is inside a real hole");};
        if(rod.root){
            // Each root can have multiple outgoing arms. Check every nonroot
            // endpoint: an arm entering the hole is illegal even if another
            // arm in the same component has an exterior leaf. Pair separation
            // already excludes all boundary intersections except this root.
            for(const auto& edge:rod.edges){if(edge.start!=*rod.root)endpoint(edge.a);if(edge.end!=*rod.root)endpoint(edge.b);}
        }else{
            // A separated connected internal tree/spine needs one endpoint.
            endpoint(rod.edges.front().a);endpoint(rod.edges.back().b);
        }
    }
}
VectorFaceGeometry lower(const Graph& graph,int region_id,std::uint64_t color,Certification& budget,VectorShapeFaceGeometry* cached=nullptr,bool certified=true){
    const auto& region=*graph.regions.at(region_id);VectorFaceGeometry g;g.color=color;if(cached)cached->region_bounds=region.outer.archived_bounds;
    //Native5987E0 gates a Face by the saved embedded outer bbox before
    //routing any holes. Factory/recomputed empty outers are native no-ops;
    //positive stale gates on an empty member chain need a separate cached
    //render contract and cannot be inferred from5A1CF0's infinite predicate.
    if(region.outer.members.empty()&&region.outer.archived_bounds.left<region.outer.archived_bounds.right&&region.outer.archived_bounds.top<region.outer.archived_bounds.bottom)
        throw std::invalid_argument("Empty persistent outer has a positive stale drawing gate");
    std::map<LoopId,std::uint32_t> loops;std::map<int,std::uint32_t> models,vertices;std::vector<LoopId> pending;
    const auto add_loop=[&](LoopId id){const auto [entry,inserted]=loops.emplace(id,std::uint32_t(loops.size()));if(inserted){if(loops.size()>count_limit)throw std::length_error("Expanded paint loop limit exceeded");pending.push_back(id);g.loops.emplace_back();if(cached)cached->loop_bounds.push_back(graph.all_loops.at(id)->archived_bounds);}return entry->second;};
    g.outer_loop=add_loop({true,region_id});for(auto hole:region.holes)g.holes.push_back(add_loop({false,hole}));
    std::size_t spans{},members{};
    const auto add_vertex=[&](int id){live(graph.vertices,id);const auto [i,inserted]=vertices.emplace(id,std::uint32_t(vertices.size()));if(inserted){const auto& v=*graph.vertices.at(id);
        // Bound exact polygon predicates and native original-span differences.
        if(std::abs(std::int64_t(v.x))>134217727||std::abs(std::int64_t(v.y))>134217727)throw std::invalid_argument("Persistent face coordinate outside certified domain");
        if(vertices.size()>count_limit)throw std::length_error("Expanded paint vertex limit exceeded");budget.expand(budget.vertices,1);g.vertices.push_back({v.x,v.y});}return i->second;};
    for(std::size_t li=0;li<pending.size();++li){budget.charge();const auto id=pending[li];const auto& loop=*graph.all_loops.at(id);
        if(loop.members.size()>count_limit-members)throw std::length_error("Expanded paint member limit exceeded");members+=loop.members.size();budget.expand(budget.members,loop.members.size());
        std::vector<VectorFaceMember> lowered;for(auto member:loop.members){budget.charge();auto i=models.find(member.model);
            if(i==models.end()){const auto& raw=*graph.models.at(member.model);
                //59E950 containment and59F640 Face emission read only bit0.
                // Width/endpoint/marked state is not a Face geometry class.
                // Preserve every raw field; normalize the transient renderer
                // packet to its existing fixed linear/quadratic contract.
                const auto flags=(raw.flags&1u)|2u;
                if(raw.subcurves.empty())throw std::invalid_argument("Empty persistent face model");
                if(raw.subcurves.size()>count_limit-spans)throw std::length_error("Expanded paint span limit exceeded");spans+=raw.subcurves.size();budget.expand(budget.spans,raw.subcurves.size());
                // Native Face traversal consumes only linear bit0, never
                // Line width nodes or saved cap descriptors. Keep the core
                // Face class contract fixed while retaining source flags.
                VectorFaceModel m;m.flags=flags;
                for(int side=0;side<2;++side){const auto t=target(raw,side);const auto dense=t?int(add_loop(*t)):-1;if(side)m.inside_loop=dense;else m.outside_loop=dense;}
                for(auto sub_id:raw.subcurves){budget.charge();const auto& sub=*graph.subcurves.at(sub_id);const auto a=add_vertex(sub.vertex_a),b=add_vertex(sub.vertex_b);
                    if(!(raw.flags&1)&&(std::abs(std::int64_t(sub.control_x))>134217727||std::abs(std::int64_t(sub.control_y))>134217727))
                        throw std::invalid_argument("Persistent face control outside bounded raw domain");
                    if(!m.spans.empty()&&m.spans.back().end!=a)throw std::invalid_argument("Disconnected persistent face model");
                    // The saved control is inactive under the linear vtable.
                    // Keep its raw SC words; only the drawing/query carrier
                    // uses the endpoint so consumed-coordinate admission does
                    // not invent a bound on a field the native path ignores.
                    m.spans.push_back({a,b,(raw.flags&1)?g.vertices[a].x:sub.control_x,(raw.flags&1)?g.vertices[a].y:sub.control_y});}
                if(models.size()>=count_limit)throw std::length_error("Expanded paint model limit exceeded");const auto dense=std::uint32_t(g.models.size());g.models.push_back(std::move(m));if(cached)cached->model_bounds.push_back({raw.scalars[0],raw.scalars[1],raw.scalars[2],raw.scalars[3]});i=models.emplace(member.model,dense).first;}
            lowered.push_back({i->second,member.selector});}
        g.loops.at(loops.at(id)).members=std::move(lowered);
    }
    // Reciprocal source lanes were already validated; every dense closure loop
    // must be identity-closed before passing the runtime renderer's contract.
    for(const auto& [id,loop]:loops){budget.charge();if(id.embedded&&g.loops[loop].members.empty())continue;(void)contour(g,loop,false,certified?2:1);}
    //5987E0 ->5A2150 ->59D350 consumes the original walks, including
    // internal Models and nonsimple contours. Keep the separate public
    // simple-contour certificate for clients that require that guarantee.
    if(certified)certify(g,budget);return g;
}
} // namespace

VectorArchiveGradNode decode_vector_gradnode(const Bytes& bytes){
    if(bytes.size()>byte_limit)throw std::length_error("Vector gradation leaf exceeds64MiB");Reader r(bytes);VectorArchiveGradNode n;
    for(unsigned shift:{0u,16u,32u,48u})n.color|=std::uint64_t(r.u16())<<shift;n.position_bits=r.u32();n.suffix=r.rest();return n;
}
Bytes encode_vector_gradnode(const VectorArchiveGradNode& n){
    if(n.suffix.size()>byte_limit-12)throw std::length_error("Vector gradation leaf exceeds64MiB");Writer w;
    for(unsigned shift:{0u,16u,32u,48u})w.u16(std::uint16_t(n.color>>shift));w.u32(n.position_bits);w.raw(n.suffix);return std::move(w.data);
}
VectorArchiveGradation decode_vector_gradation(const Bytes& bytes){
    if(bytes.size()>byte_limit)throw std::length_error("Vector gradation leaf exceeds64MiB");Reader r(bytes);VectorArchiveGradation g;g.flags=r.u32();
    g.stops=refs(r,24);g.faces=refs(r,20);g.start_x=r.i32();g.start_y=r.i32();g.end_x=r.i32();g.end_y=r.i32();g.minor_ratio_bits=r.u32();g.suffix=r.rest();return g;
}
Bytes encode_vector_gradation(const VectorArchiveGradation& g){
    if(g.stops.size()>count_limit||g.faces.size()>count_limit)throw std::length_error("Vector gradation leaf reference limit exceeded");
    const auto known=32+4*g.stops.size()+4*g.faces.size();if(g.suffix.size()>byte_limit-known)throw std::length_error("Vector gradation leaf exceeds64MiB");
    Writer w;w.u32(g.flags);write_refs(w,g.stops);write_refs(w,g.faces);w.i32(g.start_x);w.i32(g.start_y);w.i32(g.end_x);w.i32(g.end_y);w.u32(g.minor_ratio_bits);w.raw(g.suffix);return std::move(w.data);
}
VectorArchiveTexture decode_vector_texture(const Bytes& bytes){
    if(bytes.size()>byte_limit)throw std::length_error("Vector texture leaf exceeds64MiB");
    Reader r(bytes);const auto size=r.u32();r.take(size);VectorArchiveTexture v;v.offscreen.assign(bytes.begin(),bytes.begin()+std::size_t(size)+4);
    v.metadata=r.string();v.flags=r.u32();v.faces=refs(r,24);v.origin_x=r.i32();v.origin_y=r.i32();v.scale_x_bits=r.u32();v.scale_y_bits=r.u32();v.rotation_bits=r.u32();v.shear_x_bits=r.u32();v.suffix=r.rest();return v;
}
Bytes encode_vector_texture(const VectorArchiveTexture& v){
    if(v.faces.size()>count_limit||v.metadata.size()>65535)throw std::length_error("Vector texture leaf reference/string limit exceeded");
    if(v.offscreen.size()>byte_limit||v.suffix.size()>byte_limit)throw std::length_error("Vector texture leaf exceeds64MiB");
    const auto known=std::uint64_t(v.offscreen.size())+34+v.metadata.size()+4*v.faces.size()+v.suffix.size();
    if(known>byte_limit)throw std::length_error("Vector texture leaf exceeds64MiB");
    Reader image(v.offscreen);(void)image.section();if(image.remaining())bad("Texture offscreen must be one complete framed payload");
    Writer w;w.raw(v.offscreen);w.string(v.metadata);w.u32(v.flags);write_refs(w,v.faces);w.i32(v.origin_x);w.i32(v.origin_y);w.u32(v.scale_x_bits);w.u32(v.scale_y_bits);w.u32(v.rotation_bits);w.u32(v.shear_x_bits);w.raw(v.suffix);return std::move(w.data);
}
VectorPaintArchive decode_vector_paint_archive(const Layer& layer){
    if(layer.opaque||(layer.family!=0x21200000&&layer.family!=0x20800000))throw std::invalid_argument("Unsupported vector paint owner");
    if(layer.specific.size()>byte_limit)throw std::length_error("Vector paint archive exceeds64MiB");VectorPaintArchive a;Reader outer(layer.specific);auto core=outer.section();
    auto bh=core.section();markers(bh,a.base.high_water);a.base.header_suffix=bh.rest();auto fh=core.section();markers(fh,a.face_high_water);a.face_header_suffix=fh.rest();
    auto bb=core.section();a.base.vertices=read_pool<VectorVertex>(bb.section(),a.base.high_water[0],[](Reader& r){VectorVertex v;v.x=r.i32();v.y=r.i32();v.subcurves=refs(r);return v;});
    a.base.scales=read_pool<VectorLineScale>(bb.section(),a.base.high_water[1],[](Reader& r){return VectorLineScale{r.u32(),r.u32()};});
    a.base.subcurves=read_pool<VectorSubcurve>(bb.section(),a.base.high_water[2],[](Reader& r){VectorSubcurve v;v.vertex_a=r.i32();v.vertex_b=r.i32();v.control_x=r.i32();v.control_y=r.i32();v.model=r.i32();v.scales=refs(r);return v;});
    a.base.models=read_pool<VectorModelCurve>(bb.section(),a.base.high_water[3],[](Reader& r){VectorModelCurve v;v.subcurves=refs(r,64);v.flags=r.u32();v.line=r.i32();for(std::size_t i=0;i<2;++i){v.target_kinds[i]=r.i32();v.targets[i]=r.i32();}for(auto& s:v.scalars)s=r.i32();for(auto& c:v.crosspoints)c=r.i32();return v;});
    a.base.crosspoints=read_pool<VectorCrosspoint>(bb.section(),a.base.high_water[4],[](Reader& r){VectorCrosspoint v;for(auto& w:v.words)w=r.u32();return v;});
    a.base.lines=read_pool<VectorLine>(bb.section(),a.base.high_water[5],[](Reader& r){VectorLine v;v.members=pairs(r,28);v.half_width_bits=r.u32();v.color=r.u64();v.archived_bounds=read_bounds(r);return v;});a.base.body_suffix=bb.rest();
    auto fb=core.section();a.loops=read_pool<VectorArchiveLoop>(fb.section(),a.face_high_water[0],[](Reader& r){return read_loop(r);});
    a.regions=read_pool<VectorArchiveRegion>(fb.section(),a.face_high_water[1],[](Reader& r){VectorArchiveRegion v;auto outer=r.section();v.outer=read_loop(outer);v.outer.suffix=outer.rest();v.holes=refs(r);v.faces=refs(r);return v;});
    a.faces=read_pool<VectorArchiveFace>(fb.section(),a.face_high_water[2],[](Reader& r){VectorArchiveFace v;v.region=r.i32();v.flags=r.u32();if((v.flags&15)==1)v.color=r.u64();else if((v.flags&15)==2||(v.flags&15)==4)v.material=r.i32();return v;});
    const auto opaque=[](Reader& r){return r.rest();};a.gradnodes=read_pool<Bytes>(fb.section(),a.face_high_water[3],opaque);a.gradations=read_pool<Bytes>(fb.section(),a.face_high_water[4],opaque);a.textures=read_pool<Bytes>(fb.section(),a.face_high_water[5],opaque);a.face_body_suffix=fb.rest();a.core_suffix=core.rest();
    auto border=outer.section();auto common=border.section();a.border.common=read_common(common);auto specific=border.section();auto topology=specific.section();a.border.vertices=refs(topology);a.border.models=refs(topology);a.border.topology_suffix=topology.rest();a.border.regions=refs(specific);a.border.specific_suffix=specific.rest();a.border.record_suffix=border.rest();a.outer_suffix=outer.rest();const auto graph=validate(a);
    (void)vector_runtime_detail::face_colors_storage(layer);
    if(layer.runtime_vector_face_colors)for(const auto& [id,color]:*layer.runtime_vector_face_colors){
        const auto i=graph.faces.find(id);if(i==graph.faces.end()||(i->second->flags&15)==1)bad("Stale runtime Face color identity");
        const_cast<VectorArchiveFace*>(i->second)->color=color;
    }
    return a;
}
Bytes encode_vector_paint_archive(const VectorPaintArchive& a){
    wire_budget(a);(void)validate(a);if(!a.base.layer_suffix.empty())bad("Unexpected base-only suffix in face archive");Writer bh,fh,bb,fb,core,outer;markers(bh,a.base.high_water);bh.raw(a.base.header_suffix);markers(fh,a.face_high_water);fh.raw(a.face_header_suffix);
    write_pool(bb,a.base.vertices,a.base.high_water[0],[](Writer& w,const VectorVertex& v){w.i32(v.x);w.i32(v.y);write_refs(w,v.subcurves);});
    write_pool(bb,a.base.scales,a.base.high_water[1],[](Writer& w,const VectorLineScale& v){w.u32(v.scale_bits);w.u32(v.parameter_bits);});
    write_pool(bb,a.base.subcurves,a.base.high_water[2],[](Writer& w,const VectorSubcurve& v){w.i32(v.vertex_a);w.i32(v.vertex_b);w.i32(v.control_x);w.i32(v.control_y);w.i32(v.model);write_refs(w,v.scales);});
    write_pool(bb,a.base.models,a.base.high_water[3],[](Writer& w,const VectorModelCurve& v){write_refs(w,v.subcurves);w.u32(v.flags);w.i32(v.line);for(std::size_t i=0;i<2;++i){w.i32(v.target_kinds[i]);w.i32(v.targets[i]);}for(auto s:v.scalars)w.i32(s);for(auto c:v.crosspoints)w.i32(c);});
    write_pool(bb,a.base.crosspoints,a.base.high_water[4],[](Writer& w,const VectorCrosspoint& v){for(auto word:v.words)w.u32(word);});
    write_pool(bb,a.base.lines,a.base.high_water[5],[](Writer& w,const VectorLine& v){write_pairs(w,v.members);w.u32(v.half_width_bits);w.u64(v.color);write_bounds(w,v.archived_bounds);});bb.raw(a.base.body_suffix);
    write_pool(fb,a.loops,a.face_high_water[0],[](Writer& w,const VectorArchiveLoop& v){write_loop(w,v);});
    write_pool(fb,a.regions,a.face_high_water[1],[](Writer& w,const VectorArchiveRegion& v){Writer loop;write_loop(loop,v.outer);w.section(loop.data);write_refs(w,v.holes);write_refs(w,v.faces);});
    write_pool(fb,a.faces,a.face_high_water[2],[](Writer& w,const VectorArchiveFace& v){w.i32(v.region);w.u32(v.flags);if((v.flags&15)==1)w.u64(v.color);else if((v.flags&15)==2||(v.flags&15)==4)w.i32(v.material);});
    const auto raw=[](Writer& w,const Bytes& v){w.raw(v);};write_pool(fb,a.gradnodes,a.face_high_water[3],raw);write_pool(fb,a.gradations,a.face_high_water[4],raw);write_pool(fb,a.textures,a.face_high_water[5],raw);fb.raw(a.face_body_suffix);
    core.section(bh.data);core.section(fh.data);core.section(bb.data);core.section(fb.data);core.raw(a.core_suffix);outer.section(core.data);
    Writer border,common,specific,topology;write_common(common,a.border.common);write_refs(topology,a.border.vertices);write_refs(topology,a.border.models);topology.raw(a.border.topology_suffix);specific.section(topology.data);write_refs(specific,a.border.regions);specific.raw(a.border.specific_suffix);border.section(common.data);border.section(specific.data);border.raw(a.border.record_suffix);outer.section(border.data);outer.raw(a.outer_suffix);
    if(outer.data.size()>byte_limit)throw std::length_error("Vector paint archive exceeds64MiB");return std::move(outer.data);
}
void vector_archive_detail::publish_paint_storage(Layer& layer,const VectorPaintArchive& archive,Bytes wire){
    if(archive.faces.size()>count_limit)throw std::length_error("Runtime Face color count exceeds100000");
    std::shared_ptr<const std::map<std::int32_t,std::uint64_t>> retained;
    std::map<std::int32_t,std::uint64_t> colors;
    for(const auto& face:archive.faces)if((face.value.flags&15)!=1&&face.value.color)colors.emplace(face.index,face.value.color);
    if(!colors.empty())retained=std::make_shared<const std::map<std::int32_t,std::uint64_t>>(std::move(colors));
    layer.specific=std::move(wire);layer.runtime_vector_face_colors=std::move(retained);
}
void store_vector_paint_archive(Layer& layer,const VectorPaintArchive& archive){
    vector_archive_detail::publish_paint_storage(layer,archive,encode_vector_paint_archive(archive));
}
namespace {
std::vector<VectorPaintPlaneGeometry> project_planes(const VectorPaintArchive& a,const Layer& layer,std::optional<std::size_t> selected,std::uint64_t reserved,
    std::map<int,std::shared_ptr<const VectorShapeTextureRecipe>>* recipes=nullptr,bool certified=true){
    if(layer.opaque||(layer.family!=0x21200000&&layer.family!=0x20800000)||layer.planes.size()>100000)throw std::invalid_argument("Unsupported vector paint owner");
    wire_budget(a);const auto graph=validate(a);plane_ownership(a,layer,graph);
    auto texture_preparation=prepare_textures(a,layer,graph,reserved+2*512ull*512*sizeof(Pixel));
    const auto gradient=gradients(a,layer,graph);
    std::set<int> border(a.border.regions.begin(),a.border.regions.end());std::vector<VectorPaintPlaneGeometry> out(layer.planes.size());Certification budget;
    for(std::size_t plane=0;plane<layer.planes.size();++plane){
        const auto& p=layer.planes[plane];if(p.family!=0x22100001&&p.family!=0x22100002)continue;const auto lists=paint_refs(p);
        for(auto id:lists.faces){const auto& face=*graph.faces.at(id);const auto kind=face.flags&15;
            if(selected&&*selected!=plane&&kind!=4)continue;
            if(kind!=1&&kind!=2&&kind!=4)throw std::invalid_argument("Unsupported persistent face material");
            if(face.region<0||!border.contains(face.region))throw std::invalid_argument("Paint face lacks a border-owned region");auto g=lower(graph,face.region,face.color,budget,nullptr,certified);g.material_flags=face.flags;
            if(kind==2)g.gradation=gradient.materials.at(face.material);
            out[plane].faces.push_back({id,face.region,std::move(g)});}
    }
    if(recipes){
        for(const auto& [id,v]:texture_preparation){auto recipe=std::make_shared<VectorShapeTextureRecipe>();recipe->flags=v.material.flags;
            recipe->origin_x=v.material.origin_x;recipe->origin_y=v.material.origin_y;recipe->scale_x_bits=v.material.scale_x_bits;recipe->scale_y_bits=v.material.scale_y_bits;
            recipe->rotation_bits=v.material.rotation_bits;recipe->shear_x_bits=v.material.shear_x_bits;recipe->image_prefix_size=v.offscreen.size();
            recipes->emplace(id,std::move(recipe));}
        return out;
    }
    const auto texture=decode_textures(std::move(texture_preparation),layer,graph,out);
    for(auto& plane:out)for(auto& face:plane.faces)if((face.geometry.material_flags&15)==4)face.geometry.texture=texture.at(graph.faces.at(face.face_index)->material);
    return out;
}
}
VectorPaintPlaneGeometry project_vector_paint_plane(const VectorPaintArchive& a,const Layer& layer,std::size_t plane){
    if(plane>=layer.planes.size()||layer.planes[plane].opaque||(layer.planes[plane].family!=0x22100001&&layer.planes[plane].family!=0x22100002))throw std::invalid_argument("Unsupported vector paint plane");
    auto out=project_planes(a,layer,plane,0);return std::move(out[plane]);
}
std::vector<VectorPaintPlaneGeometry> project_vector_paint_planes(const VectorPaintArchive& a,const Layer& layer,std::uint64_t reserved){
    if(reserved>512ull*1024*1024-2*512ull*512*sizeof(Pixel))throw std::length_error("Paint owner reserved storage exceeds512MiB");
    return project_planes(a,layer,std::nullopt,reserved);
}
std::vector<VectorPaintPlaneGeometry> vector_archive_detail::project_native_paint_faces(const VectorPaintArchive& a,const Layer& layer,std::uint64_t reserved){
    if(reserved>512ull*1024*1024-2*512ull*512*sizeof(Pixel))throw std::length_error("Paint owner reserved storage exceeds512MiB");
    return project_planes(a,layer,std::nullopt,reserved,nullptr,false);
}
std::vector<VectorPaintPlaneGeometry> vector_render_detail::project_stored_paint_faces(VectorPaintArchive& a,const Layer& layer,
    std::map<int,std::shared_ptr<const VectorShapeTextureRecipe>>& recipes,std::uint64_t reserved){
    if(!recipes.empty())throw std::invalid_argument("Paint texture recipes must start empty");
    auto out=project_planes(a,layer,std::nullopt,reserved,&recipes,false);
    // Keep complete active texture/Face/plane inverse validation, but transfer
    // each raw leaf without image expansion. Joint Line/Face admission and
    // executor accounting finish before the first real sampler snapshot.
    for(auto& r:a.textures)if(const auto it=recipes.find(r.index);it!=recipes.end()){
        auto recipe=std::const_pointer_cast<VectorShapeTextureRecipe>(it->second);recipe->leaf=std::make_shared<const Bytes>(std::move(r.value));}
    return out;
}
std::vector<vector_archive_detail::PaintRegionProjection> vector_archive_detail::project_paint_regions(const VectorPaintArchive& a,const Layer& layer){
    wire_budget(a);const auto graph=validate(a);plane_ownership(a,layer,graph);Certification budget;
    std::vector<PaintRegionProjection> out;out.reserve(a.border.regions.size());
    for(auto id:a.border.regions){VectorShapeFaceGeometry cached;cached.geometry=lower(graph,id,0,budget,&cached,false);out.push_back({id,std::move(cached)});}
    return out;
}
std::vector<vector_archive_detail::ShapeFaceProjection> vector_archive_detail::project_shape_faces(VectorPaintArchive& a,std::span<const std::int32_t> selected,std::uint64_t reserved){
    wire_budget(a);const auto graph=validate(a);Certification budget;
    if(selected.size()>count_limit)throw std::length_error("Shape selected Face limit exceeded");
    constexpr std::uint64_t limit=512ull*1024*1024;std::uint64_t storage=reserved;
    const auto charge=[&](std::uint64_t n){if(storage>limit||n>limit-storage)throw std::length_error("Shape projection storage exceeds512MiB");storage+=n;};
    std::set<int> needed,needed_textures;for(auto id:selected){live(graph.faces,id);const auto& face=*graph.faces.at(id);const auto kind=face.flags&15;
        if((kind!=1&&kind!=2&&kind!=4)||face.region<0)throw std::invalid_argument("Shape projection requires a supported Face with live Region");
        if(kind==2){if(face.material<0)throw std::invalid_argument("NULL Shape Face gradation");needed.insert(face.material);}
        if(kind==4){if(face.material<0)throw std::invalid_argument("NULL Shape Face texture");needed_textures.insert(face.material);}}
    const auto raw=index(a.gradations),nodes=index(a.gradnodes);std::map<int,int> node_owner;std::map<int,std::shared_ptr<const VectorGradationMaterial>> materials;
    std::size_t global_nodes{},selected_nodes{},reverse_refs{};
    if(!needed.empty()){
        // Native633830 loads all Gradation chains;59AE20 rewrites intrusive
        // node links even for an unpainted G. Scan every stop-ID prefix before
        // expanding selected leaves, leaving inactive colors/scalars/tails raw.
        for(const auto& [id,bytes]:raw){Reader r(*bytes);r.u32();const auto n=r.i32();r.require_count(n,4,count_limit);
            if(std::size_t(n)>count_limit-global_nodes)throw std::length_error("Shape global gradient stop-reference limit exceeded");global_nodes+=std::size_t(n);}
        charge(global_nodes*80+needed.size()*(sizeof(VectorGradationMaterial)+128));
        for(const auto& [id,bytes]:raw){Reader r(*bytes);r.u32();const auto n=r.i32();for(int i=0;i<n;++i){const auto node=r.i32();live(nodes,node);if(!node_owner.emplace(node,id).second)bad("Shared intrusive Shape gradient stop node");}}
        for(auto id:needed){Reader r(*raw.at(id));r.u32();const auto n=r.i32();r.require_count(n,4,count_limit);
            if(r.remaining()<24||std::size_t(n)>(r.remaining()-24)/4||std::size_t(n)>count_limit-selected_nodes)throw std::length_error("Shape selected gradient stop limit exceeded");selected_nodes+=std::size_t(n);r.take(std::size_t(n)*4);
            const auto f=r.i32();r.require_count(f,4,count_limit);if(r.remaining()<20||std::size_t(f)>(r.remaining()-20)/4||std::size_t(f)>count_limit-reverse_refs)throw std::length_error("Shape selected gradient reverse-reference limit exceeded");reverse_refs+=std::size_t(f);
        }
        charge(2*selected_nodes*sizeof(VectorGradationStop)+2*(selected_nodes+reverse_refs)*sizeof(int));
        std::set<int> reciprocal;
        for(auto id:needed){const auto leaf=decode_vector_gradation(*raw.at(id));auto m=std::make_shared<VectorGradationMaterial>();m->flags=leaf.flags;m->start_x=leaf.start_x;m->start_y=leaf.start_y;m->end_x=leaf.end_x;m->end_y=leaf.end_y;m->minor_ratio_bits=leaf.minor_ratio_bits;
            m->stops.reserve(leaf.stops.size());for(auto node:leaf.stops){const auto value=decode_vector_gradnode(*nodes.at(node));m->stops.push_back({value.color,value.position_bits});}
            // Modern strict inverse consistency, not a native draw-time gate.
            // These are ordinary pointer nodes: NULL and same-owner repeats
            // survive; neither the Face nor G needs same-plane ownership.
            for(auto face:leaf.faces){live(graph.faces,face,true);if(face<0)continue;const auto& f=*graph.faces.at(face);if((f.flags&15)!=2||f.material!=id)bad("Shape gradient reverse Face disagrees with material");reciprocal.insert(face);}
            materials.emplace(id,std::move(m));
        }
        for(auto id:selected){const auto& face=*graph.faces.at(id);if((face.flags&15)==2&&!reciprocal.contains(id))bad("Shape gradient Face lacks reciprocal material reference");}
    }
    std::map<int,std::shared_ptr<VectorShapeTextureRecipe>> texture_recipes;
    const auto texture_raw=index(a.textures);std::set<int> reciprocal_textures;std::size_t texture_refs{};
    charge(needed_textures.size()*(sizeof(VectorShapeTextureRecipe)+128));
    for(auto id:needed_textures){const auto v=texture_view(*texture_raw.at(id));const auto n=v.faces.size()/4;
        if(n>count_limit-texture_refs)throw std::length_error("Shape texture reverse-reference limit exceeded");texture_refs+=n;charge(n*64);
        if((v.material.flags&15)!=1&&(v.material.flags&15)!=2)throw std::invalid_argument("Unknown Shape texture cursor mode");
        (void)vector_texture_inverse_matrix(v.material);(void)vector_archive_detail::texture_image_storage(v.offscreen);
        Reader reverse(v.faces);while(reverse.remaining()){const auto face=reverse.i32();live(graph.faces,face,true);if(face<0)continue;const auto& f=*graph.faces.at(face);
            if((f.flags&15)!=4||f.material!=id)bad("Shape texture reverse Face disagrees with material");reciprocal_textures.insert(face);}
        auto recipe=std::make_shared<VectorShapeTextureRecipe>();recipe->flags=v.material.flags;recipe->origin_x=v.material.origin_x;recipe->origin_y=v.material.origin_y;
        recipe->scale_x_bits=v.material.scale_x_bits;recipe->scale_y_bits=v.material.scale_y_bits;recipe->rotation_bits=v.material.rotation_bits;recipe->shear_x_bits=v.material.shear_x_bits;
        recipe->image_prefix_size=v.offscreen.size();texture_recipes.emplace(id,std::move(recipe));
    }
    for(auto id:selected)if((graph.faces.at(id)->flags&15)==4&&!reciprocal_textures.contains(id))bad("Shape texture Face lacks reciprocal material reference");
    charge(selected.size()*sizeof(ShapeFaceProjection));std::vector<ShapeFaceProjection> out;out.reserve(selected.size());
    for(auto id:selected){const auto& face=*graph.faces.at(id);VectorShapeFaceGeometry cached;cached.geometry=lower(graph,face.region,face.color,budget,&cached,false);cached.geometry.material_flags=face.flags;
        if((face.flags&15)==2)cached.geometry.gradation=materials.at(face.material);
        if((face.flags&15)==4)cached.texture_recipe=texture_recipes.at(face.material);
        out.push_back({id,std::move(cached)});
    }
    // Transfer each selected raw leaf once after borrowed graph/view use ends.
    // No image bytes are copied or decompressed during Shape projection.
    for(auto& record:a.textures)if(const auto i=texture_recipes.find(record.index);i!=texture_recipes.end())i->second->leaf=std::make_shared<const Bytes>(std::move(record.value));
    return out;
}
std::size_t vector_archive_detail::paint_wire_size(const VectorPaintArchive& a){return wire_budget(a);}
}
