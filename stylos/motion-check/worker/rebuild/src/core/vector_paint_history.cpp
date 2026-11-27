#include "vector_paint_history.hpp"
#include "vector_graph_edit.hpp"
#include "retas/vector_extension_archive.hpp"
#include "vector_draw_history.hpp"
#include "vector_material_edit.hpp"
#include "vector_region_probe.hpp"
#include "retas/vector_color_metadata.hpp"
#include <algorithm>
#include <bit>
#include <map>
#include <set>
#include <cmath>
#include <tuple>
#include <zlib.h>

namespace retas::vector_paint_history_detail {
namespace {
constexpr std::uint64_t max_bytes=512ull*1024*1024,max_work=128ull*1024*1024;
enum Kind {Vertex,Scale,SC,MC,Cross,LC,Loop,Region,Face,GradNode,Grad,Texture};
bool line_plane(unsigned f){return f==0x21110001||f==0x21120001||f==0x21210001||f==0x21220001;}
bool paint_plane(unsigned f){return f==0x22100001||f==0x22100002;}
std::uint32_t material_flags(const Bytes& bytes,bool gradient){Reader r(bytes);if(!gradient){(void)r.section();(void)r.string();}return r.u32();}
bool positive(VectorBounds r){return r.left<r.right&&r.top<r.bottom;}
bool overlap(VectorBounds a,VectorBounds b){return a.left<b.right&&b.left<a.right&&a.top<b.bottom&&b.top<a.bottom;}
std::optional<VectorBounds> effective_window(std::optional<VectorBounds> r){return r&&positive(*r)?r:std::nullopt;}
int reference(int id){return id<0?-1:id;}
void nullable_refs(std::vector<int>& ids){for(int& id:ids)id=reference(id);}
template<class T>const T& get(const std::vector<VectorPoolRecord<T>>& pool,int id){const auto i=std::ranges::find(pool,id,&VectorPoolRecord<T>::index);if(i==pool.end())throw std::invalid_argument("Paint history has a dead typed reference");return i->value;}
template<class T>void remove_first(std::vector<T>& values,const T& value){const auto i=std::ranges::find(values,value);if(i!=values.end())values.erase(i);}
void append_owner(std::vector<int>& values,int value){
    if(std::ranges::find(values,value)!=values.end())throw std::invalid_argument("Paint history window leaves a restored intrusive owner active");
    values.push_back(value);
}
struct Child {
    std::uint64_t identity{};unsigned family{};std::size_t hint{};
    std::array<std::vector<int>,3> lists;
    Bytes tail;
    std::optional<vector_graph_detail::OwnerLists> topology;
};
std::vector<Child> children(const Layer& owner,Budget& budget){
    if(owner.planes.size()>1024)throw std::length_error("Paint history child limit");
    budget.allocate(owner.planes.size()*sizeof(Child));std::vector<Child> out;out.reserve(owner.planes.size());
    std::array<std::set<int>,6> owned;std::size_t references{};for(std::size_t i=0;i<owner.planes.size();++i){const auto& p=owner.planes[i];budget.visit();
        //44E280/44F3F0 skip non-vector companions. Retain a metadata-only
        //slot so vector ordinals still address the original owner children;
        //no raster/offscreen bytes enter this typed history packet.
        if((p.family&0xf0000000u)!=0x20000000u){out.push_back({p.common.runtime_identity,p.family,i,{},{},{}});continue;}
        if(p.opaque||p.raster||(!line_plane(p.family)&&!paint_plane(p.family)))throw std::invalid_argument("Paint history requires typed Paint/Pure children");
        budget.allocate(p.specific.size()*2);Child c{p.common.runtime_identity,p.family,i,{},{},{}};
        if(p.family==0x21210001u||p.family==0x21220001u){c.topology.emplace(p,nullptr);c.lists[0]=c.topology->lines;}else if(line_plane(p.family)){Reader r(p.specific);const auto n=r.i32();r.require_count(n,4,8192);for(int j=0;j<n;++j)c.lists[0].push_back(r.i32());c.tail=r.rest();}
        else{vector_material_detail::PaintLists p_lists(p);c.lists=std::move(p_lists.ids);c.tail=std::move(p_lists.tail);}
        for(unsigned lane=0;lane<3;++lane){const auto& list=c.lists[lane];references+=list.size();for(int id:list)if(id<0||!owned[line_plane(c.family)?0:lane+1].insert(id).second)throw std::invalid_argument("Paint history competing intrusive child ownership");}
        if(c.topology){references+=c.topology->vertices.size()+c.topology->models.size();for(int id:c.topology->vertices)if(id<0||!owned[4].insert(id).second)throw std::invalid_argument("Paint history competing Vert vertex owner");for(int id:c.topology->models)if(id<0||!owned[5].insert(id).second)throw std::invalid_argument("Paint history competing Vert model owner");}
        if(references>8192)throw std::length_error("Paint history child reference limit");out.push_back(std::move(c));
    }return out;
}
void publish_child(Plane& p,const Child& child){if((p.family&0xf0000000u)!=0x20000000u)return;if(child.topology){auto topology=*child.topology;topology.lines=child.lists[0];topology.publish(p,nullptr);return;}Writer w;for(int lane=0;lane<(line_plane(p.family)?1:3);++lane){w.i32(int(child.lists[lane].size()));for(int id:child.lists[lane])w.i32(id);}w.raw(child.tail);p.specific=std::move(w.data);}
std::uint64_t records(const VectorPaintArchive& a){return a.base.vertices.size()+a.base.scales.size()+a.base.subcurves.size()+a.base.models.size()+a.base.crosspoints.size()+a.base.lines.size()+a.loops.size()+a.regions.size()+a.faces.size()+a.gradnodes.size()+a.gradations.size()+a.textures.size();}
template<class T>std::uint64_t value_storage(const T& value){
    std::uint64_t size=sizeof(T);
    if constexpr(std::is_same_v<T,Bytes>)size+=value.size();
    if constexpr(std::is_same_v<T,VectorVertex>||std::is_same_v<T,VectorModelCurve>)size+=4*value.subcurves.size();
    if constexpr(std::is_same_v<T,VectorSubcurve>)size+=4*value.scales.size();
    if constexpr(std::is_same_v<T,VectorLine>||std::is_same_v<T,VectorArchiveLoop>)size+=sizeof(VectorLineMember)*value.members.size();
    if constexpr(std::is_same_v<T,VectorArchiveLoop>)size+=value.suffix.size();
    if constexpr(std::is_same_v<T,VectorArchiveRegion>)size+=value_storage(value.outer)+4*(value.holes.size()+value.faces.size());
    return size;
}
template<class T>std::uint64_t pool_storage(const std::vector<VectorPoolRecord<T>>& pool){std::uint64_t size{};for(const auto& r:pool)size+=sizeof(r)+value_storage(r.value)+r.suffix.size();return size;}
std::uint64_t archive_storage(const VectorPaintArchive& a){
    return sizeof(a)+pool_storage(a.base.vertices)+pool_storage(a.base.scales)+pool_storage(a.base.subcurves)+pool_storage(a.base.models)+pool_storage(a.base.crosspoints)+pool_storage(a.base.lines)
        +pool_storage(a.loops)+pool_storage(a.regions)+pool_storage(a.faces)+pool_storage(a.gradnodes)+pool_storage(a.gradations)+pool_storage(a.textures)
        +a.base.header_suffix.size()+a.base.body_suffix.size()+a.base.layer_suffix.size()+a.face_header_suffix.size()+a.face_body_suffix.size()+a.core_suffix.size()+a.outer_suffix.size()
        +4*(a.border.vertices.size()+a.border.models.size()+a.border.regions.size())+a.border.topology_suffix.size()+a.border.specific_suffix.size()+a.border.record_suffix.size()+a.border.common.name.size()+a.border.common.suffix.size();
}
VectorPaintArchive charged_state(VectorPaintArchive a,Budget& b){b.allocate(2*archive_storage(a));return a;}
using Shape=std::optional<VectorShapeArchive>;
Shape shape_envelope(const Layer& owner){if(owner.family!=0x40002000u)return {};auto value=decode_vector_shape_archive(owner);value.paint={};return value;}
void copy_shapes(Shape& destination,const Shape& source,Budget& b){
    if(!source)return;if(!destination)throw std::invalid_argument("Paint history Shape envelope mismatch");
    for(const auto& record:source->shapes){b.visit(destination->shapes.size()+1);auto i=std::ranges::find(destination->shapes,record.index,&VectorPoolRecord<VectorArchiveShape>::index);if(i==destination->shapes.end()){b.allocate(sizeof(record));destination->shapes.push_back({record.index,{},{}});i=destination->shapes.end()-1;destination->high_water=std::max(destination->high_water,record.index);}i->value=record.value;i->value.line=reference(i->value.line);i->value.region=reference(i->value.region);i->value.flags&=0xffffu;}std::ranges::sort(destination->shapes,{},&VectorPoolRecord<VectorArchiveShape>::index);
}
void store_content(Layer& owner,VectorPaintArchive paint,Shape shape={}){if(owner.family==0x40002000u){if(!shape)shape=shape_envelope(owner);shape->paint=std::move(paint);store_vector_shape_archive(owner,*shape);}else store_vector_paint_archive(owner,paint);}
VectorPaintArchive decode(const Layer& owner,Budget& budget){
    if(owner.opaque||(owner.family!=0x21200000&&owner.family!=0x20800000&&owner.family!=0x40002000u))throw std::invalid_argument("Paint history requires a decoded Paint212 owner");
    std::uint64_t wire=owner.specific.size();for(const auto& p:owner.planes)wire+=p.specific.size();
    if(wire>8ull*1024*1024)throw std::length_error("Paint history wire limit");budget.allocate(32*wire+8192+vector_runtime_detail::face_colors_storage(owner));budget.visit(16*wire+1);
    auto a=owner.family==0x40002000u?decode_vector_shape_archive(owner).paint:decode_vector_paint_archive(owner);const auto extension=shape_envelope(owner);if(records(a)+(extension?extension->shapes.size():0)>4096)throw std::length_error("Paint history record limit");
    std::uint64_t refs=a.border.vertices.size()+a.border.models.size()+a.border.regions.size();
    for(const auto& r:a.base.vertices)refs+=r.value.subcurves.size();
    for(const auto& r:a.base.subcurves)refs+=r.value.scales.size()+3;
    for(const auto& r:a.base.models)refs+=r.value.subcurves.size()+5;
    for(const auto& r:a.base.lines)refs+=r.value.members.size();
    for(const auto& r:a.loops)refs+=r.value.members.size()+1;
    for(const auto& r:a.regions)refs+=r.value.outer.members.size()+r.value.holes.size()+r.value.faces.size();
    if(refs>8192)throw std::length_error("Paint history reference limit");
    return a;
}
//59DB20 and5A1DB0. Cached boxes and the original-span classifier have
//separate roles; mere Region bbox overlap does not select a window packet.
bool contained_window(const vector_probe_detail::PaintGraph& graph,const VectorArchiveLoop& loop,VectorBounds r,Budget& budget){
    const auto b=loop.archived_bounds;
    if(!(r.left>b.left&&r.top>b.top&&r.right<b.right&&r.bottom<b.bottom))return false;
    for(auto member:loop.members){budget.visit();const auto& m=*graph.lines.models.at(member.model);if(overlap(r,{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]}))return false;}
    const auto mid=[](int a,int b){return std::bit_cast<std::int32_t>(std::uint32_t(a)+std::uint32_t(b))/2;};
    return graph.classify(loop,{mid(r.left,r.right),mid(r.top,r.bottom)},{budget.work,budget.stop})==1;
}
bool region_matches(const vector_probe_detail::PaintGraph& graph,const VectorArchiveRegion& region,VectorBounds r,Budget& budget){
    budget.visit();if(positive(region.outer.archived_bounds)){if(!overlap(r,region.outer.archived_bounds))return false;if(!contained_window(graph,region.outer,r,budget))return true;}
    for(int id:region.holes){const auto& hole=*graph.loops.at(id);if(overlap(r,hole.archived_bounds)&&contained_window(graph,hole,r,budget))return false;}return true;
}
std::optional<VectorBounds> inflated(std::optional<VectorBounds> r){
    if(!r)return {};const auto edge=[](int n,int delta){const auto v=std::int64_t(n)+delta;if(v<INT32_MIN||v>INT32_MAX)throw std::overflow_error("Paint history window overflow");return int(v);};
    return VectorBounds{edge(r->left,-16),edge(r->top,-16),edge(r->right,16),edge(r->bottom,16)};
}
// Retired slots retain their known scalar fields until reused. In particular,
// a later LC/Loop may write a backlink through a currently inactive MC slot.
template<class T>using PoolMemory=vector_paint_history_detail::CachePoolMemory<T>;
template<class T>struct Pool {
    struct Cell {T value{};Bytes suffix;bool active{};};
    std::map<int,Cell> cells;
    vector_material_detail::VectorSlots slots;
    Budget& budget;
    bool touched{};
    Pool(const std::vector<VectorPoolRecord<T>>& source,std::optional<int> high,Budget& b):slots(source,b.work,b.stop,high),budget(b){
        b.allocate(source.size()*(sizeof(Cell)+128));for(const auto& r:source)cells.emplace(r.index,Cell{r.value,r.suffix,true});
    }
    bool active(int id)const{const auto i=cells.find(id);return i!=cells.end()&&i->second.active;}
    Cell& cell(int id){
        if(id<0)throw std::invalid_argument("Paint history negative slot reference");
        auto found=cells.find(id);if(found!=cells.end())return found->second;
        budget.allocate(sizeof(Cell)+128);Cell fresh;
        if constexpr(std::is_same_v<T,VectorModelCurve>){fresh.value.line=-1;fresh.value.targets={-1,-1};fresh.value.crosspoints={-1,-1};}
        if constexpr(std::is_same_v<T,VectorArchiveFace>){fresh.value.region=-1;fresh.value.material=-1;}
        return cells.emplace(id,std::move(fresh)).first->second;
    }
    T& value(int id){return cell(id).value;}
    bool activate(int id,Budget& b){
        b.visit();if(active(id))return false;auto& target=cell(id);
        //634A20 activates the requested identity, never a fresh free slot.
        slots.live.insert(id);if(id>slots.high)slots.high=id;if(id==slots.cursor)slots.cursor=slots.lowest(b.work,b.stop);
        target.active=true;touched=true;return true;
    }
    void retire(int id,Budget& b){b.visit();if(!active(id))return;slots.release(id);cells.at(id).active=false;touched=true;}
    std::vector<VectorPoolRecord<T>> output()const{std::vector<VectorPoolRecord<T>> out;for(const auto& [id,c]:cells)if(c.active)out.push_back({id,c.value,c.suffix});return out;}
    std::uint64_t storage()const{std::uint64_t size{};for(const auto& [id,c]:cells){(void)id;if(c.active)size+=sizeof(VectorPoolRecord<T>)+value_storage(c.value)+c.suffix.size();}return size;}
    PoolMemory<T> remember()const{
        PoolMemory<T> result;result.high=slots.high;result.cursor=slots.cursor;
        for(const auto& [id,c]:cells)if(!c.active){budget.visit();budget.allocate(sizeof(VectorPoolRecord<T>)+value_storage(c.value)+c.suffix.size());result.retired.push_back({id,c.value,c.suffix});}
        return result;
    }
    void restore(const PoolMemory<T>& saved){
        if(saved.high!=slots.high||saved.cursor!=slots.cursor)throw std::invalid_argument("Paint baseline allocator state changed: H "+std::to_string(saved.high)+"/"+std::to_string(slots.high)+", cursor "+std::to_string(saved.cursor)+"/"+std::to_string(slots.cursor));
        for(const auto& r:saved.retired){budget.visit();budget.allocate(value_storage(r.value)+r.suffix.size());auto& c=cell(r.index);
            if(c.active)throw std::invalid_argument("Paint baseline retired identity is active");c.value=r.value;c.suffix=r.suffix;}
    }
};
using Memory=vector_paint_history_detail::CacheMemory;
template<class Map,class Records>void query_records(Map& target,const Records& records,Budget& b){
    for(const auto& r:records){b.visit(1+std::bit_width(target.size()));if(target.contains(r.index))continue;b.allocate(64);target.emplace(r.index,&r.value);}
}
void query_backing(vector_probe_detail::PaintGraph& graph,const Memory& memory,Budget& b){
    query_records(graph.lines.vertices,std::get<0>(memory).retired,b);query_records(graph.lines.subcurves,std::get<2>(memory).retired,b);
    query_records(graph.lines.models,std::get<3>(memory).retired,b);query_records(graph.loops,std::get<6>(memory).retired,b);query_records(graph.regions,std::get<7>(memory).retired,b);
}
struct State {
    VectorPaintArchive envelope;Shape shape;
    std::vector<Child> child;
    Pool<VectorVertex> vertices;Pool<VectorLineScale> scales;Pool<VectorSubcurve> subs;Pool<VectorModelCurve> models;Pool<VectorCrosspoint> crosses;Pool<VectorLine> lines;
    Pool<VectorArchiveLoop> loops;Pool<VectorArchiveRegion> regions;Pool<VectorArchiveFace> faces;Pool<Bytes> nodes,grads,textures;
    Budget& b;
    State(const Layer& owner,VectorPaintArchive a,Budget& budget,bool fresh=false,Shape extension={}):envelope(charged_state(std::move(a),budget)),shape(std::move(extension)),child(children(owner,budget)),
        vertices(envelope.base.vertices,fresh?std::nullopt:owner.runtime_vector_high_water[0],budget),scales(envelope.base.scales,fresh?std::nullopt:owner.runtime_vector_high_water[1],budget),subs(envelope.base.subcurves,fresh?std::nullopt:owner.runtime_vector_high_water[2],budget),
        models(envelope.base.models,fresh?std::nullopt:owner.runtime_vector_high_water[3],budget),crosses(envelope.base.crosspoints,fresh?std::nullopt:owner.runtime_vector_high_water[4],budget),lines(envelope.base.lines,fresh?std::nullopt:owner.runtime_vector_high_water[5],budget),
        loops(envelope.loops,fresh?std::nullopt:owner.runtime_vector_high_water[6],budget),regions(envelope.regions,fresh?std::nullopt:owner.runtime_vector_high_water[7],budget),faces(envelope.faces,fresh?std::nullopt:owner.runtime_vector_high_water[8],budget),
        nodes(envelope.gradnodes,fresh?std::nullopt:owner.runtime_vector_high_water[9],budget),grads(envelope.gradations,fresh?std::nullopt:owner.runtime_vector_high_water[10],budget),textures(envelope.textures,fresh?std::nullopt:owner.runtime_vector_high_water[11],budget),b(budget){if(owner.family==0x40002000u&&!shape)shape.emplace();if(fresh)for(auto& c:child){for(auto& list:c.lists)list.clear();if(c.topology){c.topology->vertices.clear();c.topology->models.clear();c.topology->lines.clear();}}}
    auto pools(){return std::tie(vertices,scales,subs,models,crosses,lines,loops,regions,faces,nodes,grads,textures);}
    template<std::size_t... I>void restore(const Memory& memory,std::index_sequence<I...>){auto p=pools();([&]{try{std::get<I>(p).restore(std::get<I>(memory));}catch(const std::invalid_argument& e){throw std::invalid_argument("Paint history pool "+std::to_string(I)+": "+e.what());}}(),...);}
    void restore(const Memory& memory){restore(memory,std::make_index_sequence<12>{});}
    Memory remember(){
        std::size_t count{};std::apply([&](const auto&... p){((count+=p.cells.size()),...);},pools());if(shape)count+=shape->shapes.size();
        if(count>4096)throw std::length_error("Paint baseline backing record limit");
        return std::apply([](const auto&... p){return Memory{p.remember()...};},pools());
    }
    void query_backing(vector_probe_detail::PaintGraph& graph){
        const auto add=[&](auto& target,const auto& pool){for(const auto& [id,c]:pool.cells){b.visit(1+std::bit_width(target.size()));if(target.contains(id))continue;b.allocate(64);target.emplace(id,&c.value);}};
        add(graph.lines.vertices,vertices);add(graph.lines.subcurves,subs);add(graph.lines.models,models);add(graph.loops,loops);add(graph.regions,regions);
    }
    //59CE10 drops pointer nodes; surviving MC side fields remain in backing.
    void clear_loop(VectorArchiveLoop& loop){loop.members.clear();}
    void material(int id,bool gradient){
        auto& pool=gradient?grads:textures;if(!pool.active(id))return;pool.retire(id,b);
        if(gradient){auto g=decode_vector_gradation(pool.value(id));std::set<int> stops;
            for(int stop:g.stops)if(stop<0||!nodes.active(stop)||!stops.insert(stop).second)throw std::invalid_argument("Paint history requires exclusive live gradient stops");
            if(!stops.empty())for(const auto& c:child)if(paint_plane(c.family))for(int other:c.lists[1])if(other!=id&&grads.active(other)){
                Reader r(grads.value(other));r.u32();const auto n=r.i32();r.require_count(n,4,8192);b.visit(n);for(int j=0;j<n;++j)if(stops.contains(r.i32()))throw std::invalid_argument("Paint history shared gradient stop during clear");}
            for(int node:g.stops)nodes.retire(node,b);g.stops.clear();g.faces.clear();pool.value(id)=encode_vector_gradation(g);}
        else{auto t=decode_vector_texture(pool.value(id));t.offscreen=Bytes(4,0);t.metadata.clear();t.faces.clear();pool.value(id)=encode_vector_texture(t);}
        for(auto& c:child)if(paint_plane(c.family))remove_first(c.lists[gradient?1:2],id);
    }
    void free_face(int id){
        if(!faces.active(id))return;faces.retire(id,b);auto& f=faces.value(id);const auto kind=f.flags&15;
        if((kind==2||kind==4)&&f.material>=0){auto& pool=kind==2?grads:textures;
            if(kind==2){auto g=decode_vector_gradation(pool.value(f.material));const auto before=g.faces.size();remove_first(g.faces,id);pool.value(f.material)=encode_vector_gradation(g);if(g.faces.size()!=before&&g.faces.empty())material(f.material,true);}
            else{auto t=decode_vector_texture(pool.value(f.material));const auto before=t.faces.size();remove_first(t.faces,id);pool.value(f.material)=encode_vector_texture(t);if(t.faces.size()!=before&&t.faces.empty())material(f.material,false);}
        }
        if(f.region>=0)remove_first(regions.value(f.region).faces,id);for(auto& c:child)if(paint_plane(c.family))remove_first(c.lists[0],id);
        f.material=-1;
    }
    void free_region(int id,bool mark){
        if(!regions.active(id))return;auto& r=regions.value(id);
        if(mark)for(int face:r.faces)if(face>=0){const auto& f=faces.value(face);
            if(f.material>=0&&(f.flags&2)){auto g=decode_vector_gradation(grads.value(f.material));g.flags|=0x10000000u;grads.value(f.material)=encode_vector_gradation(g);}
            else if(f.material>=0&&(f.flags&4)){auto t=decode_vector_texture(textures.value(f.material));t.flags|=0x10000000u;textures.value(f.material)=encode_vector_texture(t);}}
        regions.retire(id,b);clear_loop(r.outer);const auto holes=r.holes;for(int hole:holes){loops.retire(hole,b);clear_loop(loops.value(hole));}r.holes.clear();
        const auto saved=r.faces;for(int face:saved)if(face>=0)free_face(face);r.faces.clear();remove_first(envelope.border.regions,id);
    }
    void remove_vertex(int id){remove_first(envelope.border.vertices,id);for(auto& c:child)if(c.topology)remove_first(c.topology->vertices,id);}
    void endpoint(int vertex,int sc){auto& v=vertices.value(vertex);const auto before=v.subcurves.size();remove_first(v.subcurves,sc);if(v.subcurves.size()!=before&&v.subcurves.empty()){vertices.retire(vertex,b);remove_vertex(vertex);}}
    void free_model(int id){
        if(!models.active(id))return;models.retire(id,b);auto& m=models.value(id);const auto chain=m.subcurves;
        for(unsigned side=0;side<2;++side)if(m.crosspoints[side]>=0){crosses.retire(m.crosspoints[side],b);m.flags&=~(0x10u<<side);m.crosspoints[side]=-1;}
        if(!chain.empty()){endpoint(subs.value(chain.front()).vertex_a,chain.front());endpoint(subs.value(chain.back()).vertex_b,chain.back());
            for(std::size_t i=0;i+1<chain.size();++i){const int v=subs.value(chain[i]).vertex_b;vertices.retire(v,b);vertices.value(v).subcurves.clear();remove_vertex(v);}}
        for(int sc:chain){const auto widths=subs.value(sc).scales;subs.retire(sc,b);subs.value(sc).scales.clear();for(int width:widths)scales.retire(width,b);}
        m.subcurves.clear();if(!chain.empty())m.scalars={};remove_first(envelope.border.models,id);for(auto& c:child)if(c.topology)remove_first(c.topology->models,id);
    }
    void free_line(int id){if(!lines.active(id))return;lines.retire(id,b);auto& line=lines.value(id);for(auto member:line.members)models.value(member.model).line=-1;line.members.clear();line.archived_bounds={};for(auto& c:child)if(line_plane(c.family))remove_first(c.lists[0],id);}
    void clear(std::optional<VectorBounds> window){
        std::vector<int> region_ids,model_ids;std::vector<std::vector<int>> line_ids(child.size());
        if(window){vector_probe_detail::PaintGraph graph(envelope,{b.work,b.stop});query_backing(graph);for(int id:envelope.border.regions)if(region_matches(graph,get(envelope.regions,id),*window,b))region_ids.push_back(id);
            for(int id:envelope.border.models){const auto& m=get(envelope.base.models,id);if(overlap(*window,{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]}))model_ids.push_back(id);}}
        else{region_ids=envelope.border.regions;model_ids=envelope.border.models;}
        for(std::size_t i=0;i<child.size();++i)if(line_plane(child[i].family))for(int id:child[i].lists[0])if(child[i].topology||!window||overlap(*window,get(envelope.base.lines,id).archived_bounds))line_ids[i].push_back(id);
        for(int id:region_ids)free_region(id,bool(window));for(int id:model_ids)free_model(id);
        if(!window){const auto remaining=envelope.border.vertices;for(int id:remaining){vertices.retire(id,b);vertices.value(id).subcurves.clear();}envelope.border.vertices.clear();}
        //Native partial Paint copies only LC references for Vert. Keep the
        //whole independent child as a modern safety envelope instead of
        //publishing references to inactive MC backing.
        for(auto& c:child)if(c.topology){const auto ms=c.topology->models;for(int id:ms)free_model(id);const auto vs=c.topology->vertices;for(int id:vs){vertices.retire(id,b);vertices.value(id).subcurves.clear();}c.topology->vertices.clear();c.topology->models.clear();}
        for(std::size_t i=0;i<child.size();++i){if(line_plane(child[i].family)){for(int id:line_ids[i])free_line(id);}
            else{for(int lane:{1,2}){const auto ids=child[i].lists[lane];for(int id:ids){auto& pool=lane==1?grads:textures;if(!pool.active(id))continue;if(material_flags(pool.value(id),lane==1)&0x10000000u)material(id,lane==1);}}}}
    }
    VectorPaintArchive packet(){
        b.allocate(vertices.storage()+scales.storage()+subs.storage()+models.storage()+crosses.storage()+lines.storage()+loops.storage()+regions.storage()+faces.storage()+nodes.storage()+grads.storage()+textures.storage());
        auto& a=envelope;a.base.vertices=vertices.output();a.base.scales=scales.output();a.base.subcurves=subs.output();a.base.models=models.output();a.base.crosspoints=crosses.output();a.base.lines=lines.output();
        a.loops=loops.output();a.regions=regions.output();a.faces=faces.output();a.gradnodes=nodes.output();a.gradations=grads.output();a.textures=textures.output();
        const auto publish=[&](const auto& p,unsigned k){if(p.touched)(k<6?a.base.high_water[k]:a.face_high_water[k-6])=p.slots.high;};
        publish(vertices,0);publish(scales,1);publish(subs,2);publish(models,3);publish(crosses,4);publish(lines,5);publish(loops,6);publish(regions,7);publish(faces,8);publish(nodes,9);publish(grads,10);publish(textures,11);
        return std::move(a);
    }
    Layer carrier(const Layer& owner){
        b.allocate(8*std::uint64_t(owner.specific.size())+8192);Layer result=owner;
        const auto publish=[&](const auto& p,unsigned k){if(p.touched)result.runtime_vector_high_water[k]=p.slots.high;};
        publish(vertices,0);publish(scales,1);publish(subs,2);publish(models,3);publish(crosses,4);publish(lines,5);publish(loops,6);publish(regions,7);publish(faces,8);publish(nodes,9);publish(grads,10);publish(textures,11);
        for(std::size_t i=0;i<child.size();++i)publish_child(result.planes[i],child[i]);return result;
    }
    Layer finish(const Layer& owner){
        Layer result=carrier(owner);const auto a=packet();b.allocate(4*archive_storage(a));store_content(result,a,shape);
        // The union of retained outsiders and restored IDs must remain inside
        // the same admission domain that the next opposite replay requires.
        (void)decode(result,b);(void)children(result,b);b.visit();return result;
    }
};

