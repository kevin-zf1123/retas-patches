#include "vector_fill_boundary.hpp"
#include <algorithm>
#include <map>
#include <set>
namespace retas::vector_fill_detail {
namespace {
using namespace vector_probe_detail;
template<class T> const T& value(const std::vector<VectorPoolRecord<T>>& pool,int id,Budget budget){
    budget.charge(pool.size()+1);const auto i=std::ranges::find(pool,id,&VectorPoolRecord<T>::index);
    if(i==pool.end())throw std::invalid_argument("BFIL dead persistent graph identity");return i->value;
}
bool positive(VectorBounds b){return b.left<b.right&&b.top<b.bottom;}
bool contains(VectorBounds b,VectorFaceVertex p){return b.left<=p.x&&b.top<=p.y&&b.right>p.x&&b.bottom>p.y;}
int classify(const VectorArchiveLoop& loop,VectorFaceVertex point,const Graph& graph,Budget budget,bool exclude_origins=false){
    budget.charge();if(!contains(loop.archived_bounds,point))return 0;
    if(exclude_origins)for(auto member:loop.members){budget.charge();const auto& m=*graph.models.at(member.model);if(m.subcurves.empty())throw std::invalid_argument("BFIL query empty member");const auto& s=*graph.subcurves.at(member.selector?m.subcurves.front():m.subcurves.back());const auto& v=*graph.vertices.at(member.selector?s.vertex_a:s.vertex_b);if(point.x==v.x&&point.y==v.y)return 0;}
    const auto ray=[&](bool horizontal){std::vector<int> hits;
        for(auto member:loop.members){budget.charge();const auto& m=*graph.models.at(member.model);const auto& b=m.scalars;
            if(horizontal?(point.y<b[1]||point.y>=b[3]):(point.x<b[0]||point.x>=b[2]))continue;
            for(auto id:m.subcurves){budget.charge();const auto& s=*graph.subcurves.at(id);const auto c=graph.curve(m,s,budget);
                for(auto p:{c.start,c.end})if(std::abs(std::int64_t(p.x))>134217727||std::abs(std::int64_t(p.y))>134217727)throw std::invalid_argument("BFIL span endpoint outside raw domain");
                if(!c.linear&&(std::abs(std::int64_t(c.control.x))>134217727||std::abs(std::int64_t(c.control.y))>134217727))throw std::invalid_argument("BFIL span control outside raw domain");
                auto b=VectorBounds{std::min(c.start.x,c.end.x),std::min(c.start.y,c.end.y),std::max(c.start.x,c.end.x)+1,std::max(c.start.y,c.end.y)+1};
                if(!c.linear){b.left=std::min(b.left,c.control.x);b.top=std::min(b.top,c.control.y);b.right=std::max(b.right,c.control.x+1);b.bottom=std::max(b.bottom,c.control.y+1);}
                const int query=horizontal?point.y:point.x;
                if(horizontal?(query<b.top||query>=b.bottom):(query<b.left||query>=b.right))continue;
                for(auto t:axis_roots(c,horizontal,query,true,budget)){const auto p=integer_position(c,t,budget);hits.push_back(horizontal?p.x:p.y);}
            }
        }return hits;
    };
    const auto x=ray(true);if(x.empty())return 0;const bool odd=x.size()%2;
    if(!odd&&std::ranges::find(x,point.x)==x.end())return std::ranges::count_if(x,[&](int v){return v<point.x;})%2?1:0;
    const auto y=ray(false);if(y.empty())return 0;
    if(odd||std::ranges::find(y,point.y)!=y.end())return 2;
    return std::ranges::count_if(y,[&](int v){return v<point.y;})%2?1:0;
}
}
bool region_contains(const VectorPaintArchive& a,int id,VectorFaceVertex point,Budget budget,bool exclude_origins){
    budget.charge();if(std::abs(std::int64_t(point.x))>1073741824||std::abs(std::int64_t(point.y))>1073741824)throw std::invalid_argument("BFIL seed outside raw domain");
    const auto& r=value(a.regions,id,budget);Graph graph(a.base,budget);
    if(positive(r.outer.archived_bounds)&&!classify(r.outer,point,graph,budget,exclude_origins))return false;
    for(auto hole:r.holes)if(classify(value(a.loops,hole,budget),point,graph,budget,exclude_origins))return false;
    return true;
}
bool region_contains(const VectorPaintArchive& a,int region,VectorFaceVertex point,Budget budget){return region_contains(a,region,point,budget,false);}
void validate_paint_owners(const VectorPaintArchive& a,const Layer& layer,Budget budget){
    std::array<std::set<int>,3> assigned;
    for(const auto& plane:layer.planes){budget.charge();if(plane.opaque)throw std::invalid_argument("Opaque plane prevents BFIL paint ownership query");
        if(plane.family!=0x22100001&&plane.family!=0x22100002)continue;
        Reader r(plane.specific);for(unsigned kind=0;kind<3;++kind){const auto count=r.i32();r.require_count(count,4,8192);budget.charge(32*std::uint64_t(count));
            for(int i=0;i<count;++i){const int id=r.i32();if(id<0||!assigned[kind].insert(id).second)throw std::invalid_argument("BFIL competing intrusive paint-plane ownership");
                if(kind==0)(void)value(a.faces,id,budget);else if(kind==1)(void)value(a.gradations,id,budget);else(void)value(a.textures,id,budget);
            }
        }
    }
}
}

