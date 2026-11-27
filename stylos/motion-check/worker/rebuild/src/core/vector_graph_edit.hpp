#pragma once
#include "vector_edit_profile.hpp"
#include "vector_document_geometry.hpp"
#include "vector_material_edit.hpp"
#include "vector_probe.hpp"
#include "retas/numeric.hpp"
#include <bit>
#include <cmath>
#include <limits>
namespace retas::vector_graph_detail {
using namespace vector_probe_detail;
using numeric_detail::binary64;
using vector_material_detail::charge;
using vector_material_detail::VectorSlots;
using P=VectorFaceVertex;
inline int integer(double n){if(!std::isfinite(n)||n<INT32_MIN||n>INT32_MAX)throw std::invalid_argument("LCOL cut coordinate exceeds signed32");return int(n);}
inline float f32(double n){const auto f=float(n);if(!std::isfinite(f))throw std::invalid_argument("LCOL cut scalar is not finite");return f;}
inline void unite(VectorBounds& a,VectorBounds b){if(b.left>=b.right||b.top>=b.bottom)return;if(a.left>=a.right||a.top>=a.bottom)a=b;else a={std::min(a.left,b.left),std::min(a.top,b.top),std::max(a.right,b.right),std::max(a.bottom,b.bottom)};}
template<class T> auto& record(std::vector<VectorPoolRecord<T>>& pool,int id){const auto i=std::ranges::find(pool,id,&VectorPoolRecord<T>::index);if(i==pool.end())throw std::invalid_argument("LCOL cut dead pool identity");return *i;}
inline int sq(P a,P b){const auto x=std::int64_t(a.x)-b.x,y=std::int64_t(a.y)-b.y;if(std::abs(x)>46340||std::abs(y)>46340)return INT32_MAX;return int(std::min<std::int64_t>(x*x+y*y,INT32_MAX));}
inline int dot(P a,P b,P q){const auto x=std::int64_t(b.x)-a.x,y=std::int64_t(b.y)-a.y,u=std::int64_t(q.x)-a.x,v=std::int64_t(q.y)-a.y;return int(std::clamp(x*u+y*v,-std::int64_t(INT32_MAX),std::int64_t(INT32_MAX)));}
inline float project(P a,P b,P q){const int n=dot(a,b,q),d=sq(a,b);if(n<=0)return 0;if(n>=d)return 1;
    if(d!=INT32_MAX)return f32(binary64(double(n)/d));const auto x=double(std::int64_t(b.x)-a.x),y=double(std::int64_t(b.y)-a.y),u=double(std::int64_t(q.x)-a.x),v=double(std::int64_t(q.y)-a.y);
    const auto real_n=binary64(binary64(y*v)+binary64(x*u)),real_d=binary64(binary64(y*y)+binary64(x*x));return real_n>=real_d?1:f32(binary64(real_n/real_d));}
inline float closest(const Curve& c,P q,Budget b){
    if(dot(c.start,c.control,q)<=0)return 0;if(dot(c.end,c.control,q)<=0)return 1;
    const auto count=std::max(2.0,std::trunc(binary64(four_chord_length(c,b)/64)));if(count>1000000)throw std::length_error("LCOL closest sample limit");const float h=f32(binary64(1/count));P previous=c.start;float previous_t{},out{};int best=INT32_MAX;
    for(std::size_t i=1;i<=std::size_t(count);++i){const auto t=f32(binary64(double(i)*h));const auto p=integer_position(c,t,b);const auto local=project(previous,p,q);const auto hit=integer_position({previous,{},p,true},local,b);const auto distance=sq(hit,q);
        if(distance<best){best=distance;const auto candidate=binary64(binary64(double(local)*h)+previous_t);out=candidate>1?1:f32(candidate);}previous=p;previous_t=t;}
    return out;
}
inline std::array<P,2> controls(const Curve& c,float t,P actual,Budget b){
    if(c.linear){const auto mid=[](P a,P z){const auto component=[](int x,int y){const auto sum=std::int64_t(x)+y;return integer(sum>=0?sum/2:-((-sum+1)/2));};return P{component(a.x,z.x),component(a.y,z.y)};};return {mid(c.start,actual),mid(actual,c.end)};}
    const auto u=binary64(1-double(t));const auto mix=[&](double a,double z){return binary64(binary64(a*u)+binary64(z*t));};
    const VectorPoint l{mix(c.start.x,c.control.x),mix(c.start.y,c.control.y)},r{mix(c.control.x,c.end.x),mix(c.control.y,c.end.y)};
    const P temporary{integer(std::floor(mix(l.x,r.x))),integer(std::floor(mix(l.y,r.y)))};P lc{integer(std::floor(l.x)),integer(std::floor(l.y))},rc{integer(std::floor(r.x)),integer(std::floor(r.y))};
    const auto kl=closest({c.start,lc,temporary,false},lc,b),kr=closest({temporary,rc,c.end,false},rc,b);
    const auto dx=double(std::int64_t(actual.x)-temporary.x),dy=double(std::int64_t(actual.y)-temporary.y);
    lc.x=integer(double(lc.x)+std::trunc(binary64(dx*kl)));lc.y=integer(double(lc.y)+std::trunc(binary64(dy*kl)));
    rc.x=integer(double(rc.x)+std::trunc(binary64(dx*binary64(1-double(kr)))));rc.y=integer(double(rc.y)+std::trunc(binary64(dy*binary64(1-double(kr)))));return {lc,rc};
}
struct BorderOwnerTag {};
struct OwnerLists {
    bool vertex_plane{},border_owner{};std::vector<int> vertices,models,lines;Bytes topology_tail,line_tail,tail;
    static std::vector<int> read(Reader& r){const auto n=r.i32();r.require_count(n,4,8192);std::vector<int> out;for(int i=0;i<n;++i)out.push_back(r.i32());return out;}
    static void write(Writer& w,const std::vector<int>& list){w.i32(int(list.size()));for(auto id:list)w.i32(id);}
    OwnerLists(std::vector<int> v,std::vector<int> m,std::vector<int> l):vertex_plane(true),vertices(std::move(v)),models(std::move(m)),lines(std::move(l)){}
    OwnerLists(const Plane& p,VectorPaintArchive* paint){vertex_plane=(p.family&0x00f00000u)==0x00200000u;Reader r(p.specific);if(vertex_plane){auto t=r.section();vertices=read(t);models=read(t);topology_tail=t.rest();auto l=r.section();lines=read(l);line_tail=l.rest();tail=r.rest();}else{lines=read(r);tail=r.rest();if(!paint)throw std::invalid_argument("LCOL pure Line cutter requires its Paint Border topology owner");vertices=paint->border.vertices;models=paint->border.models;}}
    explicit OwnerLists(const VectorPaintArchive& paint):border_owner(true),vertices(paint.border.vertices),models(paint.border.models){}
    void publish(VectorPaintArchive& paint)const{if(!border_owner)throw std::invalid_argument("Vector graph owner is not Border");paint.border.vertices=vertices;paint.border.models=models;}
    void publish(Plane& p,VectorPaintArchive* paint){if(border_owner)throw std::invalid_argument("Border graph owner cannot publish a Plane");Writer w;if(vertex_plane){Writer t,l;write(t,vertices);write(t,models);t.raw(topology_tail);write(l,lines);l.raw(line_tail);w.section(t.data);w.section(l.data);}else{write(w,lines);paint->border.vertices=vertices;paint->border.models=models;}w.raw(tail);p.specific=std::move(w.data);}
};
struct Editor;
void weld(Editor&,int keep_vertex,int drop_vertex);
struct Editor {
    VectorLineArchive& a;VectorPaintArchive* paint;Layer& layer;OwnerLists owner;Budget budget;
    std::uint64_t& wire;std::uint64_t& nodes;std::uint64_t& refs;std::uint64_t& storage;
    std::array<std::optional<VectorSlots>,6> slots;bool topology_changed{};
    Editor(VectorLineArchive& graph,VectorPaintArchive* p,Layer& l,std::size_t plane,std::uint64_t& w,std::uint64_t& n,std::uint64_t& r,std::uint64_t& memory,std::uint64_t& work,std::stop_token stop):a(graph),paint(p),layer(l),owner(l.planes.at(plane),p),budget{work,stop},wire(w),nodes(n),refs(r),storage(memory){}
    Editor(VectorLineArchive& graph,VectorPaintArchive* p,Layer& l,BorderOwnerTag,std::uint64_t& w,std::uint64_t& n,std::uint64_t& r,std::uint64_t& memory,std::uint64_t& work,std::stop_token stop):a(graph),paint(p),layer(l),owner(border(p,graph)),budget{work,stop},wire(w),nodes(n),refs(r),storage(memory){}
    Editor(VectorLineArchive& graph,Layer& l,OwnerLists links,std::uint64_t& w,std::uint64_t& n,std::uint64_t& r,std::uint64_t& memory,std::uint64_t& work):a(graph),paint(nullptr),layer(l),owner(std::move(links)),budget{work,{}},wire(w),nodes(n),refs(r),storage(memory){}
    static const VectorPaintArchive& border(VectorPaintArchive* p,VectorLineArchive& graph){if(!p||&p->base!=&graph)throw std::invalid_argument("Border editor requires its Paint base graph");return *p;}
    template<class T> T& value(std::vector<VectorPoolRecord<T>>& pool,int id){budget.charge(pool.size());return record(pool,id).value;}
    VectorModelCurve& model(int id){return value(a.models,id);}VectorSubcurve& sub(int id){return value(a.subcurves,id);}VectorLine& line(int id){return value(a.lines,id);}VectorVertex& vertex(int id){return value(a.vertices,id);}
    template<class T> VectorSlots& allocator(std::vector<VectorPoolRecord<T>>& pool,unsigned kind){if(!slots[kind])slots[kind].emplace(pool,budget.work,budget.stop,layer.runtime_vector_high_water[kind]);return *slots[kind];}
    template<class T> void publish(std::vector<VectorPoolRecord<T>>& pool,unsigned kind){budget.charge(32*pool.size());std::ranges::sort(pool,{},&VectorPoolRecord<T>::index);a.high_water[kind]=slots[kind]->high;layer.runtime_vector_high_water[kind]=slots[kind]->high;}
    template<class T> int create(std::vector<VectorPoolRecord<T>>& pool,unsigned kind,T v){auto& alloc=allocator(pool,kind);charge(wire,128,8ull*1024*1024,"LCOL cut wire limit");charge(nodes,1,4096,"LCOL cut record limit");charge(storage,512,512ull*1024*1024,"LCOL cut storage limit");const int id=alloc.allocate(budget.work,budget.stop);pool.push_back({id,std::move(v),{}});publish(pool,kind);topology_changed=true;return id;}
    template<class T> void erase(std::vector<VectorPoolRecord<T>>& pool,unsigned kind,int id){auto& alloc=allocator(pool,kind);budget.charge(pool.size());const auto i=std::ranges::find(pool,id,&VectorPoolRecord<T>::index);if(i==pool.end())throw std::invalid_argument("LCOL cut release dead slot");alloc.release(id);pool.erase(i);--nodes;publish(pool,kind);topology_changed=true;}
    void reference(std::uint64_t n=1){charge(refs,n,8192,"LCOL cut reference limit");charge(wire,8*n,8ull*1024*1024,"LCOL cut reference wire limit");}
    static void remove(std::vector<int>& list,int id){const auto i=std::ranges::find(list,id);if(i==list.end())throw std::invalid_argument("LCOL cut missing owner/adjacency link");list.erase(i);}
    void cache(int id,bool full){vector_geometry_detail::rebuild_lines(a,{id},budget.work,budget.stop,full);}
    int new_line(const VectorLine& style,int before=-1){VectorLine v;v.half_width_bits=style.half_width_bits;v.color=style.color;const int id=create(a.lines,5,std::move(v));reference();if(before<0)owner.lines.push_back(id);else{const auto i=std::ranges::find(owner.lines,before);if(i==owner.lines.end())throw std::invalid_argument("LCOL residual missing plane owner");owner.lines.insert(i,id);}return id;}
    void width_split(const VectorSubcurve& old,int left,int right,float q){
        struct Node{float t,w;};std::vector<Node> profile;float last=-1;
        for(std::size_t i=0;i<old.scales.size();++i){const auto& n=value(a.scales,old.scales[i]);const float t=i==0?0:i+1==old.scales.size()?1:std::bit_cast<float>(n.parameter_bits),w=std::bit_cast<float>(n.scale_bits);if(!std::isfinite(t)||!std::isfinite(w)||t<last||t<0||t>1||w<0)throw std::invalid_argument("LCOL cut invalid effective width chain");profile.push_back({t,w});last=t;}
        if(profile.size()<2)throw std::invalid_argument("LCOL cut variable profile needs two nodes");const auto append=[&](int id,float t,float w){const int node=create(a.scales,1,VectorLineScale{std::bit_cast<std::uint32_t>(w),std::bit_cast<std::uint32_t>(t)});sub(id).scales.push_back(node);reference();};
        const float il=f32(binary64(1/double(q))),ir=f32(binary64(1/binary64(1-double(q))));std::size_t i=0;double scaled{};
        for(;;){if(i+1>=profile.size())throw std::invalid_argument("LCOL cut missing right width knot");scaled=binary64(double(profile[i].t)*il);append(left,f32(scaled),profile[i].w);if(profile[i+1].t>=q)break;++i;}
        auto j=i+1;const auto l=profile[i],r=profile[j];const double eps=std::bit_cast<float>(0x3a83126fu);float w;
        if(binary64(double(q)-l.t)<eps&&scaled>0){auto& last_node=value(a.scales,sub(left).scales.back());last_node.parameter_bits=0x3f800000;w=l.w;}
        else if(binary64(double(r.t)-q)<eps){w=r.w;append(left,1,w);++j;}
        else{w=f32(binary64(binary64(binary64(binary64(double(r.t)-q)*l.w)+binary64(binary64(double(q)-l.t)*r.w))/binary64(double(r.t)-l.t)));append(left,1,w);}
        append(right,0,w);for(;j<profile.size();++j)append(right,f32(binary64(binary64(double(profile[j].t)-q)*ir)),profile[j].w);value(a.scales,sub(right).scales.back()).parameter_bits=0x3f800000;
    }
    void replace(std::vector<VectorLineMember>& members,int old,int first,int second,bool side,int lane=-1){const auto i=std::ranges::find_if(members,[&](auto v){return v.model==old&&(lane<0||(v.selector!=0)==bool(lane));});if(i==members.end())throw std::invalid_argument("LCOL cut missing LC/Loop membership");const auto n=std::size_t(i-members.begin());const int selector=side?(lane?1:0):(i->selector?1:0);members[n]={second,selector};members.insert(members.begin()+std::ptrdiff_t(n),{first,selector});reference();}
    struct Split{bool cut{};int vertex{-1},left{-1},right{-1};};
    Split split(int model_id,int sc_id,float q){
        const auto old=model(model_id);const auto original=sub(sc_id);const auto at=std::ranges::find(old.subcurves,sc_id);if(at==old.subcurves.end())throw std::invalid_argument("LCOL event SC no longer belongs to residual MC");const auto pos=std::size_t(at-old.subcurves.begin());Graph graph(a,budget);const auto c=graph.curve(old,original,budget);const auto p=node_position(c,q,budget);const int da=sq(p,c.start),db=sq(p,c.end);int cut_vertex=-1;if(std::min(da,db)<=16)cut_vertex=da<db?original.vertex_a:original.vertex_b;
        if((cut_vertex==original.vertex_a&&pos==0)||(cut_vertex==original.vertex_b&&pos+1==old.subcurves.size()))return {false,cut_vertex};
        if(cut_vertex<0){cut_vertex=create(a.vertices,0,VectorVertex{p.x,p.y,{}});owner.vertices.push_back(cut_vertex);reference();}
        return split_at_vertex(model_id,sc_id,q,cut_vertex);
    }
    //5B9B70 uses the supplied live identity, with coordinate-equal endpoint
    // welds before its no-split gate. No nearest-point/squared-distance snap.
    // Mutation is in-place; callers publish their candidate atomically.
    Split split_at_vertex(int model_id,int sc_id,float q,int provided){
        if(!std::isfinite(q))throw std::invalid_argument("Vector supplied cut parameter is not finite");
        auto old=model(model_id);auto original=sub(sc_id);const auto at=std::ranges::find(old.subcurves,sc_id);if(at==old.subcurves.end())throw std::invalid_argument("Vector supplied cut SC is outside its model");const auto pos=std::size_t(at-old.subcurves.begin());
        auto cut_vertex=provided;const auto point=[&](int id){const auto& v=vertex(id);return P{v.x,v.y};};const auto actual=point(provided),first=point(original.vertex_a),last=point(original.vertex_b);
        for(const auto& v:{actual,first,last})if(std::abs(std::int64_t(v.x))>134217727||std::abs(std::int64_t(v.y))>134217727)throw std::invalid_argument("Vector supplied cut vertex exceeds bounded raw domain");
        if(provided!=original.vertex_a&&provided!=original.vertex_b){
            if(actual.x==first.x&&actual.y==first.y){weld(*this,original.vertex_a,provided);cut_vertex=original.vertex_a;}
            else if(actual.x==last.x&&actual.y==last.y){weld(*this,original.vertex_b,provided);cut_vertex=original.vertex_b;}
        }
        // Native5A4560 clamps the sampled point, not the original parameter.
        // SC endpoint partition moves chains without consuming q; accumulated
        // sampling parameters may lie just outside [0,1] on that path.
        if(cut_vertex!=original.vertex_a&&cut_vertex!=original.vertex_b&&(q<0||q>1))throw std::invalid_argument("Vector supplied interior cut parameter is outside [0,1]");
        if((cut_vertex==original.vertex_a&&pos==0)||(cut_vertex==original.vertex_b&&pos+1==old.subcurves.size()))return {false,cut_vertex};
        Graph graph(a,budget);const auto c=graph.curve(old,original,budget);const auto& v=vertex(cut_vertex);const P p{v.x,v.y};
        VectorModelCurve lm,rm;lm.flags=old.flags&~0x20u;rm.flags=old.flags&~0x10u;if((old.flags&12)!=12){lm.flags&=~8u;rm.flags&=~4u;}lm.line=rm.line=old.line;lm.target_kinds=rm.target_kinds=old.target_kinds;lm.targets=rm.targets=old.targets;lm.crosspoints={old.crosspoints[0],-1};rm.crosspoints={-1,old.crosspoints[1]};
        const int left=create(a.models,3,lm),right=create(a.models,3,rm);owner.models.push_back(left);owner.models.push_back(right);reference(2);
        std::vector<int> ls(old.subcurves.begin(),at),rs(std::next(at),old.subcurves.end());
        if(cut_vertex==original.vertex_a)rs.insert(rs.begin(),sc_id);else if(cut_vertex==original.vertex_b)ls.push_back(sc_id);
        else{const int ps=create(a.subcurves,2,VectorSubcurve{}),ss=create(a.subcurves,2,VectorSubcurve{});const auto xy=controls(c,q,p,budget);sub(ps)={original.vertex_a,cut_vertex,xy[0].x,xy[0].y,left,{}};sub(ss)={cut_vertex,original.vertex_b,xy[1].x,xy[1].y,right,{}};
            vertex(original.vertex_a).subcurves.push_back(ps);vertex(cut_vertex).subcurves.push_back(ps);vertex(cut_vertex).subcurves.push_back(ss);vertex(original.vertex_b).subcurves.push_back(ss);reference(6);
            if(old.line>=0&&!(old.flags&2))width_split(original,ps,ss,q);
            remove(vertex(original.vertex_a).subcurves,sc_id);remove(vertex(original.vertex_b).subcurves,sc_id);for(auto id:original.scales)erase(a.scales,1,id);erase(a.subcurves,2,sc_id);ls.push_back(ps);rs.insert(rs.begin(),ss);
        }
        model(left).subcurves=ls;model(right).subcurves=rs;for(auto id:ls)sub(id).model=left;for(auto id:rs)sub(id).model=right;
        if(old.line>=0){const auto& members=line(old.line).members;const auto member=std::ranges::find(members,model_id,&VectorLineMember::model);if(member==members.end())throw std::invalid_argument("LCOL cut MC has no LC");const bool forward=member->selector!=0;replace(line(old.line).members,model_id,forward?left:right,forward?right:left,false);}
        for(int lane=0;lane<2;++lane)if(old.targets[std::size_t(lane)]>=0){if(!paint)throw std::invalid_argument("LCOL cut side Loop requires Paint graph");auto& loop=old.target_kinds[std::size_t(lane)]?value(paint->regions,old.targets[std::size_t(lane)]).outer:value(paint->loops,old.targets[std::size_t(lane)]);replace(loop.members,model_id,lane?left:right,lane?right:left,true,lane);}
        model(model_id).crosspoints={-1,-1};if(pos==0)vector_edit_detail::retained_endpoint(a,layer,left,true,wire,nodes,refs,storage,budget.work,budget.stop);if(pos+1==old.subcurves.size())vector_edit_detail::retained_endpoint(a,layer,right,false,wire,nodes,refs,storage,budget.work,budget.stop);
        slots[4].reset();
        vector_geometry_detail::rebuild_models(a,{left,right},budget.work,budget.stop);remove(owner.models,model_id);erase(a.models,3,model_id);return {true,cut_vertex,left,right};
    }
    void repair_vertex(int vertex_id,std::size_t plane,const std::map<int,std::size_t>& planes){
        // EndpointEngine has its own CP cursor. Reconstruct our cursor after
        // that visitor, including failed candidate edits, from live runtime H.
        slots[4].reset();
        try{vector_edit_detail::repair_vertex_endpoints(a,layer,vertex_id,plane,planes,wire,nodes,refs,storage,budget.work,budget.stop);}catch(...){slots[4].reset();throw;}
        slots[4].reset();
    }
};
// Mutable counterparts of5B92F0/5B9830/5BA1F0/5BA580. Owner lists and pool
// cursors belong to the caller and persist across all cuts in one operation.
void normalize(Editor&,int line,const std::map<int,std::size_t>& line_planes);
void weld(Editor&,int keep_vertex,int drop_vertex);
void clear_endpoint(Editor&,int model,bool at_start);
void split_subcurve(Editor&,int subcurve,float parameter,P point,bool copy_stored_width=false);
//59C4E0 followed by545800. Only remaining members are deeply released.
void erase_line(Editor&,int line);
}
