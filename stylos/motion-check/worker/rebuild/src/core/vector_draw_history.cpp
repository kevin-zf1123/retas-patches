#include "vector_draw_history.hpp"
#include "vector_graph_edit.hpp"
#include <bit>
#include "retas/vector_extension_archive.hpp"
namespace retas::vector_draw_history_detail {
namespace {
using vector_graph_detail::OwnerLists;
bool vert(unsigned f){return f==0x21210001u||f==0x21220001u;}
bool positive(VectorBounds b){return b.left<b.right&&b.top<b.bottom;}
bool contained(VectorBounds a,VectorBounds b){return a.left<=b.left&&a.top<=b.top&&a.right>=b.right&&a.bottom>=b.bottom;}
int reference(int id){return id<0?-1:id;}
template<class T>void remove_first(std::vector<T>& a,const T& v){const auto i=std::ranges::find(a,v);if(i!=a.end())a.erase(i);}
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
        if(saved.high!=slots.high||saved.cursor!=slots.cursor)throw std::invalid_argument("Paint baseline allocator state changed");
        for(const auto& r:saved.retired){budget.visit();budget.allocate(value_storage(r.value)+r.suffix.size());auto& c=cell(r.index);
            if(c.active)throw std::invalid_argument("Paint baseline retired identity is active");c.value=r.value;c.suffix=r.suffix;}
    }
};
using Memory=vector_paint_history_detail::CacheMemory;
struct Child {
    std::uint64_t identity{};unsigned family{};std::optional<OwnerLists> lists;
};
std::vector<Child> children(const Layer& owner,Budget& b){
    if(owner.planes.size()>1024)throw std::length_error("VecDraw child limit");b.allocate(owner.planes.size()*sizeof(Child));std::vector<Child> result;
    std::array<std::set<int>,3> owned;std::size_t references{};
    for(const auto& p:owner.planes){b.visit();Child c{p.common.runtime_identity,p.family,{}};if(vert(p.family)){if(p.opaque||p.raster)throw std::invalid_argument("VecDraw requires decoded Vert topology");b.allocate(p.specific.size()*2);c.lists.emplace(p,nullptr);
            const std::array<const std::vector<int>*,3> lists{&c.lists->vertices,&c.lists->models,&c.lists->lines};for(unsigned i=0;i<3;++i)for(int id:*lists[i]){if(id<0||!owned[i].insert(id).second)throw std::invalid_argument("VecDraw competing topology ownership");++references;}}
        result.push_back(std::move(c));}
    if(references>8192)throw std::length_error("VecDraw child reference limit");return result;
}
struct Content {
    VectorLineArchive line;
    std::optional<VectorPaintArchive> paint;
    std::optional<VectorShapeArchive> shape;
};
Content decode(const Layer& owner,Budget& b){
    if(owner.opaque||(owner.family!=0x22100000u&&owner.family!=0x24400000u&&owner.family!=0x21200000u&&owner.family!=0x20800000u&&owner.family!=0x40002000u&&owner.family!=0x20000200u))throw std::invalid_argument("VecDraw requires a native Vector owner");
    std::uint64_t wire=owner.specific.size();for(const auto& p:owner.planes)wire+=p.specific.size();if(wire>8ull*1024*1024)throw std::length_error("VecDraw wire limit");b.allocate(32*wire+8192+vector_runtime_detail::face_colors_storage(owner));b.visit(16*wire+1);
    Content content;if(owner.family==0x40002000u){content.shape=decode_vector_shape_archive(owner);content.paint=std::move(content.shape->paint);content.shape->paint={};}else if(owner.family==0x21200000u||owner.family==0x20800000u)content.paint=decode_vector_paint_archive(owner);
    if(content.paint){content.line=std::move(content.paint->base);content.paint->base={};}else content.line=decode_vector_line_archive(owner);
    const auto& a=content.line;auto records=a.vertices.size()+a.scales.size()+a.subcurves.size()+a.models.size()+a.crosspoints.size()+a.lines.size();std::uint64_t refs{};
    for(const auto& r:a.vertices)refs+=r.value.subcurves.size();for(const auto& r:a.subcurves)refs+=r.value.scales.size()+3;for(const auto& r:a.models)refs+=r.value.subcurves.size()+5;for(const auto& r:a.lines)refs+=r.value.members.size();
    if(content.paint){const auto& p=*content.paint;records+=p.loops.size()+p.regions.size()+p.faces.size()+p.gradnodes.size()+p.gradations.size()+p.textures.size();refs+=p.border.vertices.size()+p.border.models.size()+p.border.regions.size();for(const auto& l:p.loops)refs+=l.value.members.size()+1;for(const auto& r:p.regions)refs+=r.value.outer.members.size()+r.value.holes.size()+r.value.faces.size();}if(content.shape)records+=content.shape->shapes.size();
    if(records>4096||refs>8192)throw std::length_error("VecDraw record/reference limit");return content;
}
std::uint64_t content_storage(const Content& c){
    const auto& a=c.line;std::uint64_t size=sizeof(c)+pool_storage(a.vertices)+pool_storage(a.scales)+pool_storage(a.subcurves)+pool_storage(a.models)+pool_storage(a.crosspoints)+pool_storage(a.lines)+a.header_suffix.size()+a.body_suffix.size()+a.layer_suffix.size();
    if(c.paint){const auto& p=*c.paint;size+=pool_storage(p.loops)+pool_storage(p.regions)+pool_storage(p.faces)+pool_storage(p.gradnodes)+pool_storage(p.gradations)+pool_storage(p.textures)+p.face_header_suffix.size()+p.face_body_suffix.size()+p.core_suffix.size()+p.outer_suffix.size()+4*(p.border.vertices.size()+p.border.models.size()+p.border.regions.size())+p.border.topology_suffix.size()+p.border.specific_suffix.size()+p.border.record_suffix.size()+p.border.common.name.size()+p.border.common.suffix.size();}
    if(c.shape)size+=pool_storage(c.shape->shapes)+c.shape->header_suffix.size()+c.shape->body_suffix.size();return size;
}
void store(Layer& owner,Content content){if(content.paint){content.paint->base=std::move(content.line);if(content.shape){content.shape->paint=std::move(*content.paint);store_vector_shape_archive(owner,*content.shape);}else store_vector_paint_archive(owner,*content.paint);}else owner.specific=encode_vector_line_archive(content.line);}
struct State {
    VectorLineArchive envelope;std::optional<VectorPaintArchive> paint;std::optional<VectorShapeArchive> shape;std::array<std::optional<int>,6> extra_high{};std::vector<Child> child;Budget& b;
    Memory retained;bool extension_memory_restored{};Pool<VectorVertex> vertices;Pool<VectorLineScale> scales;Pool<VectorSubcurve> subs;Pool<VectorModelCurve> models;Pool<VectorCrosspoint> crosses;Pool<VectorLine> lines;
    State(const Layer& owner,Content a,Budget& budget,bool fresh=false):envelope(std::move(a.line)),paint(std::move(a.paint)),shape(std::move(a.shape)),child(children(owner,budget)),b(budget),
        vertices(envelope.vertices,fresh?std::nullopt:owner.runtime_vector_high_water[0],b),scales(envelope.scales,fresh?std::nullopt:owner.runtime_vector_high_water[1],b),subs(envelope.subcurves,fresh?std::nullopt:owner.runtime_vector_high_water[2],b),models(envelope.models,fresh?std::nullopt:owner.runtime_vector_high_water[3],b),crosses(envelope.crosspoints,fresh?std::nullopt:owner.runtime_vector_high_water[4],b),lines(envelope.lines,fresh?std::nullopt:owner.runtime_vector_high_water[5],b){for(std::size_t i=0;i<6;++i)extra_high[i]=fresh?std::optional<int>{}:owner.runtime_vector_high_water[i+6];if(fresh&&(owner.family==0x21200000u||owner.family==0x20800000u||owner.family==0x40002000u))paint.emplace();if(fresh&&owner.family==0x40002000u)shape.emplace();if(fresh)for(auto& c:child)if(c.lists){c.lists->vertices.clear();c.lists->models.clear();c.lists->lines.clear();}}
    auto pools(){return std::tie(vertices,scales,subs,models,crosses,lines);}
    template<std::size_t... I>void restore(const Memory& m,std::index_sequence<I...>){auto p=pools();(std::get<I>(p).restore(std::get<I>(m)),...);}
    template<std::size_t... I>void retain_extensions(const Memory& m,std::index_sequence<I...>){((b.allocate(pool_storage(std::get<I+6>(m).retired)),std::get<I+6>(retained)=std::get<I+6>(m)),...);extension_memory_restored=true;}
    void restore(const Memory& m){restore(m,std::make_index_sequence<6>{});retain_extensions(m,std::make_index_sequence<6>{});}
    template<std::size_t... I>void copy_extensions(Memory& m,std::index_sequence<I...>){((b.allocate(pool_storage(std::get<I+6>(retained).retired)),std::get<I+6>(m)=std::get<I+6>(retained)),...);}
    Memory remember(){std::size_t count{};std::apply([&](const auto&... p){((count+=p.cells.size()),...);},pools());std::apply([&](const auto&... p){((count+=p.retired.size()),...);},retained);if(paint)count+=paint->loops.size()+paint->regions.size()+paint->faces.size()+paint->gradnodes.size()+paint->gradations.size()+paint->textures.size();if(shape)count+=shape->shapes.size();if(count>4096)throw std::length_error("VecDraw retired backing limit");Memory result;std::get<0>(result)=vertices.remember();std::get<1>(result)=scales.remember();std::get<2>(result)=subs.remember();std::get<3>(result)=models.remember();std::get<4>(result)=crosses.remember();std::get<5>(result)=lines.remember();copy_extensions(result,std::make_index_sequence<6>{});return result;}
    void remove_vertex(int id){for(auto& c:child)if(c.lists)remove_first(c.lists->vertices,id);}
    void endpoint(int id,int sc){auto& v=vertices.value(id);const auto n=v.subcurves.size();remove_first(v.subcurves,sc);if(n!=v.subcurves.size()&&v.subcurves.empty()){vertices.retire(id,b);remove_vertex(id);}}
    void free_model(int id){if(!models.active(id))return;models.retire(id,b);auto& m=models.value(id);const auto chain=m.subcurves;
        for(unsigned side=0;side<2;++side)if(m.crosspoints[side]>=0){crosses.retire(m.crosspoints[side],b);m.flags&=~(0x10u<<side);m.crosspoints[side]=-1;}
        if(!chain.empty()){endpoint(subs.value(chain.front()).vertex_a,chain.front());endpoint(subs.value(chain.back()).vertex_b,chain.back());for(std::size_t i=0;i+1<chain.size();++i){const int id=subs.value(chain[i]).vertex_b;vertices.retire(id,b);vertices.value(id).subcurves.clear();remove_vertex(id);}}
        for(int sc:chain){const auto widths=subs.value(sc).scales;subs.retire(sc,b);subs.value(sc).scales.clear();for(int width:widths)scales.retire(width,b);}m.subcurves.clear();if(!chain.empty())m.scalars={};for(auto& c:child)if(c.lists)remove_first(c.lists->models,id);
    }
    void free_line(int id,bool deep){if(!lines.active(id))return;const auto members=lines.value(id).members;if(deep)for(auto member:members)free_model(member.model);lines.retire(id,b);auto& l=lines.value(id);for(auto member:members)models.value(member.model).line=-1;l.members.clear();l.archived_bounds={};for(auto& c:child)if(c.lists)remove_first(c.lists->lines,id);}
    void clear(std::optional<std::size_t> plane,std::optional<VectorBounds> window){
        for(std::size_t p=0;p<child.size();++p){if(!child[p].lists||(plane&&p!=*plane))continue;auto& list=*child[p].lists;
            if(window){const auto ids=list.lines;for(int id:ids)if(contained(*window,lines.value(id).archived_bounds))free_line(id,true);}
            else{const auto ms=list.models;for(int id:ms)free_model(id);const auto vs=list.vertices;for(int id:vs){vertices.retire(id,b);vertices.value(id).subcurves.clear();}list.vertices.clear();const auto ls=list.lines;for(int id:ls)free_line(id,false);}
        }
    }
    VectorLineArchive line_packet(){auto a=envelope;b.allocate(vertices.storage()+scales.storage()+subs.storage()+models.storage()+crosses.storage()+lines.storage());a.vertices=vertices.output();a.scales=scales.output();a.subcurves=subs.output();a.models=models.output();a.crosspoints=crosses.output();a.lines=lines.output();std::size_t i{};std::apply([&](const auto&... p){((p.touched?void(a.high_water[i++]=p.slots.high):void(++i)),...);},pools());return a;}
    Content packet(){return {line_packet(),paint,shape};}
    Layer carrier(const Layer& live){auto layer=live;std::size_t i{};std::apply([&](const auto&... p){((p.touched?void(layer.runtime_vector_high_water[i++]=p.slots.high):void(++i)),...);},pools());for(std::size_t p=0;p<child.size();++p)if(child[p].lists)child[p].lists->publish(layer.planes[p],nullptr);for(std::size_t i=0;i<6;++i)if(extra_high[i])layer.runtime_vector_high_water[i+6]=extra_high[i];return layer;}
    Layer finish(const Layer& live){auto result=carrier(live);store(result,packet());(void)decode(result,b);(void)children(result,b);return result;}
};
struct Copier {
    State& d;const Content& content;const VectorLineArchive& s;Budget& b;OwnerLists* topology{};
    Copier(State& target,const Content& source,Budget& budget):d(target),content(source),s(source.line),b(budget){}
    template<class T>const T& read(const std::vector<VectorPoolRecord<T>>& p,int id){b.visit(p.size()+1);return vector_graph_detail::record(const_cast<std::vector<VectorPoolRecord<T>>&>(p),id).value;}
    void vertex(int id,bool adjacency){const auto& source=read(s.vertices,id);const bool fresh=d.vertices.activate(id,b);auto& target=d.vertices.value(id);if(fresh){target.x=source.x;target.y=source.y;if(topology)topology->vertices.push_back(id);}if(fresh||adjacency){target.subcurves=source.subcurves;for(int& id:target.subcurves)id=reference(id);}}
    void scale(int id){d.scales.activate(id,b);d.scales.value(id)=read(s.scales,id);}
    void sub(int id,bool deep){d.subs.activate(id,b);const auto& source=read(s.subcurves,id);auto& target=d.subs.value(id);target=source;target.vertex_a=reference(target.vertex_a);target.vertex_b=reference(target.vertex_b);target.model=reference(target.model);if(deep){vertex(source.vertex_a,false);vertex(source.vertex_b,false);for(int width:source.scales)scale(width);}}
    void cross(int id){d.crosses.activate(id,b);d.crosses.value(id)=read(s.crosspoints,id);}
    void model(int id,bool deep){const bool fresh=d.models.activate(id,b);if(fresh&&topology)topology->models.push_back(id);const auto& source=read(s.models,id);auto& target=d.models.value(id);target.flags=source.flags&0xffffu;target.subcurves=source.subcurves;target.line=reference(source.line);target.scalars=source.scalars;target.crosspoints=source.crosspoints;for(int& id:target.crosspoints)id=reference(id);for(int side=0;side<2;++side)if(source.targets[side]>=0){target.targets[side]=source.targets[side];target.target_kinds[side]=source.target_kinds[side];}
        if(deep){for(int sc:source.subcurves)sub(sc,true);for(int cp:source.crosspoints)if(cp>=0)cross(cp);if(!source.subcurves.empty()){vertex(read(s.subcurves,source.subcurves.front()).vertex_a,true);vertex(read(s.subcurves,source.subcurves.back()).vertex_b,true);}}}
    void line(int id,bool deep){d.lines.activate(id,b);const auto& source=read(s.lines,id);if(deep)for(auto member:source.members)model(member.model,true);d.lines.value(id)=source;for(auto member:source.members)d.models.value(member.model).line=id;}
    template<class T,class F>void ascending(const std::vector<VectorPoolRecord<T>>& p,F f){std::vector<int> ids;for(const auto& r:p)ids.push_back(r.index);std::ranges::sort(ids);for(int id:ids){b.visit();f(id);}}
    void extensions(){
        if(!content.paint)return;if(!d.paint)throw std::invalid_argument("VecDraw extension family mismatch");auto destination=*d.paint,source=*content.paint;destination.base=d.line_packet();source.base=s;
        Layer carrier;carrier.family=0x21200000u;auto memory=d.remember();const auto initialize=[&](std::size_t i,auto& saved,const auto& records){if(!d.extension_memory_restored){saved.high=d.extra_high[i].value_or(destination.face_high_water[i]);std::set<int> active;for(const auto& r:records)active.insert(r.index);saved.cursor=0;while(active.contains(saved.cursor)){b.visit();++saved.cursor;}}};initialize(0,std::get<6>(memory),destination.loops);initialize(1,std::get<7>(memory),destination.regions);initialize(2,std::get<8>(memory),destination.faces);initialize(3,std::get<9>(memory),destination.gradnodes);initialize(4,std::get<10>(memory),destination.gradations);initialize(5,std::get<11>(memory),destination.textures);std::size_t pool_index{};std::apply([&](const auto&... p){((carrier.runtime_vector_high_water[pool_index++]=p.high),...);},memory);
        auto copied=vector_paint_history_detail::copy_draw_extensions(carrier,std::move(destination),std::move(source),b,&memory);d.retain_extensions(memory,std::make_index_sequence<6>{});
        for(const auto& r:std::get<3>(memory).retired){auto& target=d.models.value(r.index);target.targets=r.value.targets;target.target_kinds=r.value.target_kinds;}
        for(const auto& m:copied.base.models){auto& target=d.models.value(m.index);target.targets=m.value.targets;target.target_kinds=m.value.target_kinds;}copied.base={};d.extra_high={std::get<6>(memory).high,std::get<7>(memory).high,std::get<8>(memory).high,std::get<9>(memory).high,std::get<10>(memory).high,std::get<11>(memory).high};d.paint=std::move(copied);
        if(content.shape){if(!d.shape)throw std::invalid_argument("VecDraw Shape family mismatch");for(const auto& record:content.shape->shapes){b.visit(d.shape->shapes.size()+1);auto i=std::ranges::find(d.shape->shapes,record.index,&VectorPoolRecord<VectorArchiveShape>::index);if(i==d.shape->shapes.end()){b.allocate(sizeof(record));d.shape->shapes.push_back({record.index,{},{}});i=d.shape->shapes.end()-1;d.shape->high_water=std::max(d.shape->high_water,record.index);}i->value=record.value;i->value.line=reference(i->value.line);i->value.region=reference(i->value.region);i->value.flags&=0xffffu;}std::ranges::sort(d.shape->shapes,{},&VectorPoolRecord<VectorArchiveShape>::index);}
    }
    void full(const std::vector<Child>& child){ascending(s.vertices,[&](int id){vertex(id,true);});ascending(s.scales,[&](int id){scale(id);});ascending(s.subcurves,[&](int id){sub(id,false);});ascending(s.models,[&](int id){model(id,false);});ascending(s.crosspoints,[&](int id){cross(id);});ascending(s.lines,[&](int id){line(id,false);});
        for(std::size_t p=0;p<child.size();++p)if(child[p].lists){auto& out=*d.child[p].lists;const auto& in=*child[p].lists;for(int id:in.vertices)if(std::ranges::find(out.vertices,id)==out.vertices.end())out.vertices.push_back(id);out.models.insert(out.models.end(),in.models.begin(),in.models.end());out.lines.insert(out.lines.end(),in.lines.begin(),in.lines.end());}extensions();}
    void selected(const std::vector<Child>& child,const std::vector<std::size_t>& planes,std::optional<VectorBounds> window){
        std::set<std::size_t> seen;
        for(auto plane:planes){b.visit();if(plane>=child.size()||!child[plane].lists||!seen.insert(plane).second)throw std::invalid_argument("VecDraw selected history requires unique Vert children");
            auto& out=*d.child[plane].lists;topology=&out;
            //646550 walks LC membership even when the rectangle is NULL.
            for(int id:child[plane].lists->lines)if(!window||contained(*window,read(s.lines,id).archived_bounds)){line(id,true);out.lines.push_back(id);}
            topology=nullptr;
        }
    }
    void partial(const std::vector<Child>& child,std::size_t plane,VectorBounds window){if(plane>=child.size()||!child[plane].lists)throw std::invalid_argument("VecDraw ROI requires a Vert child");auto& out=*d.child[plane].lists;
        topology=&out;for(int id:child[plane].lists->lines)if(contained(window,read(s.lines,id).archived_bounds)){line(id,true);out.lines.push_back(id);}topology=nullptr;}
};
}
struct Snapshot {std::vector<std::size_t> selected;Content content;std::vector<Child> child;std::optional<std::size_t> plane;std::optional<VectorBounds> window;std::uint64_t owner{};};
struct ManagerState{Layer owner;Content content;Memory memory;};
const Layer* Manager::baseline_owner()const noexcept{return state_?&state_->owner:nullptr;}
vector_paint_history_detail::Cache Manager::cache(Budget& b)const{
    if(!state_||!state_->content.paint)throw std::logic_error("Missing extended Draw baseline");b.allocate(8192+4*content_storage(state_->content));std::apply([&](const auto&... p){(b.allocate(pool_storage(p.retired)),...);},state_->memory);
    vector_paint_history_detail::Cache result;result.owner=state_->owner;result.content=*state_->content.paint;result.content.base=state_->content.line;result.shape=state_->content.shape;result.memory=state_->memory;return result;
}
std::shared_ptr<const Manager> Manager::from_cache(vector_paint_history_detail::Cache cache,Budget& b){
    b.allocate(sizeof(Manager)+sizeof(ManagerState));auto state=std::make_shared<ManagerState>();state->owner=std::move(cache.owner);state->content.line=std::move(cache.content.base);cache.content.base={};state->content.paint=std::move(cache.content);state->content.shape=std::move(cache.shape);state->memory=std::move(cache.memory);auto result=std::make_shared<Manager>();result->state_=std::move(state);return result;
}
namespace {
std::shared_ptr<const Snapshot> capture(const Layer& owner,const Content& source,std::optional<std::size_t> plane,std::optional<VectorBounds> window,Budget& b,const std::vector<std::size_t>& selected={}){
    b.allocate(sizeof(Snapshot)+selected.size()*sizeof(std::size_t));auto result=std::make_shared<Snapshot>();result->owner=owner.common.runtime_identity;result->plane=plane;result->window=window;result->selected=selected;auto child=children(owner,b);State fresh(owner,{},b,true);Copier copy{fresh,source,b};if(!selected.empty())copy.selected(child,selected,window);else if(window&&plane)copy.partial(child,*plane,*window);else copy.full(child);result->content=fresh.packet();result->child=std::move(fresh.child);return result;
}
void apply(State& target,const Snapshot& saved,const Layer& receiver,Budget& b){
    if(saved.owner!=receiver.common.runtime_identity||saved.child.size()!=target.child.size())throw std::invalid_argument("VecDraw owner membership changed");std::vector<Child> mapped=target.child;std::optional<std::size_t> plane;std::vector<std::size_t> selected;
    std::set<std::size_t> used;for(std::size_t p=0;p<target.child.size();++p){std::optional<std::size_t> found;for(std::size_t q=0;q<saved.child.size();++q)if(target.child[p].identity==saved.child[q].identity&&target.child[p].family==saved.child[q].family){if(found)throw std::invalid_argument("VecDraw ambiguous child identity");found=q;}if(!found||!used.insert(*found).second)throw std::invalid_argument("VecDraw missing child identity");mapped[p]=saved.child[*found];if(saved.plane&&*saved.plane==*found)plane=p;if(std::ranges::find(saved.selected,*found)!=saved.selected.end())selected.push_back(p);}
    if(selected.empty())target.clear(plane,saved.window);else for(auto p:selected)target.clear(p,saved.window);Copier copy{target,saved.content,b};copy.full(mapped);
    for(std::size_t p=0;p<mapped.size();++p)if(mapped[p].lists&&(!plane||p==*plane)&&(selected.empty()||std::ranges::find(selected,p)!=selected.end()))for(int id:target.child[p].lists->lines)target.lines.value(id).color=receiver.planes[p].common.color;
}
std::shared_ptr<const ManagerState> remember(State& state,const Layer& live,Budget& b){b.allocate(sizeof(ManagerState));auto result=std::make_shared<ManagerState>();result->owner=state.carrier(live);result->owner.specific.clear();result->memory=state.remember();result->content=state.packet();return result;}
}
Prepared Edit::replay(const Layer& receiver,Budget& b)const{if(!primary_)throw std::logic_error("Unbound VecDraw history");auto source=decode(receiver,b);b.allocate(sizeof(Edit));auto opposite=std::make_shared<Edit>(*this);if(!opposite->secondary_){std::optional<std::size_t> plane;if(primary_->plane){const auto& key=primary_->child.at(*primary_->plane);for(std::size_t p=0;p<receiver.planes.size();++p)if(receiver.planes[p].common.runtime_identity==key.identity&&receiver.planes[p].family==key.family){if(plane)throw std::invalid_argument("VecDraw ambiguous ROI child");plane=p;}if(!plane)throw std::invalid_argument("VecDraw missing ROI child");}std::vector<std::size_t> selected;for(auto old:primary_->selected){const auto& key=primary_->child.at(old);std::optional<std::size_t> found;for(std::size_t p=0;p<receiver.planes.size();++p)if(receiver.planes[p].common.runtime_identity==key.identity&&receiver.planes[p].family==key.family){if(found)throw std::invalid_argument("VecDraw ambiguous selected child");found=p;}if(!found)throw std::invalid_argument("VecDraw missing selected child");selected.push_back(*found);}opposite->secondary_=capture(receiver,source,plane,primary_->window,b,selected);}const auto& saved=reverse_?*opposite->secondary_:*primary_;State target(receiver,std::move(source),b);apply(target,saved,receiver,b);auto layer=target.finish(receiver);opposite->reverse_=!reverse_;return {std::move(layer),std::move(opposite)};}
std::shared_ptr<const Manager> Manager::ensure(std::shared_ptr<const Manager> previous,const Layer& owner,Budget& b,bool force){if(previous&&!force&&previous->state_->owner.common.runtime_identity==owner.common.runtime_identity&&previous->state_->owner.family==owner.family)return previous;
    auto source=decode(owner,b);auto child=children(owner,b);auto carrier=owner;carrier.runtime_vector_high_water.fill(std::nullopt);State target(carrier,{},b,true);if(source.paint){b.allocate(content_storage(source));target.paint->border=source.paint->border;}Copier copy{target,source,b};copy.full(child);b.allocate(sizeof(Manager));auto result=std::make_shared<Manager>();result->state_=remember(target,carrier,b);return result;}
