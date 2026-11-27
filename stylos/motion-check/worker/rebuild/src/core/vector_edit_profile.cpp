#include "vector_edit_profile.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <map>
#include <set>
#include <stdexcept>
namespace retas::vector_edit_detail {
namespace {
using numeric_detail::binary64;
void charge(std::uint64_t& n,std::uint64_t add,std::uint64_t cap){if(n>cap||add>cap-n)throw std::length_error("Vector endpoint rewrite budget exceeded");n+=add;}
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Vector endpoint rewrite cancelled");}
float f32(double v){const float f=float(v);if(!std::isfinite(f))throw std::invalid_argument("Nonfinite vector endpoint scalar");return f;}
int integer(double v){if(!std::isfinite(v)||v<INT32_MIN||v>INT32_MAX)throw std::overflow_error("Vector endpoint coordinate overflow");return int(v);}
VectorPoint point(const VectorVertex& v){if(std::abs(std::int64_t(v.x))>134217727||std::abs(std::int64_t(v.y))>134217727)throw std::invalid_argument("Vector endpoint source exceeds raw domain");return {double(v.x),double(v.y)};}
template<class T> auto index(std::vector<VectorPoolRecord<T>>& pool){std::map<int,T*> out;for(auto& r:pool)out.emplace(r.index,&r.value);return out;}
void unite(VectorBounds& a,VectorBounds b){if(b.left>=b.right||b.top>=b.bottom)return;if(a.left>=a.right||a.top>=a.bottom)a=b;else a={std::min(a.left,b.left),std::min(a.top,b.top),std::max(a.right,b.right),std::max(a.bottom,b.bottom)};}
void include(VectorBounds& b,VectorPoint p){const int x=integer(p.x),y=integer(p.y);unite(b,{x,y,integer(p.x+1),integer(p.y+1)});}
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
VectorPoint xy(const VectorCrosspoint& cp,int at){return {double(std::bit_cast<std::int32_t>(cp.words[at])),double(std::bit_cast<std::int32_t>(cp.words[at+1]))};}
void cp_bounds(VectorBounds& b,const VectorCrosspoint& cp){include(b,xy(cp,4));include(b,xy(cp,12));if(cp.words[0]==2)include(b,xy(cp,6));if(cp.words[1]==2)include(b,xy(cp,10));if(cp.words[0]==1||cp.words[0]==2||cp.words[0]==4||cp.words[1]==1||cp.words[1]==2||cp.words[1]==4)include(b,xy(cp,8));}
VectorPoint offset(VectorPoint center,VectorPoint p){return {center.x+f32(std::floor(p.x)),center.y+std::floor(p.y)};}
float gap(float degrees){if(degrees<0)degrees=f32(binary64(double(degrees)+360));if(degrees>180)degrees=f32(binary64(double(degrees)-180));return std::clamp(degrees,30.f,150.f);}
void endpoint_lane(VectorCrosspoint& cp,const VectorCrosspoint* companion,int lane,bool sample,
    bool at_start,float degrees,VectorPoint center,VectorPoint weighted,float radius,
    VectorPoint start,VectorPoint control,VectorPoint end,bool linear){
    if(!sample){const auto cot=degrees<89?binary64(1/std::tan(binary64(double(degrees)*double(0.017453292f)))):0;const double x=lane?-weighted.y:weighted.y,y=lane?weighted.x:-weighted.x;
        cp.words[lane]=3;xy(cp,lane?12:4,offset(center,{binary64(binary64(cot*weighted.x)+x),f32(binary64(binary64(cot*weighted.y)+y))}));return;}
    const auto cot=degrees>91?binary64(1/std::tan(binary64(double(degrees)*double(0.017453292f)))):0;const VectorPoint query{binary64(binary64(cot*weighted.x)+center.x),binary64(binary64(cot*weighted.y)+center.y)};
    float t=inverse(start,control,end,query,linear).value_or(at_start?1.f:0.f);if(companion&&companion->words[1-lane]==1){const float trim=std::bit_cast<float>(companion->words[3-lane]);if(trim>0)t=at_start?std::min(t,trim):std::max(t,trim);}
    const auto p=evaluate(start,control,end,t,linear),u=tangent(start,control,end,t,linear);const bool flip=(lane==0)==at_start;const double nx=flip?-u.y:u.y,ny=flip?u.x:-u.x;
    cp.words[lane]=1;cp.words[2+lane]=std::bit_cast<std::uint32_t>(t);xy(cp,lane?12:4,{std::floor(binary64(binary64(nx*radius)+p.x)),std::floor(binary64(binary64(ny*radius)+p.y))});xy(cp,8,center);
}
struct Edge {std::uint32_t kind{};VectorPoint first,control_a,middle,control_b,last;};
//5B5290's edge record, with independent endpoint widths and side markers.
Edge corner(VectorPoint center,float heading_a,float heading_b,double width_a,double width_b,bool marked_a,bool marked_b){
    const float ra=std::max(f32(binary64(width_a-.0625)),0.f),rb=std::max(f32(binary64(width_b-.0625)),0.f);
    const auto a=direction(f32(binary64(double(heading_a)+180))),b=direction(f32(binary64(double(heading_b)+180)));
    const float ax=f32(binary64(a.y*ra)),ay=f32(binary64(-a.x*ra)),bx=f32(binary64(-b.y*rb)),by=f32(binary64(b.x*rb));
    float degrees=f32(binary64(double(heading_b)-heading_a));if(degrees<0)degrees=f32(binary64(double(degrees)+360));Edge e;e.first=offset(center,{ax,ay});e.last=offset(center,{bx,by});
    if(degrees<185||(std::abs(ax)<1&&std::abs(ay)<1)||(std::abs(bx)<1&&std::abs(by)<1))return e;
    if(degrees<210||!marked_a||!marked_b){e.kind=2;const auto average=binary64(binary64(double(ra)+rb)*.5),dx=binary64(a.x+b.x),dy=binary64(a.y+b.y);
        const auto inv=binary64(1/std::sqrt(binary64(binary64(dx*dx)+binary64(dy*dy))));const float ux=f32(binary64(inv*dx));const auto uy=binary64(inv*dy);
        const float mx=f32(binary64(double(ux)*average)),my=f32(binary64(uy*average)),sa=f32(binary64(binary64(double(ux)*a.y)-binary64(uy*a.x))),sb=f32(binary64(binary64(uy*b.x)-binary64(double(ux)*b.y)));
        const auto control=[&](float px,float py,float s,float radius){double x,y;if(s==0||(average>binary64(double(s)*radius)&&average<binary64(double(radius)/s))){
                const auto n=binary64(binary64(binary64(double(py)-my)*py)+binary64(binary64(double(px)-mx)*px)),d=binary64(binary64(double(mx)*py)-binary64(double(my)*px)),k=binary64(n/d);
                x=binary64(double(mx)-binary64(double(my)*k));y=binary64(binary64(k*mx)+my);
            }else{x=binary64(binary64(double(mx)+px)*.5);y=binary64(binary64(double(my)+py)*.5);}return offset(center,{f32(x),f32(y)});};
        e.control_a=control(ax,ay,sa,ra);e.middle=offset(center,{mx,my});e.control_b=control(bx,by,sb,rb);return e;}
    if(degrees>=330){e.kind=4;const auto dx=binary64(a.x+b.x),dy=binary64(a.y+b.y),inv=binary64(1/std::sqrt(binary64(binary64(dx*dx)+binary64(dy*dy)))),scale=binary64(binary64(double(ra)+rb)*2);
        const float ux=f32(binary64(dx*inv));e.middle=offset(center,{f32(binary64(double(ux)*scale)),f32(binary64(binary64(inv*dy)*scale))});return e;}
    e.kind=3;const auto denominator=binary64(binary64(double(bx)*ay)-binary64(double(by)*ax));if(denominator==0)e.first=center;else{
        const auto k=binary64(binary64(binary64(binary64(double(ay)-by)*ay)+binary64(binary64(double(ax)-bx)*ax))/denominator);
        e.first=offset(center,{f32(binary64(double(bx)-binary64(double(by)*k))),f32(binary64(binary64(k*bx)+by))});}e.last=e.first;return e;
}
void set_edge(VectorCrosspoint& cp,int lane,const Edge& e){cp.words[lane]=e.kind;xy(cp,lane?12:4,lane?e.last:e.first);
    if(e.kind==1){cp.words[2+lane]=0;xy(cp,8,e.middle);}else if(e.kind==2){xy(cp,lane?10:6,lane?e.control_b:e.control_a);xy(cp,8,e.middle);}else if(e.kind==4)xy(cp,8,e.middle);}
struct EndpointEngine {
    VectorLineArchive& a;Layer& layer;const std::map<int,std::size_t>& planes;
    std::uint64_t& wire;std::uint64_t& nodes;std::uint64_t& refs;std::uint64_t& storage;std::uint64_t& work;std::stop_token stop;
    std::map<int,VectorVertex*> vertices;std::map<int,VectorLineScale*> scales;std::map<int,VectorSubcurve*> subs;std::map<int,VectorModelCurve*> models;std::map<int,VectorLine*> lines;
    int cursor{},high{-1};std::set<int> live;
    EndpointEngine(VectorLineArchive& archive,Layer& l,const std::map<int,std::size_t>& p,std::uint64_t& w,std::uint64_t& n,std::uint64_t& r,std::uint64_t& s,std::uint64_t& cost,std::stop_token token):a(archive),layer(l),planes(p),wire(w),nodes(n),refs(r),storage(s),work(cost),stop(token){
        poll(stop);charge(work,32*(a.vertices.size()+a.scales.size()+a.subcurves.size()+a.models.size()+a.lines.size()+a.crosspoints.size()+1),128ull*1024*1024);charge(storage,64*(a.vertices.size()+a.scales.size()+a.subcurves.size()+a.models.size()+a.lines.size()+a.crosspoints.size()+1),512ull*1024*1024);
        vertices=index(a.vertices);scales=index(a.scales);subs=index(a.subcurves);models=index(a.models);lines=index(a.lines);for(const auto& r:a.crosspoints){live.insert(r.index);high=std::max(high,r.index);}if(const auto h=layer.runtime_vector_high_water[4]){if(*h<high||*h<-1)throw std::invalid_argument("Invalid live crosspoint H");high=*h;}cursor=lowest();
    }
    int lowest(){int id=0;while(live.contains(id)){poll(stop);charge(work,32,128ull*1024*1024);++id;}return id;}
    VectorCrosspoint& cp(int id){const auto r=std::ranges::find(a.crosspoints,id,&VectorPoolRecord<VectorCrosspoint>::index);if(r==a.crosspoints.end())throw std::invalid_argument("Dead endpoint CP");return r->value;}
    void publish(){std::ranges::sort(a.crosspoints,{},&VectorPoolRecord<VectorCrosspoint>::index);a.high_water[4]=high;layer.runtime_vector_high_water[4]=high;}
    VectorCrosspoint& ensure(VectorModelCurve& m,bool first){auto& id=m.crosspoints[first?0:1];if(id>=0)return cp(id);
        charge(wire,64,8ull*1024*1024);charge(nodes,1,4096);charge(refs,1,8192);charge(storage,512,512ull*1024*1024);id=cursor;live.insert(id);a.crosspoints.push_back({id,{}, {}});if(std::int64_t(id)==std::int64_t(high)+1){high=id;++cursor;}else cursor=lowest();publish();return cp(id);}
    void release(VectorModelCurve& m,bool first){auto& id=m.crosspoints[first?0:1];if(id<0)return;std::size_t users{};for(const auto& r:a.models){charge(work,2,128ull*1024*1024);users+=std::ranges::count(r.value.crosspoints,id);}if(users!=1)throw std::invalid_argument("Cannot release a shared endpoint CP");
        const auto r=std::ranges::find(a.crosspoints,id,&VectorPoolRecord<VectorCrosspoint>::index);if(r==a.crosspoints.end())throw std::invalid_argument("Dead endpoint CP release");wire-=64+r->suffix.size();--nodes;--refs;live.erase(id);if(cursor>id){cursor=id;if(high==id)--high;}a.crosspoints.erase(r);id=-1;m.flags&=~(first?0x10u:0x20u);publish();}
    VectorPoint control(const VectorSubcurve& s){if(std::abs(std::int64_t(s.control_x))>134217727||std::abs(std::int64_t(s.control_y))>134217727)throw std::invalid_argument("Endpoint control exceeds raw domain");return {double(s.control_x),double(s.control_y)};}
    bool first_side(const VectorModelCurve& m,int sub,int vertex){return !m.subcurves.empty()&&sub==m.subcurves.front()&&subs.at(sub)->vertex_a==vertex;}
    double width(const VectorModelCurve& m,bool first){double v=std::bit_cast<float>(lines.at(m.line)->half_width_bits);if(!(m.flags&2)){const auto& s=*subs.at(first?m.subcurves.front():m.subcurves.back());if(s.scales.empty())throw std::invalid_argument("Variable endpoint has no width node");v=binary64(v*double(std::bit_cast<float>(scales.at(first?s.scales.front():s.scales.back())->scale_bits)));}if(!std::isfinite(v))throw std::invalid_argument("Invalid endpoint width");return v;}
    VectorBounds cache(VectorModelCurve& m,int id){const auto& s=*subs.at(id);VectorBounds center{};include(center,point(*vertices.at(s.vertex_a)));include(center,point(*vertices.at(s.vertex_b)));if(!(m.flags&1))include(center,control(s));
        VectorBounds old_center{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]},old_stroke{m.scalars[4],m.scalars[5],m.scalars[6],m.scalars[7]};unite(old_center,center);
        if(m.line<0){m.scalars[0]=old_center.left;m.scalars[1]=old_center.top;m.scalars[2]=old_center.right;m.scalars[3]=old_center.bottom;return center;}
        double maximum=m.flags&2?1:0;
        if(!(m.flags&2))for(auto node:s.scales){poll(stop);charge(work,1,128ull*1024*1024);const float v=std::bit_cast<float>(scales.at(node)->scale_bits);if(v>maximum)maximum=v;}
        const int inflate=integer(std::trunc(binary64(double(std::bit_cast<float>(lines.at(m.line)->half_width_bits))*maximum))+1);VectorBounds stroke{integer(double(center.left)-inflate),integer(double(center.top)-inflate),integer(double(center.right)+inflate),integer(double(center.bottom)+inflate)};
        if(id==m.subcurves.front()&&m.crosspoints[0]>=0)cp_bounds(stroke,cp(m.crosspoints[0]));if(id==m.subcurves.back()&&m.crosspoints[1]>=0)cp_bounds(stroke,cp(m.crosspoints[1]));unite(old_stroke,stroke);m.scalars={old_center.left,old_center.top,old_center.right,old_center.bottom,old_stroke.left,old_stroke.top,old_stroke.right,old_stroke.bottom};return stroke;
    }
    void vertex(int vertex_id,std::size_t owner){poll(stop);if(vertex_id<0)throw std::invalid_argument("Nullable Line endpoint");const auto& v=*vertices.at(vertex_id);const auto center=point(v);std::map<float,int> fan;
        for(auto id:v.subcurves){poll(stop);charge(work,32,128ull*1024*1024);if(id<0)continue;const auto& s=*subs.at(id);if(s.vertex_a!=vertex_id&&s.vertex_b!=vertex_id)throw std::invalid_argument("Nonincident endpoint adjacency");const auto& m=*models.at(s.model);if(m.line<0||(m.flags&0x02000000)||!planes.contains(m.line)||planes.at(m.line)!=owner)continue;
            const auto a=point(*vertices.at(s.vertex_a)),b=point(*vertices.at(s.vertex_b));VectorPoint other=s.vertex_a==vertex_id?b:a;if(!(m.flags&1)){const auto c=control(s);if(!flat(a,c,b))other=c;}fan[angle({other.x-center.x,other.y-center.y})]=id;}
        if(fan.empty())return;if(fan.size()==1){const int id=fan.begin()->second;auto& m=*models.at(subs.at(id)->model);const bool first=first_side(m,id,vertex_id);if(!(m.flags&(first?0x10u:0x20u)))release(m,first);return;}
        if(fan.size()==2){const auto& m=*models.at(subs.at(fan.begin()->second)->model);if(subs.at(m.subcurves.front())->vertex_a!=vertex_id&&subs.at(m.subcurves.back())->vertex_b!=vertex_id)return;}
        charge(storage,128*fan.size(),512ull*1024*1024);charge(work,256*fan.size(),128ull*1024*1024);std::vector<std::pair<float,int>> rays(fan.begin(),fan.end());std::vector<Edge> edges;edges.reserve(rays.size());
        for(std::size_t i=0;i<rays.size();++i){poll(stop);const auto& aa=rays[i];const auto& bb=rays[(i+1)%rays.size()];const auto& ma=*models.at(subs.at(aa.second)->model);const auto& mb=*models.at(subs.at(bb.second)->model);const bool fa=first_side(ma,aa.second,vertex_id),fb=first_side(mb,bb.second,vertex_id);edges.push_back(corner(center,aa.first,bb.first,width(ma,fa),width(mb,fb),ma.flags&(fa?4u:8u),mb.flags&(fb?4u:8u)));}
        for(std::size_t i=0;i<rays.size();++i){poll(stop);const int id=rays[i].second;auto& m=*models.at(subs.at(id)->model);const bool first=first_side(m,id,vertex_id);const auto& current=edges[i];const auto& previous=edges[(i+rays.size()-1)%rays.size()];if(!current.kind&&!previous.kind){release(m,first);continue;}auto& out=ensure(m,first);set_edge(out,0,current);set_edge(out,1,previous);unite(lines.at(m.line)->archived_bounds,cache(m,id));}
    }
    void retained(int id,bool first){poll(stop);auto& m=*models.at(id);const int side=first?0:1;if(m.crosspoints[side]<0||!(m.flags&(first?0x10u:0x20u)))return;const auto& s=*subs.at(first?m.subcurves.front():m.subcurves.back());const auto start=point(*vertices.at(s.vertex_a)),end=point(*vertices.at(s.vertex_b)),c=control(s),center=first?start:end;const VectorPoint d{center.x-c.x,center.y-c.y};const auto u=unit(d);const float radius=f32(width(m,first));const VectorPoint weighted{f32(binary64(u.x*radius)),f32(binary64(u.y*radius))};const float heading=angle(d);auto& out=cp(m.crosspoints[side]);const VectorCrosspoint* companion=m.subcurves.size()==1&&m.crosspoints[1-side]>=0?&cp(m.crosspoints[1-side]):nullptr;
        for(int lane=0;lane<2;++lane){const auto old=xy(out,lane?12:4);const auto a=angle({old.x-center.x,old.y-center.y});const auto degrees=gap(f32(lane?binary64(binary64(double(heading)-a)*-1):binary64(double(heading)-a)));endpoint_lane(out,companion,lane,out.words[lane]==1,first,degrees,center,weighted,radius,start,c,end,m.flags&1);}
    }
    void cut(int id,bool first,std::array<VectorPoint,2> hints){poll(stop);auto& m=*models.at(id);if(m.subcurves.empty())throw std::invalid_argument("Cut endpoint requires a nonempty model");
        const auto& s=*subs.at(first?m.subcurves.front():m.subcurves.back());const auto start=point(*vertices.at(s.vertex_a)),end=point(*vertices.at(s.vertex_b)),c=control(s),center=first?start:end;
        const VectorPoint d{center.x-c.x,center.y-c.y};const auto u=unit(d);const auto heading=angle(d);const float radius=f32(width(m,first));const VectorPoint weighted{f32(binary64(u.x*radius)),f32(binary64(u.y*radius))};
        auto& out=ensure(m,first);m.flags|=first?0x10u:0x20u;const auto* companion=m.subcurves.size()==1&&m.crosspoints[first?1:0]>=0?&cp(m.crosspoints[first?1:0]):nullptr;
        for(int lane=0;lane<2;++lane){const auto hint=hints[std::size_t(lane)];const auto a=angle({hint.x-center.x,hint.y-center.y});const auto degrees=gap(f32(lane?binary64(binary64(double(heading)-a)*-1):binary64(double(heading)-a)));
            endpoint_lane(out,companion,lane,degrees>=90,first,degrees,center,weighted,radius,start,c,end,m.flags&1);}
    }
};
}
void incremental_subcurve(VectorLineArchive& a,Layer& layer,int id,std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& refs,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
    const std::map<int,std::size_t> planes;EndpointEngine e(a,layer,planes,wire,nodes,refs,storage,work,stop);auto& m=*e.models.at(e.subs.at(id)->model);e.cache(m,id);poll(stop);
}
void cut_endpoint(VectorLineArchive& a,Layer& layer,int model,bool first,std::array<VectorPoint,2> hints,std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& refs,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
    const std::map<int,std::size_t> planes;EndpointEngine e(a,layer,planes,wire,nodes,refs,storage,work,stop);e.cut(model,first,hints);poll(stop);
}
void retained_endpoint(VectorLineArchive& a,Layer& layer,int model,bool first,std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& refs,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
    const std::map<int,std::size_t> planes;EndpointEngine e(a,layer,planes,wire,nodes,refs,storage,work,stop);e.retained(model,first);poll(stop);
}
void partition_endpoint(VectorLineArchive& a,Layer& layer,int model_id,bool at_start,bool cache_at_start,std::uint32_t half_width,std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& refs,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
    poll(stop);charge(work,32*(a.vertices.size()+a.subcurves.size()+a.models.size()+a.scales.size()+a.crosspoints.size()+1),128ull*1024*1024);
    charge(storage,64*(a.vertices.size()+a.subcurves.size()+a.models.size()+a.scales.size()+1),512ull*1024*1024);
    const auto vertices=index(a.vertices);const auto subs=index(a.subcurves);const auto models=index(a.models);const auto scales=index(a.scales);
    auto& m=*models.at(model_id);if(m.subcurves.empty())throw std::invalid_argument("Endpoint rewrite requires a nonempty model");
    const int sub_id=at_start?m.subcurves.front():m.subcurves.back();const auto& sub=*subs.at(sub_id);const int vertex_id=at_start?sub.vertex_a:sub.vertex_b;const auto& vertex=*vertices.at(vertex_id);
    const auto center=point(vertex),start=point(*vertices.at(sub.vertex_a)),end=point(*vertices.at(sub.vertex_b));const VectorPoint control{double(sub.control_x),double(sub.control_y)};if(std::abs(control.x)>134217727||std::abs(control.y)>134217727)throw std::invalid_argument("Endpoint control exceeds raw domain");
    const float base=std::bit_cast<float>(half_width);if(!std::isfinite(base))throw std::invalid_argument("Invalid endpoint base width");double width=base;
    if(!(m.flags&2)){if(sub.scales.empty())throw std::invalid_argument("Variable endpoint has no width node");const float scale=std::bit_cast<float>(scales.at(at_start?sub.scales.front():sub.scales.back())->scale_bits);if(!std::isfinite(scale))throw std::invalid_argument("Invalid endpoint node width");width=binary64(width*scale);}
    const float radius=f32(width);const int side=at_start?0:1;int cp_id=m.crosspoints[side];
    if(cp_id<0){std::set<int> live;int high=-1;for(const auto& r:a.crosspoints){live.insert(r.index);high=std::max(high,r.index);}if(const auto h=layer.runtime_vector_high_water[4]){if(*h<high||*h<-1)throw std::invalid_argument("Invalid live crosspoint H");high=*h;}int cursor=0;while(live.contains(cursor)){poll(stop);charge(work,32,128ull*1024*1024);++cursor;}
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
    //59E460 receives the outgoing node's selected SC independently of
    //the incoming selector used to choose its reconstructed endpoint.
    const int cache_id=cache_at_start?m.subcurves.front():m.subcurves.back();const auto& cache=*subs.at(cache_id);const auto ca=point(*vertices.at(cache.vertex_a)),ce=point(*vertices.at(cache.vertex_b));const VectorPoint cc{double(cache.control_x),double(cache.control_y)};
    VectorBounds center_bounds{};include(center_bounds,ca);include(center_bounds,ce);if(!(m.flags&1))include(center_bounds,cc);
    VectorBounds saved_center{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]},saved_stroke{m.scalars[4],m.scalars[5],m.scalars[6],m.scalars[7]};unite(saved_center,center_bounds);double maximum=m.flags&2?1:0;
    if(!(m.flags&2))for(auto id:cache.scales){const float scale=std::bit_cast<float>(scales.at(id)->scale_bits);if(scale>maximum)maximum=scale;}
    const auto inflate=integer(std::trunc(binary64(double(base)*maximum))+1);VectorBounds stroke{integer(double(center_bounds.left)-inflate),integer(double(center_bounds.top)-inflate),integer(double(center_bounds.right)+inflate),integer(double(center_bounds.bottom)+inflate)};
    if(cache_id==m.subcurves.front()&&m.crosspoints[0]>=0)cp_bounds(stroke,std::ranges::find(a.crosspoints,m.crosspoints[0],&VectorPoolRecord<VectorCrosspoint>::index)->value);if(cache_id==m.subcurves.back()&&m.crosspoints[1]>=0)cp_bounds(stroke,std::ranges::find(a.crosspoints,m.crosspoints[1],&VectorPoolRecord<VectorCrosspoint>::index)->value);
    unite(saved_stroke,stroke);m.scalars={saved_center.left,saved_center.top,saved_center.right,saved_center.bottom,saved_stroke.left,saved_stroke.top,saved_stroke.right,saved_stroke.bottom};poll(stop);
}
void repair_vertex_endpoints(VectorLineArchive& a,Layer& layer,int vertex_id,std::size_t plane,const std::map<int,std::size_t>& planes,std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& refs,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
    EndpointEngine e(a,layer,planes,wire,nodes,refs,storage,work,stop);e.vertex(vertex_id,plane);poll(stop);
}
void rebuild_line_endpoints(VectorLineArchive& a,Layer& layer,int line_id,const std::map<int,std::size_t>& planes,std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& refs,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
    EndpointEngine e(a,layer,planes,wire,nodes,refs,storage,work,stop);const auto& line=*e.lines.at(line_id);if(line.members.empty())throw std::invalid_argument("Endpoint rebuild requires a nonempty Line");const auto owner=planes.at(line_id);
    const auto endpoint=[&](VectorLineMember member,bool start){const auto& m=*e.models.at(member.model);if(m.subcurves.empty())throw std::invalid_argument("Endpoint rebuild requires a nonempty model");const bool first=(member.selector!=0)==start;const auto& s=*e.subs.at(first?m.subcurves.front():m.subcurves.back());return first?s.vertex_a:s.vertex_b;};
    e.vertex(endpoint(line.members.front(),true),owner);for(auto member:line.members)e.vertex(endpoint(member,false),owner);e.retained(line.members.front().model,line.members.front().selector!=0);e.retained(line.members.back().model,line.members.back().selector==0);poll(stop);
}
}