#include "vector_material_edit.hpp"
#include "vector_curve_math.hpp"
#include "vector_document_geometry.hpp"
#include "retas/numeric.hpp"
#include <bit>
#include <deque>
namespace retas::vector_fill_detail {
namespace {
using vector_material_detail::VectorSlots;
using vector_material_detail::PaintLists;
using numeric_detail::binary64;
struct LoopKey {bool outer{};int id{};auto operator<=>(const LoopKey&)const=default;};
struct GeometryQueries {
    const VectorLineArchive& a;Budget b;
    template<class T> const T& get(const std::vector<VectorPoolRecord<T>>& pool,int id){b.charge(pool.size()+1);const auto i=std::ranges::find(pool,id,&VectorPoolRecord<T>::index);if(i==pool.end())throw std::invalid_argument("BFIL geometry dead identity");return i->value;}
    const VectorModelCurve& mc(int id){return get(a.models,id);}const VectorSubcurve& sc(int id){return get(a.subcurves,id);}const VectorVertex& vertex(int id){return get(a.vertices,id);}
    VectorFaceVertex point(int id){const auto& p=vertex(id);if(std::abs(std::int64_t(p.x))>134217727||std::abs(std::int64_t(p.y))>134217727)throw std::invalid_argument("BFIL boundary point outside raw domain");return {p.x,p.y};}
    Curve curve(int id,bool force_bezier=false){const auto& s=sc(id);const auto& m=mc(s.model);return {point(s.vertex_a),{s.control_x,s.control_y},point(s.vertex_b),!force_bezier&&bool(m.flags&1)};}
    float angle(int sub,int at){const auto c=curve(sub);const auto center=point(at);auto other=sc(sub).vertex_a==at?c.end:c.start;
        if(!c.linear){const auto dx=double(std::int64_t(c.end.x)-c.start.x),dy=double(std::int64_t(c.end.y)-c.start.y),x=double(std::int64_t(c.control.x)-c.start.x),y=double(std::int64_t(c.control.y)-c.start.y);const auto square=std::min(binary64(binary64(dx*dx)+binary64(dy*dy)),double(INT32_MAX));const auto cross=binary64(binary64(y*dx)-binary64(x*dy));const auto flat=square?binary64(binary64(cross*cross)/square)<=4:binary64(binary64(x*x)+binary64(y*y))<=4;if(!flat)other=c.control;}
        return vector_curve_detail::heading({double(center.x),double(center.y)},{double(other.x),double(other.y)});
    }
    static float scalar(double n){const float result=float(n);if(!std::isfinite(result))throw std::invalid_argument("BFIL nonfinite angle refinement scalar");return result;}
    Curve oriented_bezier(int id,int at){auto c=curve(id,true);const auto& s=sc(id);if(s.vertex_a!=at){std::swap(c.start,c.end);}return c;}
    VectorPoint radial(const Curve& c,double radius,double length){if(radius<=0)return {double(c.start.x),double(c.start.y)};const auto count=std::max(4.0,std::trunc(binary64(length/64)));if(count>1000000)throw std::length_error("BFIL boundary radial sample limit");const float step=scalar(binary64(1/count));double previous_square{};
        for(std::size_t i=1;i<=std::size_t(count);++i){const auto p=position(c,scalar(binary64(double(i)*step)),b);const auto dx=binary64(p.x-c.start.x),dy=binary64(p.y-c.start.y),square=binary64(binary64(dx*dx)+binary64(dy*dy));
            if(square>binary64(radius*radius)){const auto current=std::sqrt(square),previous=previous_square?std::sqrt(previous_square):0;const auto fraction=binary64(binary64(current-radius)/binary64(current-previous));const float t=scalar(binary64(binary64(double(i)-fraction)*step));return position(c,t,b);}previous_square=square;
        }return {double(c.end.x),double(c.end.y)};
    }
    double refine(int current,int near,int at,float heading,float neighbor,float epsilon){const auto& first=sc(current);const auto& second=sc(near);const int opposite=first.vertex_a==at?first.vertex_b:first.vertex_a,other=second.vertex_a==at?second.vertex_b:second.vertex_a;
        const auto tie=[&](){
            //Native raw-pointer ordering is identical to slot order inside one
            //SC pool page; cross-page heap address order is not on the CEL wire.
            if(first.vertex_a==at&&second.vertex_a==at&&(current>>12)!=(near>>12))throw std::invalid_argument("BFIL cross-page pointer angle tie is unsupported");
            const bool minus=first.vertex_a==at&&(second.vertex_a!=at||current<near);const auto offset=binary64(double(epsilon)*double(.1f));return minus?binary64(double(heading)-offset):binary64(double(heading)+offset);
        };
        if(opposite==other&&first.control_x==second.control_x&&first.control_y==second.control_y)return tie();
        const auto a=oriented_bezier(current,at),z=oriented_bezier(near,at);const auto al=four_chord_length(a,b),zl=four_chord_length(z,b);const VectorPoint origin{double(point(at).x),double(point(at).y)};float aa,za;
        if(al<zl){aa=vector_curve_detail::heading(origin,{double(a.end.x),double(a.end.y)});za=vector_curve_detail::heading(origin,radial(z,al,zl));}
        else{aa=vector_curve_detail::heading(origin,radial(a,zl,al));za=vector_curve_detail::heading(origin,{double(z.end.x),double(z.end.y)});}
        if(aa==za){const auto radius=binary64(std::min(al,zl)*.5);aa=vector_curve_detail::heading(origin,radial(a,radius,al));za=vector_curve_detail::heading(origin,radial(z,radius,zl));}
        auto delta=binary64(double(aa)-za);if(delta>180)delta=binary64(delta-360);else if(delta< -180)delta=binary64(delta+360);if(std::abs(delta)<=double(1e-5f))return tie();
        const auto original_raw=binary64(double(heading)-neighbor);float original=scalar(original_raw);if(original_raw< -180)original=scalar(binary64(double(original)+360));else if(original>180)original=scalar(binary64(double(original)-360));
        if(binary64(double(original)*delta)>0)return heading;
        const auto scaled=binary64(double(epsilon)*delta);float offset=scalar(scaled);if(std::abs(scaled)<double(1e-5f))offset=scalar(delta);return scalar(binary64(double(neighbor)+offset));
    }
    int jump(int at,int seed){std::map<float,int> fan;for(auto id:vertex(at).subcurves){b.charge(128);if(id<0)continue;const auto& s=sc(id);if(s.vertex_a!=at&&s.vertex_b!=at)throw std::invalid_argument("BFIL boundary nonincident adjacency");if(id!=seed&&(mc(s.model).flags&0x02000000))continue;
            float heading=angle(id,at),best=90,near_heading{};int near=-1;
            for(auto [old,edge]:fan){double delta=std::abs(binary64(double(old)-heading));if(delta>180)delta=binary64(360-delta);if(delta<best){best=scalar(delta);near=edge;near_heading=old;}}
            if(near<0){fan[heading]=id;continue;}
            float epsilon=.01f;
            for(unsigned count=0;count<100;++count){b.charge(128);const auto candidate=refine(id,near,at,heading,near_heading,epsilon);const auto hit=std::ranges::find_if(fan,[&](auto entry){return double(entry.first)==candidate;});
                if(hit==fan.end()){fan[scalar(candidate)]=id;break;}
                near=hit->second;heading=near_heading=scalar(candidate);epsilon=scalar(binary64(double(epsilon)*double(.8f)));
            }
        }
        const auto i=std::ranges::find_if(fan,[&](auto p){return p.second==seed;});if(i==fan.end())throw std::invalid_argument("BFIL boundary seed absent from native refined fan");const auto previous=i==fan.begin()?std::prev(fan.end()):std::prev(i);return previous->second;
    }
    bool orientation(const std::vector<VectorLineMember>& list,bool temporary=false){if(list.empty())throw std::invalid_argument("BFIL empty temporary contour");const auto& m=mc(list.front().model);if(m.subcurves.empty())throw std::invalid_argument("BFIL empty orientation MC");const auto& s=sc(temporary||list.front().selector?m.subcurves.front():m.subcurves.back());const auto origin=point(temporary||list.front().selector?s.vertex_a:s.vertex_b);
        const auto shifted=[](int v){return std::bit_cast<std::int32_t>(std::uint32_t(v)<<4);};const VectorFaceVertex o{shifted(origin.x),shifted(origin.y)};std::uint64_t sum{};
        const auto triangle=[&](VectorFaceVertex p,VectorFaceVertex q){const auto difference=[](int a,int z){return std::bit_cast<std::int32_t>(std::uint32_t(a)-std::uint32_t(z));};const auto px=difference(p.x,o.x),py=difference(p.y,o.y),qx=difference(q.x,o.x),qy=difference(q.y,o.y);sum+=std::uint64_t(std::int64_t(px)*qy)-std::uint64_t(std::int64_t(py)*qx);};
        for(auto member:list){const auto owner=mc(member.model);const auto& ids=owner.subcurves;const auto add=[&](int id){const auto c=curve(id);auto p=member.selector?c.start:c.end,q=member.selector?c.end:c.start;p={shifted(p.x),shifted(p.y)};q={shifted(q.x),shifted(q.y)};
                if(c.linear)triangle(p,q);else{const auto raw=position(c,.5f,b);const auto convert=[](double n){const auto value=std::trunc(binary64(n*16));if(!std::isfinite(value)||value<INT32_MIN||value>INT32_MAX)throw std::invalid_argument("BFIL area midpoint outside signed32");return int(value);};const VectorFaceVertex mid{convert(raw.x),convert(raw.y)};triangle(p,mid);triangle(mid,q);}};
            if(member.selector)for(auto id:ids)add(id);else for(auto i=ids.rbegin();i!=ids.rend();++i)add(*i);
        }return std::bit_cast<std::int64_t>(sum)>0;
    }
};
struct BoundaryEditor {
    VectorPaintArchive& a;Layer& layer;std::uint64_t& wire;std::uint64_t& nodes;std::uint64_t& refs;std::uint64_t& memory;Budget b;
    std::array<std::optional<VectorSlots>,12> slots;
    std::map<std::size_t,PaintLists> paints;std::map<int,std::size_t> face_planes;
    VectorBounds dirty{};
    BoundaryEditor(VectorPaintArchive& graph,Layer& l,std::uint64_t& w,std::uint64_t& n,std::uint64_t& r,std::uint64_t& s,Budget cost):a(graph),layer(l),wire(w),nodes(n),refs(r),memory(s),b(cost){
        b.charge(32*(nodes+refs+1));vector_material_detail::charge(memory,128*(nodes+refs+1),512ull*1024*1024,"BFIL boundary scratch storage exceeds512MiB");
        for(std::size_t i=0;i<l.planes.size();++i)if(l.planes[i].family==0x22100001||l.planes[i].family==0x22100002){auto& list=paints.emplace(i,PaintLists(l.planes[i])).first->second;for(auto id:list.ids[0])face_planes.emplace(id,i);}
    }
    template<class T> T& get(std::vector<VectorPoolRecord<T>>& pool,int id){b.charge(pool.size()+1);auto i=std::ranges::find(pool,id,&VectorPoolRecord<T>::index);if(i==pool.end())throw std::invalid_argument("BFIL boundary dead identity");return i->value;}
    VectorModelCurve& mc(int id){return get(a.base.models,id);}VectorSubcurve& sc(int id){return get(a.base.subcurves,id);}VectorVertex& vertex(int id){return get(a.base.vertices,id);}VectorArchiveRegion& region(int id){return get(a.regions,id);}
    VectorArchiveLoop& loop(LoopKey id){return id.outer?region(id.id).outer:get(a.loops,id.id);}
    LoopKey lane(int mc_id,int side){const auto& m=mc(mc_id);if(m.targets[side]<0)throw std::invalid_argument("BFIL boundary requires a live opposite Loop");return {m.target_kinds[side]!=0,m.targets[side]};}
    template<class T> VectorSlots& pool(std::vector<VectorPoolRecord<T>>& list,unsigned kind){if(!slots[kind])slots[kind].emplace(list,b.work,b.stop,layer.runtime_vector_high_water[kind]);return *slots[kind];}
    template<class T> void publish(std::vector<VectorPoolRecord<T>>& list,unsigned kind){b.charge(32*list.size());std::ranges::sort(list,{},&VectorPoolRecord<T>::index);if(kind<6)a.base.high_water[kind]=slots[kind]->high;else a.face_high_water[kind-6]=slots[kind]->high;layer.runtime_vector_high_water[kind]=slots[kind]->high;}
    template<class T> int create(std::vector<VectorPoolRecord<T>>& list,unsigned kind,T v){
        vector_material_detail::charge(wire,128,8ull*1024*1024,"BFIL boundary wire exceeds8MiB");vector_material_detail::charge(nodes,1,4096,"BFIL boundary records exceed4096");vector_material_detail::charge(memory,512,512ull*1024*1024,"BFIL boundary storage exceeds512MiB");
        const int id=pool(list,kind).allocate(b.work,b.stop);list.push_back({id,std::move(v),{}});publish(list,kind);return id;
    }
    template<class T> void erase(std::vector<VectorPoolRecord<T>>& list,unsigned kind,int id){auto& allocator=pool(list,kind);b.charge(list.size());const auto i=std::ranges::find(list,id,&VectorPoolRecord<T>::index);if(i==list.end())throw std::invalid_argument("BFIL boundary release dead identity");allocator.release(id);list.erase(i);--nodes;publish(list,kind);}
    void reference(std::uint64_t count=1){vector_material_detail::charge(refs,count,8192,"BFIL boundary references exceed8192");vector_material_detail::charge(wire,8*count,8ull*1024*1024,"BFIL boundary wire exceeds8MiB");}
    static void remove(std::vector<int>& list,int id){const auto i=std::ranges::find(list,id);if(i==list.end())throw std::invalid_argument("BFIL boundary missing ownership link");list.erase(i);}
    static void unite(VectorBounds& dst,VectorBounds src){if(!positive(src))return;if(!positive(dst))dst=src;else dst={std::min(dst.left,src.left),std::min(dst.top,src.top),std::max(dst.right,src.right),std::max(dst.bottom,src.bottom)};}
    void refresh_loop(LoopKey key){VectorBounds box{};for(auto member:loop(key).members){const auto& m=mc(member.model);unite(box,{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]});}loop(key).archived_bounds=box;unite(dirty,box);}
    bool same_material(int first,int second){const auto& x=region(first).faces;const auto& y=region(second).faces;if(x.size()!=y.size())return false;
        for(std::size_t i=0;i<x.size();++i){b.charge();if(x[i]<0||y[i]<0)throw std::invalid_argument("BFIL boundary material comparison requires live Face nodes");const auto& left=get(a.faces,x[i]);const auto& right=get(a.faces,y[i]);
            if(!face_planes.contains(x[i])||!face_planes.contains(y[i]))throw std::invalid_argument("BFIL boundary Face lacks Paint plane");
            if(face_planes.at(x[i])!=face_planes.at(y[i])||left.flags!=right.flags)return false;
            switch(left.flags&15){case 1:if(left.color!=right.color)return false;break;case 2:case 4:if(left.material!=right.material)return false;break;default:break;}
        }return true;
    }
    bool mark(LoopKey key,int seed,std::set<int>& seen,std::deque<int>& pending){bool marked{};const auto members=loop(key).members;
        for(auto member:members){auto& m=mc(member.model);if((m.flags&0x40)||m.line>=0)continue;const auto other=lane(member.model,member.selector?0:1);const int neighbor=loop(other).region;if(neighbor<0)throw std::invalid_argument("BFIL boundary opposite Loop has no Region");if(!same_material(seed,neighbor))continue;
            mc(member.model).flags=0x02000000;marked=true;if(seen.insert(neighbor).second)pending.push_back(neighbor);
        }return marked;
    }
    VectorFaceVertex point(int id){return GeometryQueries{a.base,b}.point(id);}
    float angle(int sub,int at){return GeometryQueries{a.base,b}.angle(sub,at);}
    int jump(int at,int seed){return GeometryQueries{a.base,b}.jump(at,seed);}
    struct Member {LoopKey owner;std::size_t position;};
    Member locate(LoopKey key,int model,bool side){const auto& list=loop(key).members;const auto i=std::ranges::find_if(list,[&](auto m){return m.model==model&&(m.selector!=0)==side;});if(i==list.end())throw std::invalid_argument("BFIL boundary missing ordered LoopMember");return {key,std::size_t(i-list.begin())};}
    VectorLineMember descriptor(Member m){return loop(m.owner).members.at(m.position);}
    std::vector<VectorLineMember> walk(Member first){std::vector<VectorLineMember> out;auto current=first;
        for(;;){b.charge(128);const auto member=descriptor(current);const auto mask=member.selector?0x10000000u:0x20000000u;if(mc(member.model).flags&mask)break;mc(member.model).flags|=mask;out.push_back(member);vector_material_detail::charge(memory,16,512ull*1024*1024,"BFIL boundary temporary contour storage exceeds512MiB");
            const auto& list=loop(current.owner).members;Member candidate{current.owner,(current.position+1)%list.size()};const auto next=descriptor(candidate);
            if(mc(next.model).flags&0x02000000){const auto& m=mc(member.model);if(m.subcurves.empty())throw std::invalid_argument("BFIL empty boundary MC");const int seed=member.selector?m.subcurves.back():m.subcurves.front();const auto& s=sc(seed);const int at=member.selector?s.vertex_b:s.vertex_a;const int found=jump(at,seed);const auto& peer=sc(found);const auto& target=mc(peer.model);const bool side=!target.subcurves.empty()&&found==target.subcurves.front()&&peer.vertex_a==at;candidate=locate(lane(peer.model,side?1:0),peer.model,side);}
            const auto chosen=descriptor(candidate);if(mc(chosen.model).flags&(chosen.selector?0x10000000u:0x20000000u))break;current=candidate;
        }return out;
    }
    bool orientation(const std::vector<VectorLineMember>& list){return GeometryQueries{a.base,b}.orientation(list);}
    struct Group {std::vector<VectorLineMember> members;bool outer{};};
    void groups(LoopKey key,std::vector<Group>& out){const auto list=loop(key).members;for(std::size_t i=0;i<list.size();++i){const auto member=list[i];const auto mask=member.selector?0x10000000u:0x20000000u;if((mc(member.model).flags&mask)||(mc(member.model).flags&0x03000000))continue;auto members=walk({key,i});const bool outside=orientation(members);out.push_back({std::move(members),outside});}}
    void populate(LoopKey key,const std::vector<VectorLineMember>& list){loop(key).members=list;reference(list.size());for(auto member:list){auto& m=mc(member.model);const int side=member.selector!=0;m.target_kinds[side]=key.outer?1:0;m.targets[side]=key.id;}refresh_loop(key);}
    void attach(int id,VectorArchiveFace& face,int material,unsigned kind){if(material<0)return;auto& raw=kind==2?get(a.gradations,material):get(a.textures,material);b.charge(4*raw.size()+32);vector_material_detail::charge(memory,2*raw.size()+128,512ull*1024*1024,"BFIL material clone storage exceeds512MiB");
        if(kind==2){auto m=decode_vector_gradation(raw);m.faces.push_back(id);reference();raw=encode_vector_gradation(m);}else{auto m=decode_vector_texture(raw);m.faces.push_back(id);reference();raw=encode_vector_texture(m);}face.material=material;
    }
    void clone_faces(int source,int destination){const auto original=region(source).faces;for(auto old:original){if(old<0||!face_planes.contains(old))throw std::invalid_argument("BFIL cloning requires a live same-plane Face");const auto v=get(a.faces,old);const auto plane=face_planes.at(old);VectorArchiveFace face;face.region=destination;face.material=-1;face.flags=v.flags;const unsigned kind=v.flags&15;if(kind==1)face.color=v.color;const int id=create(a.faces,8,face);paints.at(plane).ids[0].push_back(id);region(destination).faces.push_back(id);face_planes.emplace(id,plane);reference(2);if(kind==2||kind==4)attach(id,get(a.faces,id),v.material,kind);}}
    void free_face(int id){if(!face_planes.contains(id))throw std::invalid_argument("BFIL Face release lacks plane");const auto plane=face_planes.at(id);auto face=get(a.faces,id);remove(paints.at(plane).ids[0],id);erase(a.faces,8,id);face_planes.erase(id);
        vector_material_detail::MaterialDetach detach{a,paints.at(plane),layer,nodes,refs,memory,b.work,b.stop,{},{},false};detach.detach(id,face);
    }
    void free_region(int id){const auto r=region(id);remove(a.border.regions,id);erase(a.regions,7,id);for(auto hole:r.holes)erase(a.loops,6,hole);for(auto face:r.faces){if(face<0)throw std::invalid_argument("BFIL Region release requires live Face chain");free_face(face);}}
    void release_cp(int id,std::size_t expected=1){if(id<0)return;std::size_t users{};for(auto& m:a.base.models){b.charge();users+=std::ranges::count(m.value.crosspoints,id);}if(users!=expected)throw std::invalid_argument("BFIL cannot release shared endpoint CP");erase(a.base.crosspoints,4,id);}
    void free_model(int id){const auto m=mc(id);remove(a.border.models,id);erase(a.base.models,3,id);release_cp(m.crosspoints[0],0);release_cp(m.crosspoints[1],0);
        for(auto sub:m.subcurves){const auto s=sc(sub);for(auto at:{s.vertex_a,s.vertex_b}){remove(vertex(at).subcurves,sub);if(vertex(at).subcurves.empty()){remove(a.border.vertices,at);erase(a.base.vertices,0,at);}}for(auto node:s.scales)erase(a.base.scales,1,node);erase(a.base.subcurves,2,sub);}
    }
    void reverse(int id){auto& m=mc(id);std::ranges::reverse(m.subcurves);for(auto sub:m.subcurves){auto& s=sc(sub);std::swap(s.vertex_a,s.vertex_b);const auto ids=s.scales;std::vector<VectorLineScale> values;for(auto node:ids)values.push_back(get(a.base.scales,node));for(std::size_t i=0;i<ids.size();++i){auto& n=get(a.base.scales,ids[i]);n.scale_bits=values[ids.size()-1-i].scale_bits;n.parameter_bits=std::bit_cast<std::uint32_t>(float(binary64(1-double(std::bit_cast<float>(values[ids.size()-1-i].parameter_bits)))));}}
        std::swap(m.target_kinds[0],m.target_kinds[1]);std::swap(m.targets[0],m.targets[1]);std::swap(m.crosspoints[0],m.crosspoints[1]);for(auto cp:m.crosspoints)if(cp>=0){auto& c=get(a.base.crosspoints,cp);for(int lane=0;lane<2;++lane)if(c.words[lane]==1)c.words[2+lane]=std::bit_cast<std::uint32_t>(float(binary64(1-double(std::bit_cast<float>(c.words[2+lane])))));}const auto f=m.flags;m.flags=(f&~0x3cu)|((f&4)<<1)|((f&8)>>1)|((f&0x10)<<1)|((f&0x20)>>1);
    }
    void merge(int keep,int drop,int at){auto old=mc(keep),other=mc(drop);if(old.line>=0){auto& list=get(a.base.lines,old.line).members;const auto k=std::ranges::find(list,keep,&VectorLineMember::model),d=std::ranges::find(list,drop,&VectorLineMember::model);if(k==list.end()||d==list.end()||(std::abs(k-d)!=1))return;}
        const bool inverted=sc(old.subcurves.front()).vertex_a==at;if(inverted)reverse(keep);
        for(int side=0;side<2;++side)if(mc(drop).targets[side]>=0){auto& list=loop(lane(drop,side)).members;const auto i=std::ranges::find_if(list,[&](auto member){return member.model==drop&&(member.selector!=0)==bool(side);});if(i==list.end())throw std::invalid_argument("BFIL normalization missing drop LoopMember");list.erase(i);}
        if(old.line>=0){auto& list=get(a.base.lines,old.line).members;list.erase(std::ranges::find(list,drop,&VectorLineMember::model));}
        if(sc(mc(drop).subcurves.back()).vertex_b==at)reverse(drop);
        other=mc(drop);const int old_tail=mc(keep).crosspoints[1];if(old_tail>=0){release_cp(old_tail);mc(keep).flags&=~0x20u;}mc(keep).crosspoints[1]=other.crosspoints[1];mc(drop).crosspoints[1]=-1;
        for(auto sub:other.subcurves){sc(sub).model=keep;mc(keep).subcurves.push_back(sub);}mc(drop).subcurves.clear();
        if(inverted)reverse(keep);vector_geometry_detail::rebuild_models(a.base,{keep},b.work,b.stop);
        auto flags=mc(keep).flags;if((flags^other.flags)&2){const auto spans=mc(keep).subcurves;for(auto sub:spans)if(sc(sub).scales.empty()){const int first=create(a.base.scales,1,VectorLineScale{0x3f800000,0}),last=create(a.base.scales,1,VectorLineScale{0x3f800000,0x3f800000});sc(sub).scales={first,last};reference(2);}flags|=2;}
        flags=(flags&~0x28u)|(other.flags&0x28u);mc(keep).flags=flags;mc(drop).targets={-1,-1};free_model(drop);
    }
    void normalize(int id){std::vector<int> vertices;const auto collect=[&](LoopKey key){const auto members=loop(key).members;for(auto member:members){const auto m=mc(member.model);if(m.subcurves.empty())throw std::invalid_argument("BFIL normalization empty MC");const auto s=sc(member.selector?m.subcurves.front():m.subcurves.back());vertices.push_back(member.selector?s.vertex_a:s.vertex_b);}};
        collect({true,id});const auto holes=region(id).holes;for(auto hole:holes)collect({false,hole});b.charge(32*vertices.size());
        for(auto at:vertices){const auto adjacency=vertex(at).subcurves;if(adjacency.size()!=2||adjacency[0]<0||adjacency[1]<0)continue;const int first=sc(adjacency.front()).model,second=sc(adjacency.back()).model;if(first==second||mc(first).line!=mc(second).line)continue;
            const float a=angle(adjacency[0],at),z=angle(adjacency[1],at);double delta=std::abs(binary64(double(z)-a));if(delta>180)delta=binary64(360-delta);if(delta>=150)merge(first,second,at);
        }
    }
    MaterializedRegion run(int seed,std::vector<int>* related){std::set<int> seen{seed};std::deque<int> pending{seed};std::vector<int> affected;std::vector<LoopKey> unchanged,rebuild;int destination=-1;
        while(!pending.empty()){b.charge();const int id=pending.front();pending.pop_front();const bool outer=mark({true,id},seed,seen,pending);if(outer)affected.push_back(id);else destination=id;const auto holes=region(id).holes;
            for(auto hole:holes){const LoopKey key{false,hole};const bool marked=mark(key,seed,seen,pending);if(marked&&!outer)rebuild.push_back(key);else if(!marked)unchanged.push_back(key);}}
        if(related)related->insert(related->end(),affected.begin(),affected.end());
        if(affected.empty()&&rebuild.empty())return {seed,false,{}};
        std::vector<Group> all;for(auto id:affected){groups({true,id},all);const auto holes=region(id).holes;for(auto hole:holes)if(std::ranges::find(unchanged,LoopKey{false,hole})==unchanged.end())groups({false,hole},all);}for(auto key:rebuild)groups(key,all);
        if(destination<0){const auto outer=std::ranges::find(all,true,&Group::outer);if(outer==all.end())throw std::invalid_argument("BFIL materialization has no positive native contour");VectorArchiveRegion r;r.outer.region=0;destination=create(a.regions,7,r);region(destination).outer.region=destination;a.border.regions.push_back(destination);reference();populate({true,destination},outer->members);all.erase(outer);clone_faces(seed,destination);}
        for(const auto& group:all){VectorArchiveLoop l;l.region=destination;const int id=create(a.loops,6,l);region(destination).holes.push_back(id);reference();populate({false,id},group.members);}
        for(auto key:unchanged){const int old=loop(key).region;remove(region(old).holes,key.id);region(destination).holes.push_back(key.id);loop(key).region=destination;}
        for(auto key:rebuild){remove(region(loop(key).region).holes,key.id);erase(a.loops,6,key.id);}for(auto id:affected)free_region(id);
        const auto models=a.border.models;for(auto id:models){if(mc(id).flags&0x02000000)free_model(id);else mc(id).flags&=0xcfffffffu;}
        normalize(destination);const auto& final=region(destination);VectorBounds bounds=final.outer.archived_bounds;if(final.outer.members.empty()){bounds={};for(auto hole:final.holes)unite(bounds,get(a.loops,hole).archived_bounds);}unite(dirty,bounds);
        for(auto& [i,list]:paints)list.publish(layer.planes[i]);return {destination,true,dirty};
    }
};
}
float boundary_heading(const VectorLineArchive& graph,int subcurve,int vertex,Budget budget){return GeometryQueries{graph,budget}.angle(subcurve,vertex);}
int boundary_predecessor(const VectorLineArchive& graph,int vertex,int subcurve,Budget budget){return GeometryQueries{graph,budget}.jump(vertex,subcurve);}
bool temporary_orientation(const VectorLineArchive& graph,const std::vector<VectorLineMember>& members,Budget budget){return GeometryQueries{graph,budget}.orientation(members,true);}
MaterializedRegion materialize_region(VectorPaintArchive& a,Layer& layer,int seed,std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& refs,std::uint64_t& memory,Budget budget){return materialize_region(a,layer,seed,wire,nodes,refs,memory,budget,nullptr);}
MaterializedRegion materialize_region(VectorPaintArchive& a,Layer& layer,int seed,std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& refs,std::uint64_t& memory,Budget budget,std::vector<int>* related){BoundaryEditor edit(a,layer,wire,nodes,refs,memory,budget);return edit.run(seed,related);}
}