Bytes clone_image(const Bytes& raw,Budget& b){
    Reader prefix(raw);auto outer=prefix.section();if(!outer.remaining())return Bytes(4,0);
    auto h=outer.section();const auto version=h.u16();const auto width=h.u32(),height=h.u32(),depth=h.u32();
    const auto dpi=h.f64();const auto count=h.u32();const auto order=h.u8();
    if((version>>8)>1||!width||!height||width>100000||height>100000||std::uint64_t(width)*height>268435456||!std::isfinite(dpi)||dpi<=0||count>100000)
        throw std::invalid_argument("Paint history texture image header limit");
    if((depth!=1&&depth!=2&&depth!=8&&depth!=16&&depth!=24&&depth!=32&&depth!=64)||order!=(depth==24?4:depth==32||depth==64?3:0))
        throw std::invalid_argument("Paint history texture image format");
    const auto tile_bytes=std::uint64_t(256*256)*depth/8;
    b.allocate(2*tile_bytes*count+4*tile_bytes+raw.size());b.visit(tile_bytes*count+4*raw.size());
    Reader reader(raw);const auto image=decode_offscreen(reader);if(reader.remaining())throw std::invalid_argument("Paint history trailing texture image");
    // CopyAll traverses the logical frame and retains complete present tile
    // backing, including edge padding and present-zero blocks. Optimize is
    // deliberately absent. Image framing/suffixes belong to fresh objects.
    int left=int(width),top=int(height),right{},bottom{};
    for(const auto& [p,tile]:image.tiles){(void)tile;if(p.x<0||p.y<0||p.x*256>=int(width)||p.y*256>=int(height))continue;
        left=std::min(left,p.x*256);top=std::min(top,p.y*256);right=std::max(right,std::min(int(width),(p.x+1)*256));bottom=std::max(bottom,std::min(int(height),(p.y+1)*256));}
    if(!right||!bottom)left=top=0;
    const auto slots=std::uint64_t((right-left+255)/256)*((bottom-top+255)/256);
    if(slots>100000)throw std::length_error("Paint history texture grid limit");b.visit(slots);
    Writer body,header;header.u16(0x100);header.u32(width);header.u32(height);header.u32(depth);header.f64(dpi);header.u32(std::uint32_t(slots));header.u8(order);body.section(header.data);
    if(slots){Writer extent,blocks;for(int n:{left,top,right,bottom})extent.i32(n);extent.u8(1);body.section(extent.data);
        for(int y=top;y<bottom;y+=256)for(int x=left;x<right;x+=256){b.visit();const auto found=image.tiles.find({x/256,y/256});blocks.u8(found!=image.tiles.end());if(found==image.tiles.end())continue;
            const auto& pixels=*found->second;uLongf size=compressBound(static_cast<uLong>(pixels.size()));Bytes compressed(size);
            if(compress2(compressed.data(),&size,pixels.data(),static_cast<uLong>(pixels.size()),Z_DEFAULT_COMPRESSION)!=Z_OK)throw std::runtime_error("Paint history texture compression failed");
            compressed.resize(size);blocks.u32(std::uint32_t(pixels.size()));blocks.u32(std::uint32_t(size));blocks.raw(compressed);}
        body.section(blocks.data);
    }Writer framed;framed.section(body.data);b.allocate(framed.data.size());return std::move(framed.data);
}

