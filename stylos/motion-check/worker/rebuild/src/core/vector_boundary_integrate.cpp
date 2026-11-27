#include "vector_boundary_integrate.hpp"
#include "vector_fill_boundary.hpp"
#include "vector_line_detach.hpp"
#include <bit>
#include <set>
namespace retas::vector_boundary_detail {
namespace {
using namespace vector_probe_detail;
using vector_graph_detail::Editor;
using numeric_detail::binary64;
bool positive(VectorBounds b){return b.left<b.right&&b.top<b.bottom;}
bool encloses(VectorBounds a,VectorBounds b){return a.left<=b.left&&a.top<=b.top&&a.right>=b.right&&a.bottom>=b.bottom;}
template<class T>const T& get(const std::vector<VectorPoolRecord<T>>& pool,int id,Budget b){b.charge(pool.size()+1);const auto i=std::ranges::find(pool,id,&VectorPoolRecord<T>::index);if(i==pool.end())throw std::invalid_argument("Region integration dead identity");return i->value;}
std::vector<float> roots(const Curve& c,bool horizontal,double query,Budget b){
    b.charge(32);const auto coordinate=[&](VectorFaceVertex v){return horizontal?v.y:v.x;};const double first=coordinate(c.start),control=coordinate(c.control),last=coordinate(c.end),q=binary64(query+.01);std::array<double,2> raw{};int count{};
    if(c.linear){const auto d=binary64(last-first);if(d==0){if(first==query){raw={0,1};count=2;}}else{raw[0]=binary64(binary64(q-first)/d);count=1;}}
    else{const auto a=binary64(binary64(last-2*control)+first),z=binary64(2*binary64(control-first)),k=binary64(first-q);if(a==0){if(z!=0){raw[0]=binary64(-k/z);count=1;}}else{const auto d=binary64(binary64(z*z)-binary64(4*binary64(a*k)));if(d>=0){if(d==0)raw[0]=raw[1]=binary64(-z/binary64(a+a));else{const auto square=binary64(std::sqrt(d)),scale=binary64(.5/a);raw={binary64(binary64(square-z)*scale),binary64(binary64(-z-square)*scale)};}count=2;}}}
    std::vector<float> out;for(int i=0;i<count;++i)if(raw[i]>=0&&raw[i]<=1)out.push_back(float(raw[i]));return out;
}
int classify_double(const VectorArchiveLoop& loop,VectorPoint p,const Graph& graph,Budget b){
    b.charge();const auto x=std::floor(p.x),y=std::floor(p.y);const auto box=loop.archived_bounds;if(x<box.left||x>=box.right||y<box.top||y>=box.bottom)return 0;
    const auto ray=[&](bool horizontal){std::vector<double> hits;const double query=horizontal?p.y:p.x;
        for(auto member:loop.members){b.charge();const auto& m=*graph.models.at(member.model);const auto& bounds=m.scalars;if(horizontal?(query<bounds[1]||query>=bounds[3]):(query<bounds[0]||query>=bounds[2]))continue;
            for(int id:m.subcurves){const auto c=graph.curve(m,*graph.subcurves.at(id),b);auto low=std::min(horizontal?c.start.y:c.start.x,horizontal?c.end.y:c.end.x),high=std::max(horizontal?c.start.y:c.start.x,horizontal?c.end.y:c.end.x);if(!c.linear){low=std::min(low,horizontal?c.control.y:c.control.x);high=std::max(high,horizontal?c.control.y:c.control.x);}if(query<low||query>=double(high)+1)continue;
                for(auto t:roots(c,horizontal,query,b)){const auto point=position(c,t,b);hits.push_back(horizontal?point.x:point.y);}}
        }return hits;};
    const auto horizontal=ray(true);if(horizontal.empty())return 0;const bool odd=horizontal.size()%2;if(!odd&&std::ranges::find(horizontal,p.x)==horizontal.end())return std::ranges::count_if(horizontal,[&](double n){return n<p.x;})%2?1:0;
    const auto vertical=ray(false);if(vertical.empty())return 0;if(odd||std::ranges::find(vertical,p.y)!=vertical.end())return 2;return std::ranges::count_if(vertical,[&](double n){return n<p.y;})%2?1:0;
}
struct LoopKey {bool embedded{};int id{-1};auto operator<=>(const LoopKey&)const=default;};
void same_page(const std::set<int>& ids,unsigned shift){if(ids.empty())return;const int page=*ids.begin()>>shift;for(int id:ids)if(id<0||id>>shift!=page)throw std::invalid_argument("Region integration cross-page native pointer order is unsupported");}
}
bool region_contains_double(const VectorPaintArchive& a,int id,VectorPoint point,Budget b){
    b.charge();if(!std::isfinite(point.x)||!std::isfinite(point.y)||std::abs(point.x)>1073741824||std::abs(point.y)>1073741824)throw std::invalid_argument("Region double query exceeds raw domain");const auto& r=get(a.regions,id,b);Graph graph(a.base,b);
    if(positive(r.outer.archived_bounds)&&!classify_double(r.outer,point,graph,b))return false;for(int hole:r.holes)if(classify_double(get(a.loops,hole,b),point,graph,b))return false;return true;
}
struct Integrator::State {
    Editor& e;VectorPaintArchive& a;Budget b;
    std::array<std::optional<vector_material_detail::VectorSlots>,6> slots;
    std::map<std::size_t,vector_material_detail::PaintLists> paints;std::map<int,std::size_t> face_owners;VectorBounds dirty;
    std::map<int,int> region_next;
    static VectorPaintArchive& required(Editor& e){(void)Editor::border(e.paint,e.a);return *e.paint;}
    explicit State(Editor& editor):e(editor),a(required(editor)),b(editor.budget){b.charge();if(!e.owner.border_owner)throw std::invalid_argument("Region integrator requires Border ownership");reload_paints();sync_region_links();}
    void sync_region_links(){for(std::size_t i=0;i<a.border.regions.size();++i)region_next[a.border.regions[i]]=i+1<a.border.regions.size()?a.border.regions[i+1]:-1;}
    void reload_paints(){paints.clear();face_owners.clear();for(std::size_t i=0;i<e.layer.planes.size();++i){b.charge();const auto family=e.layer.planes[i].family;if(family!=0x22100001&&family!=0x22100002)continue;auto& list=paints.emplace(i,vector_material_detail::PaintLists(e.layer.planes[i])).first->second;for(int id:list.ids[0])if(!face_owners.emplace(id,i).second)throw std::invalid_argument("Region Face has competing Paint owners");}}
    template<class T>T& value(std::vector<VectorPoolRecord<T>>& p,int id){return e.value(p,id);}VectorArchiveRegion& region(int id){return value(a.regions,id);}VectorArchiveLoop& loop(LoopKey k){return k.embedded?region(k.id).outer:value(a.loops,k.id);}
    LoopKey lane(int id,int side){const auto& m=e.model(id);if(m.targets[side]<0)throw std::invalid_argument("Region integration requires live side Loop");return {m.target_kinds[side]!=0,m.targets[side]};}
    template<class T>auto& allocator(std::vector<VectorPoolRecord<T>>& p,unsigned kind){auto& s=slots.at(kind-6);if(!s)s.emplace(p,b.work,b.stop,e.layer.runtime_vector_high_water[kind]);return *s;}
    template<class T>void publish_pool(std::vector<VectorPoolRecord<T>>& p,unsigned kind){b.charge(32*p.size());std::ranges::sort(p,{},&VectorPoolRecord<T>::index);a.face_high_water[kind-6]=slots[kind-6]->high;e.layer.runtime_vector_high_water[kind]=slots[kind-6]->high;}
    template<class T>int create(std::vector<VectorPoolRecord<T>>& p,unsigned kind,T v){auto& alloc=allocator(p,kind);vector_material_detail::charge(e.wire,128,8ull*1024*1024,"Region integration wire limit");vector_material_detail::charge(e.nodes,1,4096,"Region integration record limit");vector_material_detail::charge(e.storage,512,512ull*1024*1024,"Region integration storage limit");const int id=alloc.allocate(b.work,b.stop);p.push_back({id,std::move(v),{}});publish_pool(p,kind);e.topology_changed=true;return id;}
    template<class T>void erase(std::vector<VectorPoolRecord<T>>& p,unsigned kind,int id){auto& alloc=allocator(p,kind);const auto it=std::ranges::find(p,id,&VectorPoolRecord<T>::index);if(it==p.end())throw std::invalid_argument("Region integration release dead identity");alloc.release(id);p.erase(it);--e.nodes;publish_pool(p,kind);e.topology_changed=true;}
    std::map<int,std::size_t> line_owners(){std::map<int,std::size_t> owners;for(std::size_t i=0;i<e.layer.planes.size();++i){b.charge();const auto& p=e.layer.planes[i];if(p.family!=0x21110001&&p.family!=0x21120001&&p.family!=0x21210001&&p.family!=0x21220001)continue;if(p.opaque)throw std::invalid_argument("Region Line owner is opaque");vector_graph_detail::OwnerLists list(p,&a);for(int id:list.lines){b.charge();(void)e.line(id);if(!owners.emplace(id,i).second)throw std::invalid_argument("Region LC has competing plane owners");}}return owners;}
    VectorBounds cache_loop(LoopKey key){VectorBounds box;for(auto member:loop(key).members){b.charge();const auto& m=e.model(member.model);vector_graph_detail::unite(box,{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]});}loop(key).archived_bounds=box;return box;}
    void publish(){e.owner.publish(a);for(auto& [plane,list]:paints)list.publish(e.layer.planes[plane]);}
    int new_region(){VectorArchiveRegion v;const int id=create(a.regions,7,v);region(id).outer.region=id;a.border.regions.push_back(id);sync_region_links();e.reference();return id;}
    int new_hole(int owner){VectorArchiveLoop v;v.region=owner;const int id=create(a.loops,6,v);region(owner).holes.push_back(id);e.reference();return id;}
    void populate(LoopKey key,const std::vector<VectorLineMember>& members){auto& l=loop(key);l.members=members;l.archived_bounds={};e.reference(members.size());for(auto member:members){auto& m=e.model(member.model);const unsigned side=member.selector!=0;m.target_kinds[side]=key.embedded?1:0;m.targets[side]=key.id;vector_graph_detail::unite(l.archived_bounds,{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]});}vector_graph_detail::unite(dirty,l.archived_bounds);}
    void attach_material(int face,int material,unsigned kind){if(material<0)return;auto& raw=kind==2?value(a.gradations,material):value(a.textures,material);b.charge(4*raw.size()+32);vector_material_detail::charge(e.storage,2*raw.size()+128,512ull*1024*1024,"Region material backlink storage limit");if(kind==2){auto m=decode_vector_gradation(raw);m.faces.push_back(face);raw=encode_vector_gradation(m);}else{auto m=decode_vector_texture(raw);m.faces.push_back(face);raw=encode_vector_texture(m);}e.reference();value(a.faces,face).material=material;}
    int new_face(std::size_t plane,int owner,VectorArchiveFace v){if(!paints.contains(plane))throw std::invalid_argument("Region copy target is not a Paint plane");const int material=v.material;const unsigned kind=v.flags&15;v.region=owner;v.material=-1;if(kind!=1)v.color=0;const int id=create(a.faces,8,v);paints.at(plane).ids[0].push_back(id);region(owner).faces.push_back(id);face_owners.emplace(id,plane);e.reference(2);if(kind==2||kind==4)attach_material(id,material,kind);return id;}
    void free_face(int id){const auto face=value(a.faces,id);const auto plane=face_owners.at(id);Editor::remove(paints.at(plane).ids[0],id);Editor::remove(region(face.region).faces,id);auto detached=face;vector_material_detail::MaterialDetach op{a,paints.at(plane),e.layer,e.nodes,e.refs,e.storage,b.work,b.stop,{},{},false};op.detach(id,detached);for(unsigned i=3;i<6;++i)slots[i].reset();erase(a.faces,8,id);face_owners.erase(id);}
    void copy_faces(int source,int target,bool skip,bool remove){const auto faces=region(source).faces;for(int id:faces){b.charge();const auto original=value(a.faces,id);const auto plane=face_owners.at(id);const auto old=region(target).faces;const auto found=std::ranges::find_if(old,[&](int f){return face_owners.at(f)==plane;});if(skip&&found!=old.end())continue;if(!skip&&remove&&found!=old.end())free_face(*found);new_face(plane,target,original);}}
    //5990A0 writes into an existing same-Plane Face, retaining both owner
    //memberships/ID. Attachment replacement precedes the full source flags.
    void overwrite_face(int id,VectorArchiveFace source){
        b.charge();const auto old=value(a.faces,id);const auto plane=face_owners.at(id);const unsigned kind=source.flags&15;
        if(kind==1||((kind==2||kind==4)&&((old.flags&15)!=kind||old.material!=source.material))){
            auto detached=old;vector_material_detail::MaterialDetach op{a,paints.at(plane),e.layer,e.nodes,e.refs,e.storage,b.work,b.stop,{},{},false};op.detach(id,detached);for(unsigned i=3;i<6;++i)slots[i].reset();
            auto& destination=value(a.faces,id);destination.material=-1;
            if(kind==1)destination.color=source.color;else{attach_material(id,source.material,kind);}
        }
        value(a.faces,id).flags=source.flags;e.topology_changed=true;
    }
    int same_plane_face(int owner,std::size_t plane){for(int id:region(owner).faces){b.charge();if(face_owners.at(id)==plane)return id;}return -1;}
    bool color_match(int face,std::uint64_t color){b.charge();const auto& f=value(a.faces,face);return (f.flags&1)&&f.color==color;}
    void masked_faces(int source,int target,std::uint64_t color,bool overwrite){
        //loc5BE415 examines exactly ONE physical target head. The subsequent
        //source walk adds missing Planes; it does not remove other target Faces.
        if(!overwrite&&!region(target).faces.empty()){
            const int head=region(target).faces.front(),old=same_plane_face(source,face_owners.at(head));
            if(old>=0?!color_match(old,color):(color>>48)!=0)free_face(head);
        }
        const auto count=region(source).faces.size();b.charge(32*count+1);vector_material_detail::charge(e.storage,sizeof(int)*count,512ull*1024*1024,"Masked Region source snapshot storage limit");
        const auto faces=region(source).faces;
        for(int id:faces){b.charge();const auto original=value(a.faces,id);const auto plane=face_owners.at(id);const int found=same_plane_face(target,plane);
            if(found<0){if(!overwrite||(color>>48)==0)new_face(plane,target,original);}
            else if(overwrite&&color_match(found,color))overwrite_face(found,original);
        }
    }
    void free_loop(int id){const auto owner=value(a.loops,id).region;Editor::remove(region(owner).holes,id);erase(a.loops,6,id);}
    void free_region(int id){const auto r=region(id);Editor::remove(a.border.regions,id);sync_region_links();for(int h:r.holes)free_loop(h);for(int f:r.faces)free_face(f);erase(a.regions,7,id);}
    VectorBounds region_bounds(int id){const auto& r=region(id);if(!r.outer.members.empty())return r.outer.archived_bounds;VectorBounds box;for(int hole:r.holes)vector_graph_detail::unite(box,value(a.loops,hole).archived_bounds);return box;}
    int origin(const std::vector<VectorLineMember>& members){if(members.empty())throw std::invalid_argument("Region integration empty contour origin");const auto member=members.front();const auto& m=e.model(member.model);if(m.subcurves.empty())throw std::invalid_argument("Region integration empty model");const auto& s=e.sub(member.selector?m.subcurves.front():m.subcurves.back());return member.selector?s.vertex_a:s.vertex_b;}
    VectorFaceVertex point(int id){const auto& v=e.vertex(id);return {v.x,v.y};}
    int find_container(int vertex,const std::vector<int>& candidates,VectorBounds box){const auto p=point(vertex);for(int id:candidates){b.charge();const auto saved=region(id).outer.archived_bounds;if(positive(saved)&&(!(saved.left<=p.x&&p.x<saved.right&&saved.top<=p.y&&p.y<saved.bottom)||!encloses(saved,box)))continue;if(vector_fill_detail::region_contains(a,id,p,b,true))return id;}return -1;}
    struct Group {std::vector<VectorLineMember> members;int first{-1},second{-1};bool positive{};};
    using Retire=std::set<LoopKey>;
    unsigned mask(int side){return side?0x10000000u:0x20000000u;}
    bool visited(int id,int side){return e.model(id).flags&mask(side);}
    int head(int id){return e.sub(e.model(id).subcurves.front()).vertex_a;}
    struct Member {LoopKey loop;std::size_t position;};
    Member locate(LoopKey key,int model,int side){const auto& ms=loop(key).members;const auto i=std::ranges::find_if(ms,[&](auto m){return m.model==model&&bool(m.selector)==bool(side);});if(i==ms.end())throw std::invalid_argument("Region repair missing old member");return {key,std::size_t(i-ms.begin())};}
    Group trace(int model,int side,Retire& retire,RepairState& repair,bool track=true){Group g;if(track)retire.insert(lane(model,side));for(;;){b.charge(128);e.model(model).flags|=mask(side);g.members.push_back({model,side});e.reference();const auto m=e.model(model);const int seed=side?m.subcurves.back():m.subcurves.front();const auto s=e.sub(seed);const int at=side?s.vertex_b:s.vertex_a;const auto adjacent=e.vertex(at).subcurves;const auto degree=adjacent.size();if(degree==0)throw std::invalid_argument("Region repair boundary has empty fan");int next,chosen;
            if(degree==1){next=model;chosen=!side;}else{const int sub=degree==2?(adjacent.front()==seed?adjacent.back():adjacent.front()):vector_fill_detail::boundary_predecessor(a.base,at,seed,b);const auto edge=e.sub(sub);next=edge.model;chosen=sub==e.model(next).subcurves.front()&&edge.vertex_a==at;if(e.model(next).flags&0x02000000){const int old=next;next=repair.redirected_models.at(old);if(head(old)!=head(next))chosen=!chosen;}if(track&&degree>2)retire.insert(lane(next,chosen));}
            if(visited(next,chosen))break;model=next;side=chosen;
        }g.positive=vector_fill_detail::temporary_orientation(a.base,g.members,b);return g;}
    void pair_groups(const OrderedRegionPair& pair,std::vector<Group>& groups,Retire& retire,RepairState& repair){const std::set<int> common(pair.vertices.begin(),pair.vertices.end());const auto start=[&](int first,int second)->std::optional<VectorLineMember>{std::vector<LoopKey> loops{{true,first}};for(int hole:region(first).holes)loops.push_back({false,hole});for(auto key:loops)for(auto member:loop(key).members){b.charge(128);const auto& m=e.model(member.model);if((m.flags&mask(member.selector!=0))||(m.flags&0x03000000))continue;if(!common.contains(origin({member})))continue;Graph graph(a.base,b);const auto c=graph.curve(m,e.sub(m.subcurves.front()),b);if(region_contains_double(a,second,position(c,.5f,b),b))return member;}return {};};
        for(;;){auto first=start(pair.first,pair.second);if(!first)first=start(pair.second,pair.first);if(!first)return;Group g;g.first=pair.first;g.second=pair.second;int model=first->model,side=first->selector;bool special=true;std::optional<Member> member;
            for(;;){b.charge(128);e.model(model).flags|=mask(side);g.members.push_back({model,side});e.reference();const auto m=e.model(model);const int seed=side?m.subcurves.back():m.subcurves.front();const auto edge=e.sub(seed);const int at=side?edge.vertex_b:edge.vertex_a;bool jumped{};int next,chosen;
                if(special||common.contains(at)||std::ranges::find(repair.visited_vertices,at)!=repair.visited_vertices.end()){const int sub=vector_fill_detail::boundary_predecessor(a.base,at,seed,b);const auto sc=e.sub(sub);next=sc.model;chosen=sc.vertex_a==at;jumped=true;member.reset();}
                else{if(!member)member=locate(lane(model,side),model,side);const auto& list=loop(member->loop).members;member->position=(member->position+1)%list.size();const auto v=list[member->position];next=v.model;chosen=v.selector;}
                const auto flags=e.model(next).flags;if(flags&0x01000000)special=true;else if(flags&0x02000000){next=repair.redirected_models.at(next);chosen=head(next)==at;special=true;}else special=false;
                const auto key=lane(next,chosen);if(special||jumped){retire.insert(key);member.reset();}if(!special&&!member)member=locate(key,next,chosen);if(visited(next,chosen))break;model=next;side=chosen;
            }g.positive=vector_fill_detail::temporary_orientation(a.base,g.members,b);groups.push_back(std::move(g));}
    }
    bool raster_connector(int seed,int connector,int gap,std::vector<int>& output,VectorBounds& damage){
        const auto mc=e.model(connector);const int first=e.sub(mc.subcurves.front()).vertex_a,last=e.sub(mc.subcurves.back()).vertex_b;
        Retire retire;std::vector<Group> groups;
        for(int initial:{1,0}){
            if(visited(connector,initial))continue;
            Group group;bool outer{};int model=connector,side=initial;std::optional<Member> member;
            for(;;){
                b.charge(128);e.model(model).flags|=mask(side);group.members.push_back({model,side});e.reference();
                const auto m=e.model(model);const int incoming=side?m.subcurves.back():m.subcurves.front();const auto sc=e.sub(incoming);const int at=side?sc.vertex_b:sc.vertex_a;int next,chosen;
                if(!member||at==first||at==last){
                    const int sub=vector_fill_detail::boundary_predecessor(a.base,at,incoming,b);const auto edge=e.sub(sub);next=edge.model;chosen=edge.vertex_a==at;
                    member.reset();
                    if(next!=connector){const auto key=lane(next,chosen);retire.insert(key);outer|=key.embedded;member=locate(key,next,chosen);}
                }else{
                    const auto& list=loop(member->loop).members;member->position=(member->position+1)%list.size();const auto entry=list[member->position];next=entry.model;chosen=entry.selector;
                }
                if(visited(next,chosen))break;model=next;side=chosen;
            }
            group.positive=outer||vector_fill_detail::temporary_orientation(a.base,group.members,b);groups.push_back(std::move(group));
        }
        const auto bounds=[&](const auto& members){VectorBounds box;for(auto v:members){b.charge();const auto& m=e.model(v.model);vector_graph_detail::unite(box,{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]});}return box;};
        for(const auto& g:groups)if(g.positive){const auto box=bounds(g.members);if(std::int64_t(box.right)-box.left<2ll*gap&&std::int64_t(box.bottom)-box.top<2ll*gap){for(int id:e.owner.models)e.model(id).flags&=0xccffffffu;return false;}}
        dirty={};std::vector<int> all,fresh,holes;
        if(!retire.contains({true,seed})){all.push_back(seed);output.push_back(seed);}
        for(int hole:region(seed).holes)if(!retire.contains({false,hole}))holes.push_back(hole);
        for(const auto& g:groups)if(g.positive){const int id=new_region();populate({true,id},g.members);all.push_back(id);fresh.push_back(id);output.push_back(id);}
        struct Event {int group{-1},hole{-1};VectorBounds box;std::int32_t key;std::size_t order;};std::vector<Event> events;
        const auto append=[&](int group,int hole,VectorBounds box){events.push_back({group,hole,box,std::bit_cast<std::int32_t>((std::uint32_t(box.right)-std::uint32_t(box.left))+(std::uint32_t(box.bottom)-std::uint32_t(box.top))),events.size()});vector_material_detail::charge(e.storage,64,512ull*1024*1024,"Raster connector event storage limit");};
        for(std::size_t i=0;i<groups.size();++i)if(!groups[i].positive)append(int(i),-1,bounds(groups[i].members));
        for(int id:holes)append(-1,id,value(a.loops,id).archived_bounds);
        std::ranges::sort(events,[](const auto& x,const auto& y){return x.key!=y.key?x.key>y.key:x.order>y.order;});
        for(const auto& event:events){b.charge(128);const auto& members=event.hole<0?groups[event.group].members:value(a.loops,event.hole).members;const int at=event.hole<0?head(members.front().model):origin(members);int dest=all.size()==1?all.front():find_container(at,all,event.box);if(dest<0)continue;
            if(event.hole<0)populate({false,new_hole(dest)},members);
            else{const int old=value(a.loops,event.hole).region;if(dest!=old){Editor::remove(region(old).holes,event.hole);region(dest).holes.push_back(event.hole);value(a.loops,event.hole).region=dest;vector_graph_detail::unite(dirty,region_bounds(dest));}}
        }
        for(int id:e.owner.models)e.model(id).flags&=0xccffffffu;
        std::set<int> old_holes,old_regions;for(auto key:retire)(key.embedded?old_regions:old_holes).insert(key.id);same_page(old_holes,6);same_page(old_regions,5);
        for(int id:old_holes)free_loop(id);for(int id:old_regions)free_region(id);publish();vector_graph_detail::unite(damage,dirty);return true;
    }
    VectorBounds missing_sides(const std::vector<int>& models,std::vector<int>& regions){dirty={};std::vector<Group> groups;Retire ignored;RepairState repair;for(int id:models)for(int side:{1,0})if(!visited(id,side)&&e.model(id).targets[side]<0)groups.push_back(trace(id,side,ignored,repair,false));regions.push_back(new_region());for(const auto& g:groups)if(g.positive){const int id=new_region();populate({true,id},g.members);regions.push_back(id);}
        struct Event{std::int32_t key;std::size_t order;};std::vector<Event> events;for(std::size_t i=0;i<groups.size();++i)if(!groups[i].positive){VectorBounds box;for(auto member:groups[i].members){const auto& m=e.model(member.model);vector_graph_detail::unite(box,{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]});}events.push_back({std::bit_cast<std::int32_t>((std::uint32_t(box.right)-std::uint32_t(box.left))+(std::uint32_t(box.bottom)-std::uint32_t(box.top))),i});}
        std::ranges::sort(events,[](auto x,auto y){return x.key!=y.key?x.key>y.key:x.order>y.order;});for(auto event:events){b.charge(128);const auto& g=groups[event.order];const auto p=point(head(g.members.front().model));for(int id:regions)if(vector_fill_detail::region_contains(a,id,p,b,true)){populate({false,new_hole(id)},g.members);break;}}
        for(int id:e.owner.models)e.model(id).flags&=0xccffffffu;publish();return dirty;}
    void free_model(int id){const auto m=e.model(id);if(m.line>=0){auto owners=line_owners();(void)vector_line_detail::detach_model_from_line(e,id,owners);}vector_graph_detail::clear_endpoint(e,id,true);vector_graph_detail::clear_endpoint(e,id,false);for(int sub:m.subcurves){const auto s=e.sub(sub);for(int at:{s.vertex_a,s.vertex_b}){Editor::remove(e.vertex(at).subcurves,sub);if(e.vertex(at).subcurves.empty()){Editor::remove(e.owner.vertices,at);e.erase(a.base.vertices,0,at);}}for(int node:s.scales)e.erase(a.base.scales,1,node);e.erase(a.base.subcurves,2,sub);}Editor::remove(e.owner.models,id);e.erase(a.base.models,3,id);}
    void rerun(const std::vector<int>& all,std::vector<int>* collector=nullptr,bool canonical_page_order=false){std::set<int> collected;std::set<int> pending(all.begin(),all.end());if(!pending.empty()&&*pending.begin()<0)throw std::invalid_argument("Region materializer negative pending identity");if(!canonical_page_order)same_page(pending,5);publish();while(!pending.empty()){b.charge();const int id=*pending.begin();pending.erase(pending.begin());
            //5AAA30 clears the slot bitmap and every member/hole/Face list,
            //but retains page backing. A previously live pending pointer is
            //then an empty5C14D0 no-op. A live reused ID still runs normally.
            b.charge(a.regions.size()+1);if(std::ranges::find(a.regions,id,&VectorPoolRecord<VectorArchiveRegion>::index)==a.regions.end())continue;std::vector<int> related;const auto result=vector_fill_detail::materialize_region(a,e.layer,id,e.wire,e.nodes,e.refs,e.storage,b,&related);e.topology_changed|=result.changed;vector_graph_detail::unite(dirty,result.context_dirty);if(collector)collected.insert(result.region);for(int old:related){pending.erase(old);collected.erase(old);}for(auto& slot:e.slots)slot.reset();for(auto& slot:slots)slot.reset();e.owner.vertices=a.border.vertices;e.owner.models=a.border.models;reload_paints();}if(collector){if(!canonical_page_order)same_page(collected,5);e.reference(collected.size());collector->insert(collector->end(),collected.begin(),collected.end());}}
    VectorBounds commit(RepairState& repair,const std::vector<int>& primary,const std::vector<int>& affected,bool materialize,std::vector<int>* selected,std::vector<int>* output,std::optional<std::uint64_t> frozen_color){dirty=repair.context_dirty;std::vector<Group> groups;Retire retire;
        for(const auto& pair:repair.region_pairs)pair_groups(pair,groups,retire,repair);
        for(auto [old,next]:repair.redirected_models){b.charge();retire.insert(lane(old,0));retire.insert(lane(old,1));for(int side:{1,0})if(!visited(next,side)){auto g=trace(next,side,retire,repair);const int nr=loop(lane(next,side)).region,os=head(old)==head(next)?side:!side,other=loop(lane(old,os)).region;const bool from_primary=std::ranges::find(primary,nr)!=primary.end();g.first=from_primary?nr:other;g.second=from_primary?other:nr;groups.push_back(std::move(g));}}
        for(int vertex:repair.visited_vertices){const auto fan=e.vertex(vertex).subcurves;for(int sub:fan){const int model=e.sub(sub).model;if(e.model(model).flags&0x02000000)continue;for(int side:{1,0})if(!visited(model,side)){auto g=trace(model,side,retire,repair);std::set<int> owners;for(auto member:g.members)owners.insert(loop(lane(member.model,member.selector!=0)).region);same_page(owners,5);const int first=*owners.begin(),second=owners.size()>1?*std::next(owners.begin()):first;const bool from_primary=std::ranges::find(primary,first)!=primary.end();g.first=from_primary?first:second;g.second=from_primary?second:first;groups.push_back(std::move(g));}}}
        const std::set<int> selected_input=selected?std::set<int>(selected->begin(),selected->end()):std::set<int>{};if(selected)selected->clear();std::vector<int> left,right,fresh,all,source_holes,target_holes;
        for(int id:primary){b.charge();const LoopKey outer{true,id};if(region(id).outer.members.empty())retire.insert(outer);else if(!retire.contains(outer)){left.push_back(id);all.push_back(id);const auto& loop=region(id).outer;const int match=find_container(origin(loop.members),affected,loop.archived_bounds);if(match>=0){if(frozen_color)masked_faces(match,id,*frozen_color,false);else copy_faces(match,id,true,false);}if(selected&&selected_input.contains(id))selected->push_back(id);vector_graph_detail::unite(dirty,loop.archived_bounds);}for(int hole:region(id).holes)if(!retire.contains({false,hole}))source_holes.push_back(hole);}
        for(int id:affected){b.charge();if(!retire.contains({true,id})){right.push_back(id);all.push_back(id);if(!region(id).outer.members.empty()){const auto& loop=region(id).outer;const int match=find_container(origin(loop.members),primary,loop.archived_bounds);if(match>=0){if(frozen_color)masked_faces(match,id,*frozen_color,true);else copy_faces(match,id,false,true);if(selected&&selected_input.contains(match))selected->push_back(id);}}}for(int hole:region(id).holes)if(!retire.contains({false,hole}))target_holes.push_back(hole);}
        for(auto& group:groups)if(group.positive){const int id=new_region();populate({true,id},group.members);fresh.push_back(id);all.push_back(id);if(frozen_color){copy_faces(group.second,id,false,false);masked_faces(group.first,id,*frozen_color,true);}else{copy_faces(group.first,id,false,false);copy_faces(group.second,id,true,false);}if(selected&&selected_input.contains(group.first))selected->push_back(id);}
        struct Event {int kind,id;VectorBounds box;std::int32_t key;std::size_t order;};std::vector<Event> events;const auto add=[&](int kind,int id,VectorBounds box){b.charge();vector_material_detail::charge(e.storage,64,512ull*1024*1024,"Region event storage limit");const auto key=std::bit_cast<std::int32_t>((std::uint32_t(box.right)-std::uint32_t(box.left))+(std::uint32_t(box.bottom)-std::uint32_t(box.top)));events.push_back({kind,id,box,key,events.size()});};
        for(std::size_t i=0;i<groups.size();++i)if(!groups[i].positive){VectorBounds box;for(auto member:groups[i].members){const auto& m=e.model(member.model);vector_graph_detail::unite(box,{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]});}add(0,int(i),box);}for(int id:source_holes)add(1,id,value(a.loops,id).archived_bounds);for(int id:target_holes)add(2,id,value(a.loops,id).archived_bounds);
        std::ranges::sort(events,[](const Event& a,const Event& z){return a.key!=z.key?a.key>z.key:a.order>z.order;});for(const auto& event:events){b.charge();const auto& members=event.kind?value(a.loops,event.id).members:groups[event.id].members;const int at=origin(members);int destination=find_container(at,event.kind?fresh:all,event.box);if(destination<0&&event.kind)destination=find_container(at,event.kind==1?right:left,event.box);if(destination<0)continue;if(event.kind==0){const int id=new_hole(destination);populate({false,id},members);}else{const int owner=value(a.loops,event.id).region;Editor::remove(region(owner).holes,event.id);region(destination).holes.push_back(event.id);value(a.loops,event.id).region=destination;vector_graph_detail::unite(dirty,region_bounds(destination));}}
        for(auto [old,next]:repair.redirected_models){(void)next;if(e.model(old).flags&0x02000000)free_model(old);}repair.region_pairs.clear();repair.redirected_models.clear();repair.visited_vertices.clear();for(int id:e.owner.models)e.model(id).flags&=0xccffffffu;
        std::set<int> holes,regions;for(auto key:retire)(key.embedded?regions:holes).insert(key.id);same_page(holes,6);same_page(regions,5);for(int id:holes)free_loop(id);for(int id:regions)free_region(id);
        if(materialize)rerun(all);else if(output)output->insert(output->end(),all.begin(),all.end());publish();repair.context_dirty=dirty;return dirty;
    }
};
Integrator::Integrator(Editor& e):state_(std::make_unique<State>(e)){}
Integrator::~Integrator()=default;
const std::map<int,std::size_t>& Integrator::face_planes()const{return state_->face_owners;}
std::map<int,std::size_t> Integrator::line_planes(){return state_->line_owners();}
VectorBounds Integrator::cache_loop(bool embedded,int id){return state_->cache_loop({embedded,id});}
bool Integrator::retire_empty_hole(int loop){if(!state_->value(state_->a.loops,loop).members.empty())return false;state_->free_loop(loop);return true;}
Editor& Integrator::geometry(){return state_->e;}
VectorPaintArchive& Integrator::paint(){return state_->a;}
int Integrator::create_region(){return state_->new_region();}
int Integrator::create_hole(int region){return state_->new_hole(region);}
void Integrator::populate_loop(bool embedded,int id,const std::vector<VectorLineMember>& members){state_->populate({embedded,id},members);}
int Integrator::create_face(std::size_t plane,int region,VectorArchiveFace face){return state_->new_face(plane,region,face);}
int Integrator::create_material(unsigned pool,std::size_t plane,Bytes bytes){auto& s=*state_;if(pool<9||pool>11)throw std::invalid_argument("Region copy invalid material pool");if(pool!=9&&!s.paints.contains(plane))throw std::invalid_argument("Region material lacks Paint plane");vector_material_detail::charge(s.e.wire,bytes.size(),8ull*1024*1024,"Region material wire limit");vector_material_detail::charge(s.e.storage,bytes.size(),512ull*1024*1024,"Region material storage limit");const int id=pool==9?s.create(s.a.gradnodes,pool,std::move(bytes)):pool==10?s.create(s.a.gradations,pool,std::move(bytes)):s.create(s.a.textures,pool,std::move(bytes));if(pool!=9){s.paints.at(plane).ids[pool==10?1:2].push_back(id);s.e.reference();}return id;}
void Integrator::replace_face_gradation(int face,int gradation){
    auto& s=*state_;s.b.charge();(void)s.value(s.a.gradations,gradation);auto old=s.value(s.a.faces,face);
    if((old.flags&15)==2&&old.material==gradation)return;
    const auto plane=s.face_owners.at(face);if(std::ranges::find(s.paints.at(plane).ids[1],gradation)==s.paints.at(plane).ids[1].end())throw std::invalid_argument("GRAD replacement material differs from Face plane");
    vector_material_detail::MaterialDetach op{s.a,s.paints.at(plane),s.e.layer,s.e.nodes,s.e.refs,s.e.storage,s.b.work,s.b.stop,{},{},false};op.detach(face,old);
    for(unsigned i=3;i<6;++i)s.slots[i].reset();
    auto& destination=s.value(s.a.faces,face);destination.flags=(destination.flags&0xfffffff2u)|2;destination.material=-1;
    s.attach_material(face,gradation,2);s.e.topology_changed=true;
}
bool Integrator::replace_face_solid(int face,std::uint64_t color,bool preserve_alpha){
    auto& s=*state_;s.b.charge();auto old=s.value(s.a.faces,face);const auto plane=s.face_owners.at(face);
    if(preserve_alpha&&(old.flags&15)==1&&(old.color>>48)!=0xffff)color=(color&0x0000ffffffffffffull)|(old.color&0xffff000000000000ull);
    const auto flags=(old.flags&0xfffffff1u)|1u;
    const bool changed=old.flags!=flags||(old.flags&15)!=1||old.color!=color;
    vector_material_detail::MaterialDetach op{s.a,s.paints.at(plane),s.e.layer,s.e.nodes,s.e.refs,s.e.storage,s.b.work,s.b.stop,{},{},false};op.detach(face,old);
    for(unsigned i=3;i<6;++i)s.slots[i].reset();
    auto& destination=s.value(s.a.faces,face);destination.flags=flags;destination.color=color;destination.material=-1;
    s.e.topology_changed|=changed;return changed;
}
void Integrator::replace_face_texture(int face,int texture){
    auto& s=*state_;s.b.charge();(void)s.value(s.a.textures,texture);auto old=s.value(s.a.faces,face);
    if((old.flags&15)==4&&old.material==texture)return;
    const auto plane=s.face_owners.at(face);if(std::ranges::find(s.paints.at(plane).ids[2],texture)==s.paints.at(plane).ids[2].end())throw std::invalid_argument("Texture replacement material differs from Face plane");
    vector_material_detail::MaterialDetach op{s.a,s.paints.at(plane),s.e.layer,s.e.nodes,s.e.refs,s.e.storage,s.b.work,s.b.stop,{},{},false};op.detach(face,old);
    for(unsigned i=3;i<6;++i)s.slots[i].reset();
    auto& destination=s.value(s.a.faces,face);destination.flags=(destination.flags&0xfffffff4u)|4u;destination.material=-1;
    s.attach_material(face,texture,4);s.e.topology_changed=true;
}
void Integrator::erase_face(int face){state_->b.charge();state_->free_face(face);}

