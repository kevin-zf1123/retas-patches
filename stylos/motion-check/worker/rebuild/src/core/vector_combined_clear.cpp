#include "vector_combined_clear.hpp"
#include "vector_grouped_face_delete.hpp"
#include "vector_line_detach.hpp"
#include "vector_fill_boundary.hpp"
#include <bit>
#include <cmath>

namespace retas::vector_clear_detail {
namespace {
using namespace vector_graph_detail;
constexpr auto max_storage=vector_material_detail::max_bytes;
bool pure(std::uint32_t f){return f==0x21110001||f==0x21120001;}
bool vert(std::uint32_t f){return f==0x21210001||f==0x21220001;}
bool paint(std::uint32_t f){return f==0x22100001||f==0x22100002;}
//5DA2F0 calls5B7760 without59E460. This private engine intentionally has
//no saved-MC or LC cache path, including no cache-only budget or exceptions.
namespace spatial_endpoint {
using numeric_detail::binary64;
void charge(std::uint64_t& n,std::uint64_t add,std::uint64_t cap){vector_material_detail::charge(n,add,cap,"Combined Clear endpoint budget exceeded");}
void poll(std::stop_token stop){vector_material_detail::poll(stop);}
float f32(double v){const float f=float(v);if(!std::isfinite(f))throw std::invalid_argument("Nonfinite vector endpoint scalar");return f;}
int integer(double v){if(!std::isfinite(v)||v<INT32_MIN||v>INT32_MAX)throw std::overflow_error("Vector endpoint coordinate overflow");return int(v);}
VectorPoint point(const VectorVertex& v){if(std::abs(std::int64_t(v.x))>134217727||std::abs(std::int64_t(v.y))>134217727)throw std::invalid_argument("Vector endpoint source exceeds raw domain");return {double(v.x),double(v.y)};}
template<class T> auto index(std::vector<VectorPoolRecord<T>>& pool){std::map<int,T*> out;for(auto& r:pool)out.emplace(r.index,&r.value);return out;}
float angle(VectorPoint d){double a;if(d.x>0){a=binary64(std::atan(binary64(d.y/d.x))*double(57.295776f));if(a<0)a=binary64(a+360);}else if(d.x<0)a=binary64(binary64(std::atan(binary64(d.y/d.x))*double(57.295776f))+180);else a=d.y<0?270:90;return f32(a);}
VectorPoint direction(float a){const auto r=binary64(double(a)*double(0.017453292f));return {f32(std::cos(r)),f32(std::sin(r))};}
VectorPoint unit(VectorPoint d){const auto inverse=binary64(1/std::sqrt(binary64(binary64(d.x*d.x)+binary64(d.y*d.y))));return {f32(binary64(d.x*inverse)),f32(binary64(d.y*inverse))};}
bool flat(VectorPoint a,VectorPoint c,VectorPoint e){
    const auto dx=e.x-a.x,dy=e.y-a.y;const auto square=std::min(binary64(binary64(dx*dx)+binary64(dy*dy)),double(INT32_MAX));
    if(square==0)return binary64(binary64((c.x-a.x)*(c.x-a.x))+binary64((c.y-a.y)*(c.y-a.y)))<=4;
    const auto cross=binary64(binary64((c.y-a.y)*dx)-binary64((c.x-a.x)*dy));return binary64(binary64(cross*cross)/square)<=4;
}
std::optional<float> inverse(VectorPoint a,VectorPoint c,VectorPoint e,VectorPoint q,bool linear){
    const bool x=std::abs(e.x-a.x)>std::abs(e.y-a.y);const double start=x?a.x:a.y,end=x?e.x:e.y,control=x?c.x:c.y,query=x?q.x:q.y;
    if(linear){const float t=f32(binary64(binary64(query-start)/binary64(end-start)));if(t>=0&&t<=1)return t;return {};}
    const auto A=integer(end-2*control+start),B=integer(2*(control-start));const auto C=binary64(start-query);
    if(!A){if(!B)return {};const auto t=binary64(-C/B);return t>=0&&t<=1?std::optional<float>{f32(t)}:std::nullopt;}
    const auto D=binary64(binary64(double(B)*B)-binary64(binary64(double(A)*C)*4));if(D<0)return {};
    if(D==0){const auto t=binary64(-binary64(double(B)/binary64(double(A)+A)));return t>=0&&t<=1?std::optional<float>{f32(t)}:std::nullopt;}
    const auto root=std::sqrt(D),k=binary64(.5/A);
    for(const auto t:{binary64(binary64(root-B)*k),binary64(binary64(-double(B)-root)*k)})if(t>=0&&t<=1)return f32(t);return {};
}
VectorPoint evaluate(VectorPoint a,VectorPoint c,VectorPoint e,float t,bool linear){
    const double u=binary64(1-double(t));if(linear)return {binary64(binary64(a.x*u)+binary64(e.x*t)),binary64(binary64(a.y*u)+binary64(e.y*t))};
    const float saved=f32(u);const auto aa=binary64(u*saved),cc=binary64(binary64(double(saved)*t)+binary64(double(saved)*t)),ee=binary64(double(t)*t);
    return {binary64(binary64(binary64(e.x*ee)+binary64(c.x*cc))+binary64(a.x*aa)),binary64(binary64(binary64(e.y*ee)+binary64(c.y*cc))+binary64(a.y*aa))};
}
VectorPoint tangent(VectorPoint a,VectorPoint c,VectorPoint e,float t,bool linear){if(linear||c==a||c==e)return unit({e.x-a.x,e.y-a.y});
    const auto u=binary64(double(t)-1),v=binary64(1-binary64(double(t)+t));return unit({binary64(binary64(binary64(e.x*t)+binary64(c.x*v))+binary64(a.x*u)),binary64(binary64(binary64(e.y*t)+binary64(c.y*v))+binary64(a.y*u))});}
void xy(VectorCrosspoint& cp,int at,VectorPoint p){cp.words[at]=std::bit_cast<std::uint32_t>(std::int32_t(integer(p.x)));cp.words[at+1]=std::bit_cast<std::uint32_t>(std::int32_t(integer(p.y)));}
void endpoint_without_cache(VectorLineArchive& a,Layer& layer,int model_id,bool at_start,std::uint32_t half_width,std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& refs,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
    poll(stop);charge(work,32*(a.vertices.size()+a.subcurves.size()+a.models.size()+a.scales.size()+a.crosspoints.size()+1),128ull*1024*1024);
    charge(storage,64*(a.vertices.size()+a.subcurves.size()+a.models.size()+a.scales.size()+1),512ull*1024*1024);
    const auto vertices=index(a.vertices);const auto subs=index(a.subcurves);const auto models=index(a.models);const auto scales=index(a.scales);
    auto& m=*models.at(model_id);if(m.subcurves.empty())throw std::invalid_argument("Endpoint rewrite requires a nonempty model");
    const int sub_id=at_start?m.subcurves.front():m.subcurves.back();const auto& sub=*subs.at(sub_id);const int vertex_id=at_start?sub.vertex_a:sub.vertex_b;const auto& vertex=*vertices.at(vertex_id);
    const auto center=point(vertex),start=point(*vertices.at(sub.vertex_a)),end=point(*vertices.at(sub.vertex_b));const VectorPoint control{double(sub.control_x),double(sub.control_y)};if(std::abs(control.x)>134217727||std::abs(control.y)>134217727)throw std::invalid_argument("Endpoint control exceeds raw domain");
    const float base=std::bit_cast<float>(half_width);if(!std::isfinite(base))throw std::invalid_argument("Invalid endpoint base width");double width=base;
    if(!(m.flags&2)){if(sub.scales.empty())throw std::invalid_argument("Variable endpoint has no width node");const float scale=std::bit_cast<float>(scales.at(at_start?sub.scales.front():sub.scales.back())->scale_bits);if(!std::isfinite(scale))throw std::invalid_argument("Invalid endpoint node width");width=binary64(width*scale);}
    const float radius=f32(width);const int side=at_start?0:1;int cp_id=m.crosspoints[side];
    if(cp_id<0){charge(storage,64*(a.crosspoints.size()+1),512ull*1024*1024);std::set<int> live;int high=-1;for(const auto& r:a.crosspoints){live.insert(r.index);high=std::max(high,r.index);}if(const auto h=layer.runtime_vector_high_water[4]){if(*h<high||*h<-1)throw std::invalid_argument("Invalid live crosspoint H");high=*h;}int cursor=0;while(live.contains(cursor)){poll(stop);charge(work,32,128ull*1024*1024);++cursor;}
        charge(wire,64,8ull*1024*1024);charge(nodes,1,4096);charge(refs,1,8192);charge(storage,512,512ull*1024*1024);cp_id=cursor;if(std::int64_t(cursor)==std::int64_t(high)+1)high=cursor;a.crosspoints.push_back({cp_id,{}, {}});std::ranges::sort(a.crosspoints,{},&VectorPoolRecord<VectorCrosspoint>::index);a.high_water[4]=high;layer.runtime_vector_high_water[4]=high;m.crosspoints[side]=cp_id;}
    auto& cp=std::ranges::find(a.crosspoints,cp_id,&VectorPoolRecord<VectorCrosspoint>::index)->value;m.flags|=at_start?0x10u:0x20u;
    //5ABF20 map<float,SC*>: nullable nodes skip and later duplicate angles win.
    std::map<float,int> fan;for(auto id:vertex.subcurves){poll(stop);charge(work,32,128ull*1024*1024);if(id<0)continue;const auto& s=*subs.at(id);const auto va=point(*vertices.at(s.vertex_a)),vb=point(*vertices.at(s.vertex_b));const VectorPoint c{double(s.control_x),double(s.control_y)};
        if(s.vertex_a!=vertex_id&&s.vertex_b!=vertex_id)throw std::invalid_argument("Endpoint adjacency has a nonincident span");const auto other=((models.at(s.model)->flags&1)||flat(va,c,vb))?(s.vertex_a==vertex_id?vb:va):c;fan[angle({other.x-center.x,other.y-center.y})]=id;}
    float heading{},next{},previous{};const auto found=std::ranges::find_if(fan,[&](const auto& v){return v.second==sub_id;});if(found!=fan.end()){heading=found->first;auto n=std::next(found);if(n==fan.end())n=fan.begin();auto p=found==fan.begin()?std::prev(fan.end()):std::prev(found);next=n->first;previous=p->first;}
    const double opposite=binary64(double(heading)+180);float reverse=f32(opposite);if(opposite>=360)reverse=f32(binary64(double(reverse)-360));const auto d=direction(reverse);const VectorPoint weighted{f32(binary64(d.x*radius)),f32(binary64(d.y*radius))};
    if(next==previous){double n=binary64(double(reverse)-90);if(n<0)n=binary64(n+360);next=f32(n);const double p=binary64(double(reverse)+90);previous=f32(p);if(p>=360)previous=f32(binary64(double(previous)-360));}
    const VectorCrosspoint* other=nullptr;if(m.subcurves.size()==1&&m.crosspoints[1-side]>=0)other=&std::ranges::find(a.crosspoints,m.crosspoints[1-side],&VectorPoolRecord<VectorCrosspoint>::index)->value;
    for(int lane=0;lane<2;++lane){float degrees=f32(lane?binary64(binary64(double(reverse)-previous)*-1):binary64(double(reverse)-next));if(degrees<0)degrees=f32(binary64(double(degrees)+360));if(degrees>180)degrees=f32(binary64(double(degrees)-180));degrees=std::clamp(degrees,30.f,150.f);
        if(degrees<90){const auto cot=degrees<89?binary64(1/std::tan(binary64(double(degrees)*double(0.017453292f)))):0;const double x=lane?-weighted.y:weighted.y,y=lane?weighted.x:-weighted.x;
            const auto ox=binary64(binary64(cot*weighted.x)+x);const float oy=f32(binary64(binary64(cot*weighted.y)+y));cp.words[lane]=3;xy(cp,lane?12:4,{center.x+f32(std::floor(ox)),center.y+std::floor(oy)});
        }else{const auto cot=degrees>91?binary64(1/std::tan(binary64(double(degrees)*double(0.017453292f)))):0;const VectorPoint query{binary64(binary64(cot*weighted.x)+center.x),binary64(binary64(cot*weighted.y)+center.y)};
            float t=inverse(start,control,end,query,m.flags&1).value_or(at_start?1.f:0.f);if(other&&other->words[1-lane]==1){const float trim=std::bit_cast<float>(other->words[3-lane]);if(trim>0)t=at_start?std::min(t,trim):std::max(t,trim);}
            const auto p=evaluate(start,control,end,t,m.flags&1),u=tangent(start,control,end,t,m.flags&1);const bool flip=(lane==0)==at_start;const double nx=flip?-u.y:u.y,ny=flip?u.x:-u.x;
            cp.words[lane]=1;cp.words[2+lane]=std::bit_cast<std::uint32_t>(t);xy(cp,lane?12:4,{std::floor(binary64(binary64(nx*radius)+p.x)),std::floor(binary64(binary64(ny*radius)+p.y))});xy(cp,8,center);}
    }
    poll(stop);
}
}
struct Source {int id;VectorLine line;bool accepted;};
struct Target {std::size_t plane;OwnerLists links;std::vector<Source> sources;};
int side_region(Editor& e,int model,unsigned side,bool repeated){
    const auto& m=e.model(model);
    if(m.target_kinds[side]<0||m.target_kinds[side]>1||m.targets[side]<0)
        throw std::invalid_argument("Combined Clear requires complete MC side ownership");
    const auto& loop=m.target_kinds[side]?e.value(e.paint->regions,m.targets[side]).outer:e.value(e.paint->loops,m.targets[side]);
    const auto& owner=e.value(e.paint->regions,loop.region);
    if(m.target_kinds[side]?loop.region!=m.targets[side]:std::ranges::count(owner.holes,m.targets[side])!=1)
        throw std::invalid_argument("Combined Clear stale side Region membership");
    const auto count=std::ranges::count_if(loop.members,[&](auto item){return item.model==model&&bool(item.selector)==bool(side);});
    if(repeated?count<1:count!=1)throw std::invalid_argument("Combined Clear nonreciprocal side Loop");
    return loop.region;
}
void admit_model(Editor& e,int id,bool repeated){
    const auto& m=e.model(id);
    if(m.subcurves.empty())throw std::invalid_argument("Combined Clear empty MC geometry");
    charge(e.storage,64*(m.subcurves.size()+1),max_storage,"Combined Clear geometry admission storage limit");
    std::set<int> seen;int previous=-1;
    for(int sub:m.subcurves){e.budget.charge(32);const auto& s=e.sub(sub);
        if(!seen.insert(sub).second||s.model!=id||(previous>=0&&s.vertex_a!=previous))
            throw std::invalid_argument("Combined Clear stale SC ownership");
        for(int vertex:{s.vertex_a,s.vertex_b}){
            const auto& v=e.vertex(vertex);if(std::ranges::count(v.subcurves,sub)!=1)
                throw std::invalid_argument("Combined Clear stale Vertex adjacency");
            if(std::abs(std::int64_t(v.x))>134217727||std::abs(std::int64_t(v.y))>134217727)
                throw std::invalid_argument("Combined Clear geometry outside raw domain");
        }
        if(std::abs(std::int64_t(s.control_x))>134217727||std::abs(std::int64_t(s.control_y))>134217727)
            throw std::invalid_argument("Combined Clear control outside raw domain");
        for(int scale:s.scales)if(!std::isfinite(std::bit_cast<float>(e.value(e.a.scales,scale).scale_bits)))
            throw std::invalid_argument("Combined Clear nonfinite stored width");
        previous=s.vertex_b;
    }
    for(int cp:m.crosspoints)if(cp>=0){(void)e.value(e.a.crosspoints,cp);std::size_t count{};
        for(const auto& other:e.a.models){e.budget.charge(2);count+=std::ranges::count(other.value.crosspoints,cp);}
        if(count!=1)throw std::invalid_argument("Combined Clear competing CP ownership");}
    for(unsigned side=0;side<2;++side)(void)side_region(e,id,side,repeated);
}
std::vector<Target> admit_targets(Editor& e,vector_boundary_detail::Integrator& context,
    const std::vector<std::size_t>& selected,bool full,std::vector<std::size_t>& paints,bool spatial=false){
    const auto owners=context.line_planes();std::set<std::size_t> unique;std::vector<Target> targets;
    targets.reserve(selected.size());paints.reserve(selected.size());
    for(auto plane:selected){e.budget.charge();
        if(plane>=e.layer.planes.size()||!unique.insert(plane).second)
            throw std::invalid_argument("Combined Clear invalid selected plane occurrence");
        const auto& child=e.layer.planes[plane];
        if((!pure(child.family)&&!vert(child.family)&&!paint(child.family))||child.opaque||child.raster||child.common.width||child.common.height||child.common.depth
            ||child.common.resolution!=numeric_detail::binary64(e.layer.common.resolution*16))
            throw std::invalid_argument("Combined Clear requires native same-DPI siblings");
        if(paint(child.family)){paints.push_back(plane);continue;}
        charge(e.storage,4*child.specific.size()+8*(e.paint->border.vertices.size()+e.paint->border.models.size())+sizeof(Target),max_storage,"Combined Clear owner staging storage limit");
        Target target{plane,OwnerLists(child,e.paint),{}};
        charge(e.storage,sizeof(Source)*target.links.lines.size(),max_storage,"Combined Clear source staging storage limit");
        target.sources.reserve(target.links.lines.size());std::set<int> lines;
        for(int id:target.links.lines){e.budget.charge();const auto found=owners.find(id);
            if(!lines.insert(id).second||found==owners.end()||found->second!=plane)
                throw std::invalid_argument("Combined Clear competing LC owner");
            const auto& line=e.line(id);
            if(!line.members.empty()&&!std::isfinite(std::bit_cast<float>(line.half_width_bits)))
                throw std::invalid_argument("Combined Clear nonfinite LC width");
            charge(e.storage,64*(line.members.size()+1),max_storage,"Combined Clear frozen LC storage limit");
            bool accepted=full&&!line.members.empty();if(vert(child.family)&&!spatial)for(auto item:line.members){e.budget.charge();accepted|=bool(e.model(item.model).flags&0x10000u);}std::set<int> members;
            for(auto item:line.members){e.budget.charge(32);const auto& m=e.model(item.model);
                const bool first=members.insert(item.model).second;
                if((!full&&!first)||m.line!=id)throw std::invalid_argument("Combined Clear repeated MC or stale LC backlink");
                if(first&&(!vert(child.family)||(!spatial&&accepted)))admit_model(e,item.model,full);accepted|=bool(m.flags&0x10000u);
            }
            target.sources.push_back({id,line,accepted});
        }
        targets.push_back(std::move(target));
    }
    return targets;
}
void admit_faces(Editor& e,vector_boundary_detail::Integrator& context){
    vector_fill_detail::validate_paint_owners(*e.paint,e.layer,e.budget);
    std::set<int> regions,faces;
    for(int id:e.paint->border.regions){e.budget.charge();
        if(!regions.insert(id).second)throw std::invalid_argument("Combined Clear repeated Border Region");
        const auto& r=e.value(e.paint->regions,id);
        if(r.outer.region!=id)throw std::invalid_argument("Combined Clear stale embedded Region owner");
        for(int face:r.faces){e.budget.charge();const auto& f=e.value(e.paint->faces,face);
            if(!faces.insert(face).second||f.region!=id||!context.face_planes().contains(face))
                throw std::invalid_argument("Combined Clear stale Face Region ownership");
            if(f.material>=0&&((f.flags&15)==2|| (f.flags&15)==4)){
                const bool gradient=(f.flags&15)==2;const auto plane=context.face_planes().at(face);
                vector_material_detail::PaintLists lists(e.layer.planes[plane]);
                if(std::ranges::count(lists.ids[gradient?1:2],f.material)!=1)
                    throw std::invalid_argument("Combined Clear material differs from actual Face plane");
                const auto& bytes=gradient?e.value(e.paint->gradations,f.material):e.value(e.paint->textures,f.material);
                charge(e.storage,2*bytes.size()+128,max_storage,"Combined Clear material admission storage limit");e.budget.charge(4*bytes.size()+32);
                const auto back=gradient?decode_vector_gradation(bytes).faces:decode_vector_texture(bytes).faces;
                if(std::ranges::count(back,face)!=1)throw std::invalid_argument("Combined Clear nonreciprocal material Face ownership");
                if(gradient){const auto g=decode_vector_gradation(bytes);std::set<int> stops;
                    for(int node:g.stops){e.budget.charge();if(node<0||!stops.insert(node).second)throw std::invalid_argument("Combined Clear repeated gradient stop ownership");(void)e.value(e.paint->gradnodes,node);}
                    for(const auto& other:e.paint->gradations)if(other.index!=f.material){e.budget.charge(4*other.value.size()+32);for(int node:decode_vector_gradation(other.value).stops)if(stops.contains(node))throw std::invalid_argument("Combined Clear competing gradient stops");}
                }
            }
        }
    }
    if(faces.size()!=e.paint->faces.size()||context.face_planes().size()!=faces.size())
        throw std::invalid_argument("Combined Clear unowned Face records");
}
std::vector<int> live_regions(Editor& e,const std::vector<int>& pending){
    charge(e.storage,4*pending.size(),max_storage,"Combined Clear live Region staging limit");std::vector<int> out;out.reserve(pending.size());
    for(int id:pending){e.budget.charge(e.paint->regions.size()+1);if(std::ranges::find(e.paint->regions,id,&VectorPoolRecord<VectorArchiveRegion>::index)!=e.paint->regions.end())out.push_back(id);}
    return out;
}
std::uint64_t face_node_refunds(Editor& e,vector_boundary_detail::Integrator& context,const std::vector<int>& pending,const std::vector<std::size_t>& selected){
    std::set<std::size_t> chosen(selected.begin(),selected.end());std::set<int> erased;std::uint64_t nodes{};
    for(int id:pending)for(int face:e.value(e.paint->regions,id).faces){e.budget.charge();if(chosen.contains(context.face_planes().at(face)))erased.insert(face);}
    nodes=erased.size();
    const auto detached=[&](int face,int material,unsigned kind){
        if(!erased.contains(face))return false;
        const auto found=std::ranges::find(e.paint->faces,face,&VectorPoolRecord<VectorArchiveFace>::index);
        return found!=e.paint->faces.end()&&(found->value.flags&15)==kind&&found->value.material==material;
    };
    for(const auto& row:e.paint->gradations){e.budget.charge(4*row.value.size()+32);charge(e.storage,2*row.value.size()+128,max_storage,"Combined Clear projected material staging limit");const auto g=decode_vector_gradation(row.value);
        if(!g.faces.empty()&&std::ranges::all_of(g.faces,[&](int id){return detached(id,row.index,2);})){nodes+=g.stops.size()+1;}}
    for(const auto& row:e.paint->textures){e.budget.charge(4*row.value.size()+32);charge(e.storage,2*row.value.size()+128,max_storage,"Combined Clear projected texture staging limit");const auto t=decode_vector_texture(row.value);
        if(!t.faces.empty()&&std::ranges::all_of(t.faces,[&](int id){return detached(id,row.index,4);}))++nodes;}
    return nodes;
}
std::uint64_t face_reference_projection(Editor& e,vector_boundary_detail::Integrator& context,
    const std::vector<int>& pending,const std::vector<std::size_t>& selected){
    // MaterialDetach admits its raw intrusive lists on each Face detach.
    // Simulate that exact physical order, including shared-material repeats,
    // before any Full premark or Face deletion; creators consume it later.
    struct Material {std::vector<int> faces;std::size_t stops{},bytes{};};
    std::map<std::pair<unsigned,int>,Material> materials;
    std::set<std::size_t> chosen(selected.begin(),selected.end());std::set<int> erased;
    auto refs=e.refs,storage=e.storage;
    for(int region:pending)for(int id:e.value(e.paint->regions,region).faces){e.budget.charge();
        if(!chosen.contains(context.face_planes().at(id))||!erased.insert(id).second)continue;
        const auto& face=e.value(e.paint->faces,id);const unsigned kind=face.flags&15;
        if((kind!=2&&kind!=4)||face.material<0)continue;
        const auto key=std::pair{kind,face.material};auto found=materials.find(key);
        if(found==materials.end()){
            const auto& raw=kind==2?e.value(e.paint->gradations,face.material):e.value(e.paint->textures,face.material);
            Material item;item.bytes=raw.size();e.budget.charge(4*raw.size()+32);
            charge(e.storage,2*raw.size()+128,max_storage,"Combined Clear material ref simulation storage limit");
            if(kind==2){auto g=decode_vector_gradation(raw);item.faces=std::move(g.faces);item.stops=g.stops.size();}
            else item.faces=decode_vector_texture(raw).faces;
            found=materials.emplace(key,std::move(item)).first;
        }
        auto& material=found->second;
        charge(refs,material.faces.size()+material.stops,8192,"Combined Clear projected material reference limit");
        storage=std::max(storage,e.storage);charge(storage,2*material.bytes+128,max_storage,"Combined Clear projected material detach storage limit");
        const auto back=std::ranges::find(material.faces,id);
        if(back!=material.faces.end()){
            material.faces.erase(back);--refs;
            if(material.faces.empty()){
                if(refs<material.stops+1)throw std::invalid_argument("Combined Clear material reference ledger underflow");
                refs-=material.stops+1;
            }
        }
    }
    return refs;
}
std::vector<int> marked_regions(Editor& e,const std::set<std::size_t>& premark,vector_boundary_detail::Integrator& context){
    std::vector<int> result;result.reserve(e.paint->border.regions.size());
    for(int id:e.paint->border.regions){e.budget.charge();const auto& r=e.value(e.paint->regions,id);
        if(std::ranges::any_of(r.faces,[&](int face){return bool(e.value(e.paint->faces,face).flags&0x10000u)||premark.contains(context.face_planes().at(face));}))result.push_back(id);}
    return result;
}
void marked_preflight(Editor& e,const std::vector<Target>& targets,const std::vector<int>& pending,bool full,std::uint64_t refunds,std::uint64_t face_refs){
    std::uint64_t fresh_lines{},retained{},visits{},selected{};std::set<std::pair<int,bool>> corners;std::set<int> regions(pending.begin(),pending.end());
    if(refunds>e.nodes)throw std::invalid_argument("Combined Clear invalid projected Face refunds");
    auto live=e.nodes-refunds,peak=live;
    for(const auto& target:targets)for(const auto& source:target.sources){e.budget.charge();if(!source.accepted)continue;
        const auto before=fresh_lines+corners.size();bool first=true,previous{};
        for(auto member:source.line.members){const auto& m=e.model(member.model);const bool mark=full||bool(m.flags&0x10000u);
            if(mark){++selected;for(unsigned side=0;side<2;++side)regions.insert(side_region(e,member.model,side,full));}
            else{++retained;if(first||previous)++fresh_lines;}
            if(!first&&mark!=previous){++visits;if(m.crosspoints[mark?1:0]<0)corners.emplace(member.model,!mark);}
            first=false;previous=mark;
        }
        charge(live,fresh_lines+corners.size()-before,4096,"Combined Clear projected live record limit");peak=std::max(peak,live);if(!live)throw std::invalid_argument("Combined Clear missing LC record ledger");--live;
    }
    auto wire=e.wire,refs=face_refs,storage=e.storage;
    charge(wire,136*fresh_lines+8*retained+64*corners.size(),8ull*1024*1024,"Combined Clear projected wire limit");
    charge(refs,fresh_lines+retained+corners.size(),8192,"Combined Clear projected reference limit");
    charge(peak,0,4096,"Combined Clear projected record limit");
    charge(e.storage,32*(retained+fresh_lines+regions.size()+1)+8*selected,max_storage,"Combined Clear marked staging storage limit");
    const auto corner_scan=corners.size()*(e.a.crosspoints.size()+corners.size());
    charge(e.storage,64*corner_scan,max_storage,"Combined Clear marked CP cursor staging limit");e.budget.charge(32*corner_scan);
    const auto cache_records=e.a.vertices.size()+e.a.scales.size()+e.a.subcurves.size()+e.a.models.size()+e.a.crosspoints.size()+e.a.lines.size()+fresh_lines+corners.size()+1;
    charge(e.storage,64*fresh_lines*cache_records,max_storage,"Combined Clear retained LC cache staging limit");e.budget.charge(32*fresh_lines*cache_records);
    storage=e.storage;
    charge(storage,512*(fresh_lines+corners.size())+128*visits*(e.a.vertices.size()+e.a.subcurves.size()+e.a.models.size()+e.a.scales.size()+corners.size()+1),max_storage,"Combined Clear projected endpoint storage limit");
    if(pending.size()+2*selected>8192)throw std::length_error("Combined Clear marked pending occurrence limit");
}
void trim_endpoint(Editor& e,VectorLineMember incoming,bool first,std::uint32_t width){
    e.slots[4].reset();
    try{vector_edit_detail::partition_endpoint(e.a,e.layer,incoming.model,first,first?incoming.selector!=0:incoming.selector==0,width,
        e.wire,e.nodes,e.refs,e.storage,e.budget.work,e.budget.stop);}catch(...){e.slots[4].reset();throw;}
    e.slots[4].reset();
}
void marked_pure(Editor& e,std::vector<Target>& targets,bool full,std::vector<int>& pending,CombinedClearResult& result){
    for(auto& target:targets){e.owner.lines=target.links.lines;
        for(const auto& source:target.sources){e.budget.charge();if(!source.accepted)continue;
            unite(result.changed,source.line.archived_bounds);unite(result.context,source.line.archived_bounds);
            if(full){for(auto member:source.line.members){for(unsigned side=0;side<2;++side)pending.push_back(side_region(e,member.model,side,true));e.model(member.model).line=-1;}}
            else{
                bool first=true,previous{};int current=-1;
                const auto finish=[&]{if(current>=0){e.cache(current,false);unite(result.context,e.line(current).archived_bounds);current=-1;}};
                for(auto member:source.line.members){const bool mark=bool(e.model(member.model).flags&0x10000u),transition=!first&&mark!=previous;
                    if(first){if(!mark)current=e.new_line(source.line);}
                    else if(transition){if(current>=0)trim_endpoint(e,member,false,source.line.half_width_bits);finish();if(!mark)current=e.new_line(source.line,source.id);}
                    if(mark){for(unsigned side=0;side<2;++side)pending.push_back(side_region(e,member.model,side,false));e.model(member.model).line=-1;}
                    else{e.reference();e.line(current).members.push_back(member);e.model(member.model).line=current;if(transition)trim_endpoint(e,member,true,source.line.half_width_bits);}
                    first=false;previous=mark;
                }
                finish();
            }
            if(e.refs<source.line.members.size()+1)throw std::invalid_argument("Combined Clear missing LC reference ledger");
            e.refs-=source.line.members.size()+1;e.line(source.id).members.clear();Editor::remove(e.owner.lines,source.id);e.erase(e.a.lines,5,source.id);
            result.performed=true;++result.erased_lines;
        }
        target.links.lines=e.owner.lines;if(!target.links.vertex_plane){target.links.vertices=e.owner.vertices;target.links.models=e.owner.models;}target.links.publish(e.layer.planes[target.plane],e.paint);
    }
}
std::vector<int> region_occurrences(Editor& e,const std::vector<int>& regions){
    std::uint64_t count{};
    for(int id:regions){e.budget.charge(e.paint->regions.size()+1);if(std::ranges::find(e.paint->regions,id,&VectorPoolRecord<VectorArchiveRegion>::index)==e.paint->regions.end())continue;const auto& r=e.value(e.paint->regions,id);count+=r.outer.members.size();for(int hole:r.holes)count+=e.value(e.paint->loops,hole).members.size();}
    if(count>8192)throw std::length_error("Combined Clear Region occurrence limit");
    charge(e.storage,8*(count+1),max_storage,"Combined Clear Region occurrence staging limit");
    std::vector<int> out;out.reserve(std::size_t(count));
    for(int id:regions){e.budget.charge(e.paint->regions.size()+1);if(std::ranges::find(e.paint->regions,id,&VectorPoolRecord<VectorArchiveRegion>::index)==e.paint->regions.end())continue;const auto& r=e.value(e.paint->regions,id);for(auto m:r.outer.members)out.push_back(m.model);for(int hole:r.holes)for(auto m:e.value(e.paint->loops,hole).members)out.push_back(m.model);}
    return out;
}
struct SpatialPlan {std::vector<int> occurrences;bool all{};};
SpatialPlan spatial_preflight(Editor& e,vector_boundary_detail::Integrator& context,
    const std::vector<int>& regions,const std::vector<Target>& targets,std::uint64_t refunds,std::uint64_t face_refs){
    SpatialPlan plan;plan.occurrences=region_occurrences(e,regions);std::set<std::size_t> selected;
    for(const auto& target:targets)selected.insert(target.plane);
    std::size_t total{};for(const auto& plane:e.layer.planes){e.budget.charge();total+=(plane.family&0xff000000u)==0x21000000u;}
    plan.all=selected.size()==total;const auto owners=context.line_planes();
    charge(e.storage,128*(e.a.lines.size()+e.a.models.size()+plan.occurrences.size()+1),max_storage,"Combined Clear spatial simulation staging limit");
    std::map<int,VectorLine> lines;std::map<int,int> model_lines;std::map<int,std::size_t> planes=owners;
    for(const auto& row:e.a.lines){charge(e.storage,32*(row.value.members.size()+1),max_storage,"Combined Clear spatial LC simulation limit");lines.emplace(row.index,row.value);for(auto member:row.value.members)model_lines.emplace(member.model,row.index);}
    std::vector<std::pair<int,bool>> repairs;repairs.reserve(plan.occurrences.size()*2);std::set<int> admitted;
    std::uint64_t fresh{},cache_visits{};if(refunds>e.nodes)throw std::invalid_argument("Combined Clear invalid spatial Face refunds");auto live=e.nodes-refunds,peak=live;int temporary=-2;
    for(int mc:plan.occurrences){e.budget.charge(32);if(admitted.insert(mc).second)admit_model(e,mc,false);
        const auto at=model_lines.find(mc);if(at==model_lines.end())continue;const int source=at->second;
        if(!plan.all&&!selected.contains(planes.at(source)))continue;
        auto& line=lines.at(source);const auto member=std::ranges::find(line.members,mc,&VectorLineMember::model);
        if(member==line.members.end())throw std::invalid_argument("Combined Clear missing simulated LC member");
        const auto pos=std::size_t(member-line.members.begin());
        if(pos){const auto peer=line.members[pos-1];repairs.emplace_back(peer.model,peer.selector==0);}
        if(pos+1<line.members.size()){const auto peer=line.members[pos+1];repairs.emplace_back(peer.model,peer.selector!=0);}
        model_lines.erase(mc);
        if(pos&&pos+1<line.members.size()){
            VectorLine suffix=line;suffix.members.assign(std::next(member),line.members.end());line.members.resize(pos);
            const int id=temporary--;planes.emplace(id,planes.at(source));for(auto item:suffix.members)model_lines[item.model]=id;lines.emplace(id,std::move(suffix));
            ++fresh;cache_visits+=2;charge(live,1,4096,"Combined Clear spatial projected records");peak=std::max(peak,live);
        }else{line.members.erase(member);if(line.members.empty()){lines.erase(source);planes.erase(source);--live;}else ++cache_visits;}
    }
    std::set<std::pair<int,bool>> corners;std::uint64_t endpoint_visits{};
    for(auto item:repairs)if(model_lines.contains(item.first)){++endpoint_visits;if(e.model(item.first).crosspoints[item.second?0:1]<0)corners.insert(item);}
    charge(live,corners.size(),4096,"Combined Clear spatial projected CP records");peak=std::max(peak,live);
    auto wire=e.wire,refs=face_refs,storage=e.storage;
    charge(wire,136*fresh+64*corners.size(),8ull*1024*1024,"Combined Clear spatial projected wire limit");
    charge(refs,fresh+corners.size(),8192,"Combined Clear spatial projected reference limit");
    charge(peak,0,4096,"Combined Clear spatial projected record limit");
    const auto records=e.a.vertices.size()+e.a.subcurves.size()+e.a.models.size()+e.a.scales.size()+e.a.crosspoints.size()+e.a.lines.size()+fresh+corners.size()+1;
    charge(e.storage,64*cache_visits*records,max_storage,"Combined Clear spatial LC cache staging limit");e.budget.charge(32*cache_visits*records);
    storage=e.storage;charge(storage,512*(fresh+corners.size())+128*endpoint_visits*records,max_storage,"Combined Clear spatial endpoint projection storage limit");
    return plan;
}
void spatial_pure(Editor& e,vector_boundary_detail::Integrator& context,const SpatialPlan& plan,
    const std::vector<Target>& targets,std::set<int>& original_lines,CombinedClearResult& result){
    std::set<std::size_t> selected;for(const auto& target:targets)selected.insert(target.plane);
    auto owners=context.line_planes();std::vector<int> start,end;start.reserve(plan.occurrences.size());end.reserve(plan.occurrences.size());
    for(int id:plan.occurrences){e.budget.charge();auto& mc=e.model(id);const int source=mc.line;
        if(source>=0){const auto plane=owners.at(source);if(!plan.all&&!selected.contains(plane))continue;
            const auto& line=e.line(source);unite(result.context,line.archived_bounds);
            const auto at=std::ranges::find(line.members,id,&VectorLineMember::model);
            if(at==line.members.end())throw std::invalid_argument("Combined Clear stale native detach member");
            if(at!=line.members.begin()){const auto peer=*std::prev(at);(peer.selector?end:start).push_back(peer.model);}
            if(std::next(at)!=line.members.end()){const auto peer=*std::next(at);(peer.selector?start:end).push_back(peer.model);}
            const VectorBounds stroke{mc.scalars[4],mc.scalars[5],mc.scalars[6],mc.scalars[7]};
            const auto detached=vector_line_detail::detach_model_from_line(e,id,owners);
            if(detached.detached){unite(result.changed,stroke);result.performed=true;if(detached.source_released&&original_lines.erase(detached.source_line))++result.erased_lines;}
        }
        // Native clears even bare MCs, and this flag edit alone leaves BOOL0.
        if(plan.all&&e.model(id).flags&0x40u){e.model(id).flags&=~0x40u;e.topology_changed=true;}
    }
    for(bool first:{true,false})for(int id:first?start:end){e.budget.charge();const auto& m=e.model(id);if(m.line<0)continue;const auto width=e.line(m.line).half_width_bits;
        e.slots[4].reset();try{spatial_endpoint::endpoint_without_cache(e.a,e.layer,id,first,width,e.wire,e.nodes,e.refs,e.storage,e.budget.work,e.budget.stop);}catch(...){e.slots[4].reset();throw;}e.slots[4].reset();}
}
}
VectorBounds partition_capture_lines(vector_boundary_detail::Integrator& context,
    const std::vector<std::size_t>& selected,const std::vector<int>& regions,bool marked,VectorBounds converter_dirty){
    auto& e=context.geometry();const std::set<int> chosen(regions.begin(),regions.end());VectorBounds dirty;
    for(auto plane:selected){if(!pure(e.layer.planes.at(plane).family))continue;
        OwnerLists links(e.layer.planes[plane],e.paint);e.owner.lines=links.lines;
        for(int id:links.lines){const auto original=e.line(id);if(original.members.empty()||(!marked&&!vector_probe_detail::overlap(original.archived_bounds,converter_dirty)))continue;
            const auto hit=[&](VectorLineMember m){return marked?bool(e.model(m.model).flags&0x10000u):
                chosen.contains(side_region(e,m.model,0,false))||chosen.contains(side_region(e,m.model,1,false));};
            if(marked&&!std::ranges::any_of(original.members,hit))continue;
            unite(dirty,original.archived_bounds);
            const auto cache=[&](int line){e.cache(line,false);unite(dirty,e.line(line).archived_bounds);};
            int current=e.new_line(original,id);bool previous=false,first=true;
            for(auto member:original.members){const bool inside=hit(member);
                if(!first&&inside!=previous){trim_endpoint(e,member,false,original.half_width_bits);cache(current);current=e.new_line(original,id);}
                e.line(current).members.push_back(member);e.model(member.model).line=current;e.reference();
                if(!first&&inside!=previous)trim_endpoint(e,member,true,original.half_width_bits);
                previous=inside;first=false;
            }
            cache(current);e.refs-=original.members.size()+1;e.line(id).members.clear();Editor::remove(e.owner.lines,id);e.erase(e.a.lines,5,id);
        }
        links.lines=e.owner.lines;links.vertices=e.owner.vertices;links.models=e.owner.models;links.publish(e.layer.planes[plane],e.paint);
    }
    context.publish();return dirty;
}
CombinedClearResult delete_captured_pure_faces(vector_boundary_detail::Integrator& context,
    const std::vector<std::size_t>& selected,std::vector<int> pending,bool full,bool spatial){
    auto& e=context.geometry();std::vector<std::size_t> paints;auto targets=admit_targets(e,context,selected,full,paints);admit_faces(e,context);
    CombinedClearResult result;std::set<int> original;for(const auto& t:targets)original.insert(t.links.lines.begin(),t.links.lines.end());
    const auto active=live_regions(e,pending);const auto refunds=face_node_refunds(e,context,active,paints);
    const auto face_refs=face_reference_projection(e,context,active,paints);std::optional<SpatialPlan> plan;
    if(spatial)plan=spatial_preflight(e,context,active,targets,refunds,face_refs);else marked_preflight(e,targets,pending,full,refunds,face_refs);
    const auto deleted=vector_grouped_face_detail::delete_region_faces(context,active,paints);result.erased_faces=deleted.erased_faces;result.performed=deleted.storage_changed;
    unite(result.changed,deleted.immediate_dirty);unite(result.context,deleted.immediate_dirty);
    if(spatial)spatial_pure(e,context,*plan,targets,original,result);else marked_pure(e,targets,full,pending,result);
    unite(result.context,context.materialize_regions(pending,nullptr,true));context.publish();return result;
}
CombinedClearResult clear_combined_pure_faces(VectorPaintArchive& a,Layer& layer,
    const std::vector<std::size_t>& selected,SelectionLayerState state,
    const vector_selection_detail::FrozenNativeSelection* native,std::uint32_t flags,
    std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& refs,std::uint64_t& storage,
    std::uint64_t& work,std::stop_token stop){
    Budget budget{work,stop};budget.charge();
    charge(wire,0,8ull*1024*1024,"Combined Clear input wire limit");charge(nodes,0,4096,"Combined Clear input record limit");charge(refs,0,8192,"Combined Clear input reference limit");charge(storage,0,max_storage,"Combined Clear input storage limit");
    const bool full=state==SelectionLayerState::FullPlane,spatial=!full&&bool(flags&1);
    if((flags&0x300)!=0x300||int(state)<0||int(state)>4||(spatial&&int(state)<2)||layer.family!=0x21200000||layer.opaque||(layer.common.depth!=32&&layer.common.depth!=64)||selected.size()>1024||layer.planes.size()>1024)
        throw std::invalid_argument("Combined Clear requires native Paint212 grouped flags300");
    if(spatial?(!native||native->bypass()):native!=nullptr)throw std::invalid_argument("Combined Clear selection packet does not match route");
    const auto records=a.base.vertices.size()+a.base.scales.size()+a.base.subcurves.size()+a.base.models.size()+a.base.crosspoints.size()+a.base.lines.size()+a.loops.size()+a.regions.size()+a.faces.size()+a.gradnodes.size()+a.gradations.size()+a.textures.size();
    if(records>nodes)throw std::invalid_argument("Combined Clear incomplete live record ledger");
    std::uint64_t bytes=layer.specific.size();for(const auto& p:layer.planes)charge(bytes,p.specific.size(),8ull*1024*1024,"Combined Clear source wire limit");
    if(bytes>wire)throw std::invalid_argument("Combined Clear incomplete source wire ledger");
    std::uint64_t actual_refs=a.border.vertices.size()+a.border.models.size()+a.border.regions.size();
    for(const auto& v:a.base.vertices)actual_refs+=v.value.subcurves.size();
    for(const auto& sc:a.base.subcurves)actual_refs+=sc.value.scales.size();
    for(const auto& m:a.base.models)actual_refs+=m.value.subcurves.size()+std::ranges::count_if(m.value.crosspoints,[](int id){return id>=0;});
    for(const auto& lc:a.base.lines)actual_refs+=lc.value.members.size();
    for(const auto& loop:a.loops)actual_refs+=loop.value.members.size();
    for(const auto& r:a.regions)actual_refs+=r.value.outer.members.size()+r.value.holes.size()+r.value.faces.size();
    for(const auto& plane:layer.planes)if(pure(plane.family)||paint(plane.family)){
        Reader input(plane.specific);for(unsigned lane=0;lane<(pure(plane.family)?1u:3u);++lane){budget.charge();const auto n=input.i32();input.require_count(n,4,8192);actual_refs+=std::uint64_t(n);input.take(std::size_t(n)*4);}
    }
    for(const auto& plane:layer.planes)if(vert(plane.family)){const OwnerLists links(plane,&a);actual_refs+=links.vertices.size()+links.models.size()+links.lines.size();}
    if(actual_refs>refs)throw std::invalid_argument("Combined Clear incomplete live reference ledger");
    charge(storage,128*(records+refs+layer.planes.size()+selected.size()+1)+8*bytes,max_storage,"Combined Clear ownership staging storage limit");
    Editor e(a.base,&a,layer,BorderOwnerTag{},wire,nodes,refs,storage,work,stop);vector_boundary_detail::Integrator context(e);
    std::vector<std::size_t> paint_planes;auto targets=admit_targets(e,context,selected,full,paint_planes,spatial);admit_faces(e,context);CombinedClearResult result;std::set<int> original_lines;for(const auto& target:targets)original_lines.insert(target.links.lines.begin(),target.links.lines.end());
    if(spatial&&!native->available()){budget.charge();return result;}
    std::vector<int> pending;
    if(spatial){auto converted=vector_selection_detail::gradation_transfer(context,*native,{layer.common.x,layer.common.y});pending=std::move(converted.selected_regions);result.context=converted.converter_dirty;}
    else{std::set<std::size_t> premark;if(full)premark.insert(paint_planes.begin(),paint_planes.end());pending=marked_regions(e,premark,context);}
    std::optional<SpatialPlan> spatial_plan;
    const auto active_regions=live_regions(e,pending);const auto refunds=face_node_refunds(e,context,active_regions,paint_planes);
    const auto face_refs=face_reference_projection(e,context,active_regions,paint_planes);
    if(spatial)spatial_plan=spatial_preflight(e,context,active_regions,targets,refunds,face_refs);else marked_preflight(e,targets,pending,full,refunds,face_refs);
    // All original owners and joint resource projections precede premark.
    if(full){for(auto& target:targets){layer.planes[target.plane].common.flags[3]=1;for(const auto& source:target.sources)for(auto member:source.line.members){budget.charge();e.model(member.model).flags|=0x10000u;}}
        for(auto plane:paint_planes){budget.charge();layer.planes[plane].common.flags[3]=1;vector_material_detail::PaintLists lists(layer.planes[plane]);for(int face:lists.ids[0])e.value(a.faces,face).flags|=0x10000u;}}
    const auto deleted=vector_grouped_face_detail::delete_region_faces(context,active_regions,paint_planes);
    result.erased_faces=deleted.erased_faces;result.performed=deleted.storage_changed;unite(result.changed,deleted.immediate_dirty);unite(result.context,deleted.immediate_dirty);
    if(spatial)spatial_pure(e,context,*spatial_plan,targets,original_lines,result);else marked_pure(e,targets,full,pending,result);
    unite(result.context,context.materialize_regions(pending,nullptr,true));context.publish();budget.charge();return result;
}
}