struct Copier {
    State& d;VectorPaintArchive& s;Budget& b;bool partial;
    std::set<int> owned_gradients;
    vector_graph_detail::OwnerLists* vertex_owner{};
    template<class T>const T& read(const std::vector<VectorPoolRecord<T>>& pool,int id){b.visit(pool.size()+1);return get(pool,id);}
    void vertex(int id,bool adjacency){
        if(id<0)throw std::invalid_argument("Paint history requires live SC endpoints");
        const auto& source=read(s.base.vertices,id);const bool fresh=d.vertices.activate(id,b);auto& target=d.vertices.value(id);
        if(fresh){target.x=source.x;target.y=source.y;if(partial)append_owner(vertex_owner?vertex_owner->vertices:d.envelope.border.vertices,id);}
        if(fresh||adjacency){target.subcurves=source.subcurves;nullable_refs(target.subcurves);}
    }
    void scale(int id){d.scales.activate(id,b);d.scales.value(id)=read(s.base.scales,id);}
    void sub(int id,bool deep){
        d.subs.activate(id,b);const auto& source=read(s.base.subcurves,id);auto& target=d.subs.value(id);target=source;target.vertex_a=reference(source.vertex_a);target.vertex_b=reference(source.vertex_b);target.model=reference(source.model);
        if(deep){vertex(source.vertex_a,false);vertex(source.vertex_b,false);for(int width:source.scales)scale(width);}
    }
    void cross(int id){d.crosses.activate(id,b);d.crosses.value(id)=read(s.base.crosspoints,id);}
    void model(int id){
        d.models.activate(id,b);const auto& source=read(s.base.models,id);auto& target=d.models.value(id);
        target.flags=source.flags&0xffffu;target.subcurves=source.subcurves;target.line=reference(source.line);target.scalars=source.scalars;
        for(unsigned side=0;side<2;++side){if(source.targets[side]>=0){target.targets[side]=source.targets[side];target.target_kinds[side]=source.target_kinds[side];}
            if(!partial||source.crosspoints[side]>=0)target.crosspoints[side]=reference(source.crosspoints[side]);}
        if(partial){for(int sc:source.subcurves)sub(sc,true);for(int cp:source.crosspoints)if(cp>=0)cross(cp);
            if(!source.subcurves.empty()){vertex(read(s.base.subcurves,source.subcurves.front()).vertex_a,true);vertex(read(s.base.subcurves,source.subcurves.back()).vertex_b,true);}
            append_owner(vertex_owner?vertex_owner->models:d.envelope.border.models,id);}
    }
    void line(int id){d.lines.activate(id,b);const auto& source=read(s.base.lines,id);d.lines.value(id)=source;
        for(auto m:source.members)d.models.value(m.model).line=id;}
    void loop(VectorArchiveLoop& target,const VectorArchiveLoop& source,bool embedded,int id){
        auto suffix=std::move(target.suffix);target=source;target.suffix=std::move(suffix);target.region=reference(source.region);
        for(auto m:source.members){auto& model=d.models.value(m.model);model.targets[m.selector!=0]=id;model.target_kinds[m.selector!=0]=embedded?1:0;}}
    void hole(int id){d.loops.activate(id,b);loop(d.loops.value(id),read(s.loops,id),false,id);}
    void region(int id){
        d.regions.activate(id,b);const auto& source=read(s.regions,id);auto& target=d.regions.value(id);
        loop(target.outer,source.outer,true,id);target.holes=source.holes;target.faces=source.faces;nullable_refs(target.faces);
        if(partial){for(int h:source.holes)hole(h);append_owner(d.envelope.border.regions,id);}
    }
    void face(int id){
        d.faces.activate(id,b);const auto& source=read(s.faces,id);auto& target=d.faces.value(id);
        target.region=reference(source.region);target.flags=source.flags&0xffffu;target.color=0;target.material=-1;
        if((source.flags&15)==1)target.color=source.color;
        else if((source.flags&15)==2||(source.flags&15)==4)target.material=reference(source.material);
    }
    void node(int id){
        d.nodes.activate(id,b);const auto source=decode_vector_gradnode(read(s.gradnodes,id));auto& raw=d.nodes.value(id);
        auto target=raw.empty()?VectorArchiveGradNode{}:decode_vector_gradnode(raw);target.color=source.color;target.position_bits=source.position_bits;raw=encode_vector_gradnode(target);
    }
    bool gradient(int id){
        const bool active=d.grads.active(id);auto source=decode_vector_gradation(read(s.gradations,id));nullable_refs(source.faces);auto& raw=d.grads.value(id);
        auto target=raw.empty()?VectorArchiveGradation{}:decode_vector_gradation(raw);
        if(partial&&active){target.faces=std::move(source.faces);raw=encode_vector_gradation(target);return false;}
        if(source.stops.size()+source.faces.size()>8192)throw std::length_error("Paint history gradient reference limit");
        std::set<int> stops;for(int stop:source.stops){if(stop<0||!stops.insert(stop).second)throw std::invalid_argument("Paint history requires exclusive live gradient stops");(void)read(s.gradnodes,stop);}
        if(!stops.empty())for(const auto& other:s.gradations)if(other.index!=id&&(!partial||owned_gradients.contains(other.index))){Reader r(other.value);r.u32();const auto count=r.i32();r.require_count(count,4,8192);b.visit(count);
            for(int j=0;j<count;++j)if(stops.contains(r.i32()))throw std::invalid_argument("Paint history shared gradient stop");}
        d.grads.activate(id,b);auto suffix=std::move(target.suffix);target=source;target.suffix=std::move(suffix);target.flags&=0xffffu;
        if(partial)for(int stop:source.stops)node(stop);raw=encode_vector_gradation(target);return true;
    }
    bool texture(int id){
        const bool active=d.textures.active(id);auto source=decode_vector_texture(read(s.textures,id));nullable_refs(source.faces);auto& raw=d.textures.value(id);
        auto target=raw.empty()?VectorArchiveTexture{}:decode_vector_texture(raw);
        if(source.faces.size()>8192)throw std::length_error("Paint history texture reference limit");
        if(partial&&active){target.faces=source.faces;raw=encode_vector_texture(target);return false;}
        d.textures.activate(id,b);target.flags=source.flags&0xffffu;target.faces=source.faces;target.origin_x=source.origin_x;target.origin_y=source.origin_y;
        target.scale_x_bits=source.scale_x_bits;target.scale_y_bits=source.scale_y_bits;target.rotation_bits=source.rotation_bits;target.shear_x_bits=source.shear_x_bits;
        if(source.offscreen!=Bytes(4,0))target.offscreen=clone_image(source.offscreen,b);else if(target.offscreen.empty())target.offscreen=Bytes(4,0);
        //5AB500 never copies RCString; receiver metadata and opaque suffix stay.
        raw=encode_vector_texture(target);return true;
    }
    void mark_region(int id){
        for(int f:read(s.regions,id).faces)if(f>=0){auto& face=const_cast<VectorArchiveFace&>(read(s.faces,f));face.flags|=0x10000000u;
            if(face.material>=0&&(face.flags&2)){auto& raw=const_cast<Bytes&>(read(s.gradations,face.material));auto g=decode_vector_gradation(raw);g.flags|=0x10000000u;raw=encode_vector_gradation(g);}
            else if(face.material>=0&&(face.flags&4)){auto& raw=const_cast<Bytes&>(read(s.textures,face.material));auto t=decode_vector_texture(raw);t.flags|=0x10000000u;raw=encode_vector_texture(t);}}
    }
    void partial_copy(const std::vector<Child>& source,std::optional<VectorBounds> window,const Memory* memory=nullptr){
        for(const auto& c:source)if(c.topology){
            b.allocate(64*(c.topology->vertices.size()+c.topology->models.size()+c.lists[0].size()+1));
            const std::set<int> vertices(c.topology->vertices.begin(),c.topology->vertices.end()),models(c.topology->models.begin(),c.topology->models.end()),lines(c.lists[0].begin(),c.lists[0].end());
            for(int id:s.border.vertices){b.visit();if(vertices.contains(id))throw std::invalid_argument("Paint partial history requires exclusive Vert vertices");}
            for(int id:s.border.models){b.visit();if(models.contains(id))throw std::invalid_argument("Paint partial history requires exclusive Vert models");}
            for(int id:vertices)for(int sub:read(s.base.vertices,id).subcurves){b.visit();if(sub>=0&&!models.contains(read(s.base.subcurves,sub).model))throw std::invalid_argument("Paint partial history Vert adjacency leaves its child");}
            for(int id:models){const auto& m=read(s.base.models,id);if((m.line>=0&&!lines.contains(m.line))||m.targets[0]>=0||m.targets[1]>=0)throw std::invalid_argument("Paint partial history requires an independent Vert child");
                for(int sub:m.subcurves){const auto& sc=read(s.base.subcurves,sub);if(!vertices.contains(sc.vertex_a)||!vertices.contains(sc.vertex_b))throw std::invalid_argument("Paint partial history Vert endpoint leaves its child");}
                for(int cp:m.crosspoints)if(cp>=0)for(const auto& other:s.base.models){b.visit();if(other.index!=id&&(other.value.crosspoints[0]==cp||other.value.crosspoints[1]==cp))throw std::invalid_argument("Paint partial history Vert CrossPoint has competing owners");}}
            for(int id:lines)for(auto member:read(s.base.lines,id).members){b.visit();if(!models.contains(member.model))throw std::invalid_argument("Paint partial history Vert LC leaves its child");}
        }
        for(const auto& c:source)if(paint_plane(c.family))owned_gradients.insert(c.lists[1].begin(),c.lists[1].end());
        vector_probe_detail::PaintGraph graph(s,{b.work,b.stop});if(memory)query_backing(graph,*memory,b);
        for(int id:s.border.models){const auto& m=read(s.base.models,id);if(!window||overlap(*window,{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]}))model(id);}
        for(int id:s.border.regions)if(!window||region_matches(graph,read(s.regions,id),*window,b)){region(id);mark_region(id);}
        for(std::size_t i=0;i<source.size();++i){const auto& sc=source[i];auto& dc=d.child[i];
            if(sc.topology){
                if(!dc.topology)throw std::invalid_argument("Paint history Vert topology kind changed");
                vertex_owner=&*dc.topology;
                for(int id:sc.topology->vertices)vertex(id,true);
                for(int id:sc.topology->models)model(id);
                for(int id:sc.lists[0]){line(id);append_owner(dc.lists[0],id);}
                vertex_owner=nullptr;continue;
            }
            if(line_plane(sc.family)){for(int id:sc.lists[0])if(!window||overlap(*window,read(s.base.lines,id).archived_bounds)){line(id);append_owner(dc.lists[0],id);}}
            else{for(int id:sc.lists[0]){auto& f=const_cast<VectorArchiveFace&>(read(s.faces,id));if(f.flags&0x10000000u){f.flags&=~0x10000000u;face(id);append_owner(dc.lists[0],id);}}
                for(int lane:{1,2})for(int id:sc.lists[lane]){auto& raw=const_cast<Bytes&>(read(lane==1?s.gradations:s.textures,id));
                    if(!(material_flags(raw,lane==1)&0x10000000u))continue;
                    if(lane==1){auto g=decode_vector_gradation(raw);g.flags&=~0x10000000u;raw=encode_vector_gradation(g);}else{auto t=decode_vector_texture(raw);t.flags&=~0x10000000u;raw=encode_vector_texture(t);}
                    if(lane==1?gradient(id):texture(id))append_owner(dc.lists[lane],id);
                }}
        }
    }
    template<class T,class F>void ascending(const std::vector<VectorPoolRecord<T>>& pool,F copy){std::vector<int> ids;ids.reserve(pool.size());for(const auto& r:pool)ids.push_back(r.index);std::ranges::sort(ids);for(int id:ids){b.visit();copy(id);}}
    void full_copy(const std::vector<Child>& source){
        ascending(s.base.vertices,[&](int id){vertex(id,true);});ascending(s.base.scales,[&](int id){scale(id);});ascending(s.base.subcurves,[&](int id){sub(id,false);});
        ascending(s.base.models,[&](int id){model(id);});ascending(s.base.crosspoints,[&](int id){cross(id);});ascending(s.base.lines,[&](int id){line(id);});
        ascending(s.loops,[&](int id){hole(id);});ascending(s.regions,[&](int id){region(id);});ascending(s.faces,[&](int id){face(id);});
        ascending(s.gradnodes,[&](int id){node(id);});ascending(s.gradations,[&](int id){gradient(id);});ascending(s.textures,[&](int id){texture(id);});
        for(int id:s.border.vertices){const auto& live=d.envelope.border.vertices;
            //644800 tests next/prev and misses a surviving singleton. Its
            //append would create a self-link/count mismatch. Keep one logical
            //owner occurrence here; xy/adjacency still use native copy rules.
            if(std::ranges::find(live,id)!=live.end())continue;
            append_owner(d.envelope.border.vertices,id);}
        for(int id:s.border.models)append_owner(d.envelope.border.models,id);for(int id:s.border.regions)append_owner(d.envelope.border.regions,id);
        for(std::size_t i=0;i<source.size();++i){if(source[i].topology){if(!d.child[i].topology)throw std::invalid_argument("Paint history Vert topology kind changed");const auto append=[&](auto& out,const auto& in){for(int id:in)if(std::ranges::find(out,id)==out.end())out.push_back(id);};append(d.child[i].topology->vertices,source[i].topology->vertices);for(int id:source[i].topology->models)append_owner(d.child[i].topology->models,id);}for(unsigned lane=0;lane<3;++lane)for(int id:source[i].lists[lane])append_owner(d.child[i].lists[lane],id);}
    }
};
}