void Integrator::erase_orphan_gradation(std::size_t plane,int material){
    auto& s=*state_;const auto gradient=decode_vector_gradation(s.value(s.a.gradations,material));if(!gradient.faces.empty())throw std::invalid_argument("GRAD orphan still has Face consumers");
    Editor::remove(s.paints.at(plane).ids[1],material);--s.e.refs;
    for(int node:gradient.stops){s.b.charge();s.erase(s.a.gradnodes,9,node);--s.e.refs;}
    s.erase(s.a.gradations,10,material);
}
void Integrator::publish(){state_->publish();}
void Integrator::erase_extension_region(int region){state_->free_region(region);}
void Integrator::erase_model_after_repair(int model){state_->free_model(model);}
VectorBounds Integrator::complete_missing_sides(const std::vector<int>& models,std::vector<int>& regions){return state_->missing_sides(models,regions);}
VectorBounds Integrator::commit_regions(RepairState& r,const std::vector<int>& primary,const std::vector<int>& affected,bool rerun,std::vector<int>* selected,std::vector<int>* output){return state_->commit(r,primary,affected,rerun,selected,output,std::nullopt);}
VectorBounds Integrator::commit_regions(RepairState& r,const std::vector<int>& primary,const std::vector<int>& affected,bool rerun,std::vector<int>* selected,std::vector<int>* output,std::optional<std::uint64_t> frozen_color){return state_->commit(r,primary,affected,rerun,selected,output,frozen_color);}
VectorBounds Integrator::materialize_regions(const std::vector<int>& pending,std::vector<int>* collector,bool canonical_page_order){state_->dirty={};state_->rerun(pending,collector,canonical_page_order);state_->publish();return state_->dirty;}
bool Integrator::merge_exteriors_once(VectorBounds& dirty){auto& s=*state_;int first=-1;for(int id:s.a.border.regions){s.b.charge();if(!s.region(id).outer.members.empty())continue;if(first<0){first=id;continue;}const auto holes=s.region(id).holes;for(int hole:holes){s.region(first).holes.push_back(hole);s.value(s.a.loops,hole).region=first;vector_graph_detail::unite(dirty,s.value(s.a.loops,hole).archived_bounds);}s.region(id).holes.clear();s.free_region(id);s.publish();return true;}return false;}
}

namespace retas::vector_boundary_detail {
bool Integrator::convert_raster_connector(int seed,int model,int gap,std::vector<int>& live,VectorBounds& dirty){return state_->raster_connector(seed,model,gap,live,dirty);}
int Integrator::region_successor(int region)const{const auto i=state_->region_next.find(region);if(i==state_->region_next.end())throw std::invalid_argument("Raster connector lost live Region link");return i->second;}
}