Bound Manager::bind(const Layer& post,std::optional<std::size_t> plane,std::optional<VectorBounds> raw,Budget& b,const std::vector<std::size_t>& selected)const{
    std::optional<VectorBounds> window;if(raw){if(!positive(*raw)||(!plane&&selected.empty()))throw std::invalid_argument("VecDraw ROI requires positive damage and child");const auto edge=[](int n,int d){const auto r=std::int64_t(n)+d;if(r<INT32_MIN||r>INT32_MAX)throw std::overflow_error("VecDraw ROI overflow");return int(r);};window=VectorBounds{edge(raw->left,-16),edge(raw->top,-16),edge(raw->right,16),edge(raw->bottom,16)};}
    b.allocate(sizeof(Edit)+sizeof(Manager));auto edit=std::make_shared<Edit>();edit->primary_=capture(state_->owner,state_->content,plane,window,b,selected);State target(state_->owner,state_->content,b);target.restore(state_->memory);if(selected.empty())target.clear(plane,window);else for(auto p:selected)target.clear(p,window);auto source=decode(post,b);auto child=children(post,b);Copier copy{target,source,b};if(!selected.empty())copy.selected(child,selected,window);else if(window&&plane)copy.partial(child,*plane,*window);else copy.full(child);auto result=std::make_shared<Manager>();result->state_=remember(target,state_->owner,b);return {std::move(edit),std::move(result)};
}
Mirrored Manager::replay(const Edit& edit,const Layer& receiver,Budget& b)const{auto result=edit.replay(receiver,b);b.allocate(sizeof(Manager));auto manager=std::make_shared<Manager>(*this);
    bool paired=state_&&state_->owner.common.runtime_identity==receiver.common.runtime_identity&&state_->owner.family==receiver.family&&state_->owner.planes.size()==receiver.planes.size();if(paired)for(std::size_t p=0;p<receiver.planes.size();++p)paired=paired&&state_->owner.planes[p].family==receiver.planes[p].family;
    if(paired){const auto& saved=edit.reverse_?*result.opposite->secondary_:*edit.primary_;State target(state_->owner,state_->content,b);target.restore(state_->memory);apply(target,saved,state_->owner,b);manager->state_=remember(target,state_->owner,b);}return {std::move(result.layer),std::move(result.opposite),std::move(manager)};}
}