struct Snapshot {
    VectorPaintArchive content;Shape shape;std::vector<Child> child;
    std::vector<std::vector<int>> line_order;
    std::optional<VectorBounds> window;
    std::uint64_t owner{};
};
VectorPaintArchive copy_draw_extensions(const Layer& live,VectorPaintArchive destination,VectorPaintArchive source,Budget& b,CacheMemory* memory){
    auto carrier=live;carrier.family=0x21200000u;carrier.planes.clear();
    State target(carrier,std::move(destination),b);if(memory)target.restore(*memory);Copier copy{target,source,b,false,{}};
    copy.ascending(source.loops,[&](int id){copy.hole(id);});copy.ascending(source.regions,[&](int id){copy.region(id);});
    copy.ascending(source.faces,[&](int id){copy.face(id);});copy.ascending(source.gradnodes,[&](int id){copy.node(id);});
    copy.ascending(source.gradations,[&](int id){copy.gradient(id);});copy.ascending(source.textures,[&](int id){copy.texture(id);});
    auto result=target.packet();if(memory)*memory=target.remember();return result;
}
struct ManagerState {Layer owner;VectorPaintArchive content;Memory memory;Shape shape;};

namespace {
std::shared_ptr<const Snapshot> capture(const Layer& owner,VectorPaintArchive& source,std::optional<VectorBounds> window,Budget& b,const Memory* memory=nullptr,Shape shape={}){
    b.allocate(sizeof(Snapshot));auto snapshot=std::make_shared<Snapshot>();snapshot->window=window;snapshot->owner=owner.common.runtime_identity;
    auto source_child=children(owner,b);State fresh(owner,{},b,true);Copier copy{fresh,source,b,bool(window),{}};
    if(window){snapshot->line_order.resize(source_child.size());for(std::size_t i=0;i<source_child.size();++i)if(line_plane(source_child[i].family))snapshot->line_order[i]=source_child[i].lists[0];copy.partial_copy(source_child,effective_window(window),memory);}
    else{copy.full_copy(source_child);copy_shapes(fresh.shape,shape,b);}
    snapshot->shape=fresh.shape;snapshot->content=fresh.packet();snapshot->child=std::move(fresh.child);return snapshot;
}
// Resolve children by bound runtime identity. Exact unchanged layouts also
// admit copied fixture identities; a reordered ambiguous identity rejects.
std::vector<Child> mapped_children(const Snapshot& packet,const std::vector<Child>& receiver){
    if(packet.child.size()!=receiver.size())throw std::invalid_argument("Paint history child membership changed");
    bool exact=true;for(std::size_t i=0;i<receiver.size();++i)exact=exact&&packet.child[i].identity==receiver[i].identity&&packet.child[i].family==receiver[i].family;
    std::vector<Child> ordered;ordered.reserve(receiver.size());std::set<std::size_t> used;
    for(std::size_t i=0;i<receiver.size();++i){std::optional<std::size_t> found;
        if(exact)found=i;else for(std::size_t j=0;j<packet.child.size();++j)if(receiver[i].identity==packet.child[j].identity&&receiver[i].family==packet.child[j].family){if(found)throw std::invalid_argument("Paint history ambiguous child identity");found=j;}
        if(!found||!used.insert(*found).second)throw std::invalid_argument("Paint history missing child identity");ordered.push_back(packet.child[*found]);ordered.back().hint=*found;
    }return ordered;
}
void consume_markers(VectorPaintArchive& source,const std::vector<Child>& child,std::optional<VectorBounds> window,Budget& b){
    vector_probe_detail::PaintGraph graph(source,{b.work,b.stop});
    for(int id:source.border.regions)if(!window||region_matches(graph,get(source.regions,id),*window,b))for(int face:get(source.regions,id).faces)if(face>=0){
        auto& f=const_cast<VectorArchiveFace&>(get(source.faces,face));f.flags|=0x10000000u;
        if(f.material>=0&&(f.flags&2)){auto& raw=const_cast<Bytes&>(get(source.gradations,f.material));auto g=decode_vector_gradation(raw);g.flags|=0x10000000u;raw=encode_vector_gradation(g);}
        else if(f.material>=0&&(f.flags&4)){auto& raw=const_cast<Bytes&>(get(source.textures,f.material));auto t=decode_vector_texture(raw);t.flags|=0x10000000u;raw=encode_vector_texture(t);}}
    for(const auto& c:child)if(paint_plane(c.family)){
        for(int id:c.lists[0])const_cast<VectorArchiveFace&>(get(source.faces,id)).flags&=~0x10000000u;
        for(int lane:{1,2})for(int id:c.lists[lane]){auto& raw=const_cast<Bytes&>(get(lane==1?source.gradations:source.textures,id));
            if(!(material_flags(raw,lane==1)&0x10000000u))continue;
            if(lane==1){auto g=decode_vector_gradation(raw);g.flags&=~0x10000000u;raw=encode_vector_gradation(g);}else{auto t=decode_vector_texture(raw);t.flags&=~0x10000000u;raw=encode_vector_texture(t);}}
    }
}
bool compatible(const Layer& a,const Layer& b){
    if(a.planes.size()!=b.planes.size())return false;
    for(std::size_t i=0;i<a.planes.size();++i)if(a.planes[i].family!=b.planes[i].family)return false;return true;
}
void apply_packet(State& target,const Snapshot& saved,Budget& b,bool positional=false){
    auto child=positional?saved.child:mapped_children(saved,target.child);
    if(positional){if(child.size()!=target.child.size())throw std::invalid_argument("Paint baseline child membership changed");
        for(std::size_t i=0;i<child.size();++i){if(child[i].family!=target.child[i].family)throw std::invalid_argument("Paint baseline child family changed");child[i].hint=i;}}
    target.clear(effective_window(saved.window));
    //44F3F0 always uses633E60: membership was fixed at capture, including
    //external references, so replay copies the active packet bitmap.
    b.allocate(2*archive_storage(saved.content)+8192);auto content=saved.content;Copier copy{target,content,b,false,{}};copy.full_copy(child);copy_shapes(target.shape,saved.shape,b);
    if(saved.window)for(std::size_t i=0;i<child.size();++i)if(line_plane(child[i].family)){
        const auto& order=saved.line_order[child[i].hint];auto current=target.child[i].lists[0],expected=order;std::ranges::sort(current);std::ranges::sort(expected);
        if(current!=expected)throw std::invalid_argument("Paint history complete LC order membership changed");target.child[i].lists[0]=order;}
}
std::shared_ptr<const ManagerState> remember(State& target,const Layer& owner,Budget& b){
    b.allocate(sizeof(ManagerState));auto state=std::make_shared<ManagerState>();state->owner=target.carrier(owner);state->owner.specific.clear();
    state->memory=target.remember();state->shape=target.shape;state->content=target.packet();return state;
}
}

std::shared_ptr<const Edit> Edit::bind(const Layer& owner,std::optional<VectorBounds> raw,Budget& b){
    auto source=decode(owner,b);b.allocate(sizeof(Edit));auto edit=std::make_shared<Edit>();edit->primary_=capture(owner,source,inflated(raw),b,nullptr,shape_envelope(owner));b.visit();return edit;
}
std::optional<VectorBounds> Edit::window()const{if(!primary_)throw std::logic_error("Unbound Paint history");return primary_->window;}
Layer Edit::synchronize_source(const Layer& owner,Budget& b)const{
    const auto scope=window();if(owner.common.runtime_identity!=primary_->owner)throw std::invalid_argument("Paint history owner identity changed");auto source=decode(owner,b);auto child=children(owner,b);
    if(scope)consume_markers(source,child,effective_window(scope),b);b.allocate(8*std::uint64_t(owner.specific.size())+8192);
    auto result=owner;store_content(result,std::move(source));b.visit();return result;
}
Prepared Edit::replay(const Layer& receiver,Budget& b)const{
    if(draw_){auto result=draw_->replay(receiver,b);b.allocate(sizeof(Edit));auto opposite=std::make_shared<Edit>();opposite->draw_=std::move(result.opposite);return {std::move(result.layer),std::move(opposite)};}
    if(!primary_)throw std::logic_error("Unbound Paint history");if(receiver.common.runtime_identity!=primary_->owner)throw std::invalid_argument("Paint history owner identity changed");auto source=decode(receiver,b);
    b.allocate(sizeof(Edit));auto opposite=std::make_shared<Edit>(*this);
    if(!opposite->secondary_)opposite->secondary_=capture(receiver,source,primary_->window,b,nullptr,shape_envelope(receiver));
    const auto& saved=reverse_?*opposite->secondary_:*primary_;
    State target(receiver,std::move(source),b,false,shape_envelope(receiver));apply_packet(target,saved,b);auto layer=target.finish(receiver);opposite->reverse_=!reverse_;return {std::move(layer),std::move(opposite)};
}
const VectorPaintArchive* Manager::paint_baseline()const noexcept{return state_?&state_->content:nullptr;}
const Layer* Manager::baseline_owner()const noexcept{return state_?&state_->owner:draw_?draw_->baseline_owner():nullptr;}
std::shared_ptr<const Manager> Manager::ensure(std::shared_ptr<const Manager> previous,const Layer& owner,Budget& b,bool force,bool draw_mode,bool select_paint){
    const bool paint_owner=owner.family==0x21200000u||owner.family==0x40002000u||(owner.family==0x20800000u&&(select_paint||(previous&&previous->state_&&previous->owner_==owner.common.runtime_identity&&previous->family_==owner.family)));
    b.visit();if(previous&&!force&&previous->owner_==owner.common.runtime_identity&&previous->family_==owner.family&&(draw_mode?bool(previous->draw_):(!paint_owner||bool(previous->state_))))return previous;
    if(previous&&!force&&previous->owner_==owner.common.runtime_identity&&previous->family_==owner.family){
        if(draw_mode&&previous->state_){const auto& prior=*previous->state_;b.allocate(sizeof(Manager)+2*archive_storage(prior.content));std::apply([&](const auto&... p){(b.allocate(pool_storage(p.retired)),...);},prior.memory);if(prior.shape)b.allocate(pool_storage(prior.shape->shapes));auto result=std::make_shared<Manager>(*previous);result->draw_=vector_draw_history_detail::Manager::from_cache({prior.owner,prior.content,prior.shape,prior.memory},b);result->state_.reset();return result;}
        if(!draw_mode&&previous->draw_){auto cache=previous->draw_->cache(b);b.allocate(sizeof(Manager)+sizeof(ManagerState));auto state=std::make_shared<ManagerState>();state->owner=std::move(cache.owner);state->content=std::move(cache.content);state->memory=std::move(cache.memory);state->shape=std::move(cache.shape);auto result=std::make_shared<Manager>(*previous);result->state_=std::move(state);result->draw_.reset();return result;}
    }
    b.allocate(sizeof(Manager));auto result=std::make_shared<Manager>();result->owner_=owner.common.runtime_identity;result->family_=owner.family;
    if(draw_mode){result->draw_=vector_draw_history_detail::Manager::ensure(previous?previous->draw_:nullptr,owner,b,force);return result;}
    if(previous&&previous->draw_&&(owner.family==0x22100000u||owner.family==0x24400000u))result->draw_=vector_draw_history_detail::Manager::ensure(previous->draw_,owner,b,force);
    if(!paint_owner)return result;
    auto source=decode(owner,b);auto child=children(owner,b);auto carrier=owner;carrier.runtime_vector_high_water.fill(std::nullopt);
    State target(carrier,{},b,true);Copier copy{target,source,b,false,{}};copy.full_copy(child);copy_shapes(target.shape,shape_envelope(owner),b);
    result->state_=remember(target,carrier,b);b.visit();return result;
}
Bound Manager::bind(const Layer& post,std::optional<VectorBounds> raw,Budget& b)const{
    if(!state_||(post.family!=0x21200000&&post.family!=0x20800000&&post.family!=0x40002000u)||post.common.runtime_identity!=owner_)throw std::invalid_argument("Paint binder has no matching baseline");
    if(!compatible(post,state_->owner))throw std::invalid_argument("Paint binder baseline child families changed");
    b.allocate(2*archive_storage(state_->content)+sizeof(Edit));auto baseline=state_->content;auto edit=std::make_shared<Edit>();edit->primary_=capture(state_->owner,baseline,inflated(raw),b,&state_->memory,state_->shape);
    State target(state_->owner,std::move(baseline),b,false,state_->shape);target.restore(state_->memory);target.clear(effective_window(edit->window()));
    auto source=decode(post,b);auto child=children(post,b);Copier copy{target,source,b,bool(edit->window()),{}};
    if(edit->window())copy.partial_copy(child,effective_window(edit->window()));else{copy.full_copy(child);copy_shapes(target.shape,shape_envelope(post),b);}
    b.allocate(sizeof(Manager)+8*std::uint64_t(post.specific.size())+8192);
    auto manager=std::make_shared<Manager>(*this);manager->state_=remember(target,state_->owner,b);manager->draw_.reset();
    auto consumed=post;store_content(consumed,std::move(source));(void)decode(consumed,b);b.visit();
    return {std::move(consumed),std::move(edit),std::move(manager)};
}
Bound Manager::bind_draw(const Layer& before,const Layer& post,std::optional<std::size_t> plane,std::optional<VectorBounds> raw,Budget& b,const std::vector<std::size_t>& selected)const{
    auto baseline=vector_draw_history_detail::Manager::ensure(draw_,before,b);auto bound=baseline->bind(post,plane,raw,b,selected);
    b.allocate(sizeof(Edit)+sizeof(Manager));auto edit=std::make_shared<Edit>();edit->draw_=std::move(bound.edit);auto manager=std::make_shared<Manager>(*this);manager->draw_=std::move(bound.manager);manager->state_.reset();return {post,std::move(edit),std::move(manager)};
}
Mirrored Manager::replay(const Edit& edit,const Layer& receiver,Budget& b)const{
    const auto* baseline=baseline_owner();const bool paired=baseline&&owner_==receiver.common.runtime_identity&&family_==receiver.family&&compatible(receiver,*baseline);
    if(paired&&((edit.draw_&&!draw_&&state_)||(!edit.draw_&&!state_&&draw_))){b.allocate(sizeof(Manager));auto current=std::make_shared<Manager>(*this);return ensure(std::move(current),receiver,b,false,bool(edit.draw_))->replay(edit,receiver,b);}
    if(edit.draw_){b.allocate(sizeof(Edit)+sizeof(Manager));auto opposite=std::make_shared<Edit>();auto manager=std::make_shared<Manager>(*this);
        if(draw_){auto result=draw_->replay(*edit.draw_,receiver,b);opposite->draw_=std::move(result.opposite);manager->draw_=std::move(result.manager);return {std::move(result.layer),std::move(opposite),std::move(manager)};}
        auto result=edit.draw_->replay(receiver,b);opposite->draw_=std::move(result.opposite);return {std::move(result.layer),std::move(opposite),std::move(manager)};}
    auto result=edit.replay(receiver,b);b.allocate(sizeof(Manager));auto manager=std::make_shared<Manager>(*this);
    if(state_&&paired){
        manager->draw_.reset();
        const auto& saved=edit.reverse_?*result.opposite->secondary_:*edit.primary_;
        b.allocate(2*archive_storage(state_->content));State target(state_->owner,state_->content,b,false,state_->shape);target.restore(state_->memory);
        apply_packet(target,saved,b,true);manager->state_=remember(target,state_->owner,b);
    }
    b.visit();return {std::move(result.layer),std::move(result.opposite),std::move(manager)};
}
void Budget::allocate(std::uint64_t n){vector_material_detail::poll(stop);vector_material_detail::charge(bytes,n,max_bytes,"Paint history exceeds512MiB");}
void Budget::visit(std::uint64_t n){vector_material_detail::poll(stop);vector_material_detail::charge(work,n,max_work,"Paint history exceeds128Mi work");}
}
