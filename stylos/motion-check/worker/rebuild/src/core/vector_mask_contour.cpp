#include "vector_mask_contour.hpp"
#include "vector_document_geometry.hpp"
#include "vector_material_edit.hpp"
#include "retas/numeric.hpp"
#include "native_offscreen.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <deque>
#include <limits>

namespace retas::vector_mask_detail {
namespace {
using P=VectorFaceVertex;
using vector_probe_detail::Budget;
using vector_probe_detail::Scope;
using numeric_detail::binary64;
using vector_material_detail::charge;
int wrap(std::uint32_t v){return std::bit_cast<std::int32_t>(v);}
int add(int a,int b){return wrap(std::uint32_t(a)+std::uint32_t(b));}
int sub(int a,int b){return wrap(std::uint32_t(a)-std::uint32_t(b));}
float f32(double v){volatile float out=static_cast<float>(v);return out;}
void raw_domain(P p){if(std::abs(std::int64_t(p.x))>134217727||std::abs(std::int64_t(p.y))>134217727)throw std::invalid_argument("Mask contour point exceeds bounded raw domain");}
P mean(P a,P b){return {add(a.x,b.x)>>1,add(a.y,b.y)>>1};}
int integer(double v){if(!std::isfinite(v)||v<std::numeric_limits<int>::min()||v>std::numeric_limits<int>::max())throw std::invalid_argument("Mask contour coordinate exceeds signed32");return int(v);}
double distance(P a,P b){const auto x=double(sub(a.x,b.x)),y=double(sub(a.y,b.y));return binary64(std::sqrt(binary64(binary64(x*x)+binary64(y*y))));}
double area(P a,P z,P p){const auto dx=double(sub(z.x,a.x)),dy=double(sub(z.y,a.y)),x=double(sub(p.x,a.x)),y=double(sub(p.y,a.y));return binary64(std::abs(binary64(binary64(dx*y)-binary64(dy*x)))*.5);}
double perpendicular(P a,P z,P p){const auto n=distance(a,z);return n>0?binary64(binary64(area(a,z,p)/n)*2):distance(a,p);}
struct SamplingSettings {float accuracy,max_perpendicular;int endpoint_distance_squared;};
SamplingSettings settings(ContourProfile profile){
    switch(profile){
        case ContourProfile::FaceColor:return {32,200,4096};
        case ContourProfile::PolylineOutline:return {8,50,256};
    }
    throw std::invalid_argument("Mask contour sampling profile is not recovered");
}
int squared_distance(P a,P b){
    //605A50 saturates the integer squared distance, before5CA0A0 compares
    //it with trunc(4*accuracy*accuracy). These caller thresholds are exact.
    const auto x=std::int64_t(sub(a.x,b.x)),y=std::int64_t(sub(a.y,b.y));
    if(std::abs(x)>46340||std::abs(y)>46340)return INT32_MAX;
    return int(std::min<std::int64_t>(x*x+y*y,INT32_MAX));
}
bool corner(P a,P p,P z,const SamplingSettings& sampling){
    //5C9B70: strict perpendicular>+4C, then nonpositive native dot.
    //Both recovered accuracy packets exceed4:FCOMP4/TEST AH,41/JP skips
    //the normalized F32-vector dot<.707 branch used by other native callers.
    if(perpendicular(a,z,p)>sampling.max_perpendicular)return true;
    const auto ax=sub(p.x,a.x),ay=sub(p.y,a.y),bx=sub(z.x,p.x),by=sub(z.y,p.y);
    const auto dot=std::int64_t(ax)*bx+std::int64_t(ay)*by;
    if(dot<=0)return true;
    if(sampling.accuracy>4)return false;
    //595AC0 stores each normalized component as F32 before5C9B70's
    // binary64 dot comparison. FaceColor/Polyline retain their old branch.
    const auto an=binary64(1/distance(a,p)),bn=binary64(1/distance(p,z));
    const auto ux=f32(binary64(double(ax)*an)),uy=f32(binary64(double(ay)*an));
    const auto vx=f32(binary64(double(bx)*bn)),vy=f32(binary64(double(by)*bn));
    return binary64(binary64(double(ux)*vx)+binary64(double(uy)*vy))<.707;
}
struct Packet {P p;float length{};int mode{},serial{};float pressure{1};};
struct Builder {
    ContourGraph result;Budget budget;std::uint64_t& storage;
    int current{-1},last_vertex{-1},line{-1};
    void reserve(std::size_t n=1){budget.charge(64*n);charge(storage,512*n,512ull*1024*1024,"Mask contour storage exceeds512MiB");const auto& a=result.graph;if(a.vertices.size()+a.models.size()+a.subcurves.size()+a.lines.size()+n>4096)throw std::length_error("Mask contour exceeds4096 records");}
    int vertex(P p){reserve();const int id=int(result.graph.vertices.size());result.graph.vertices.push_back({id,{p.x,p.y,{}},{}});result.vertices.push_back(id);return id;}
    void begin(){reserve();line=int(result.graph.lines.size());result.graph.lines.push_back({line,{{},0,0xffff000000000000ull,{}},{}});result.lines.push_back(line);current=last_vertex=-1;}
    void reset(){current=-1;}
    void emit(P a,const P* control,P z){
        if(a==z)return;
        if(current<0){reserve();current=int(result.graph.models.size());VectorModelCurve m;m.flags=2;m.line=line;m.targets={-1,-1};m.crosspoints={-1,-1};result.graph.models.push_back({current,std::move(m),{}});result.models.push_back(current);result.graph.lines[std::size_t(line)].value.members.push_back({current,1});}
        if(last_vertex<0)last_vertex=vertex(a);else{const auto& p=result.graph.vertices[std::size_t(last_vertex)].value;if(p.x!=a.x||p.y!=a.y)throw std::invalid_argument("Native mask sampler discontinuous endpoint");}
        const int end=vertex(z);reserve();const int id=int(result.graph.subcurves.size());const auto c=control?*control:mean(a,z);result.graph.subcurves.push_back({id,{last_vertex,end,c.x,c.y,current,{}},{}});result.graph.models[std::size_t(current)].value.subcurves.push_back(id);result.graph.vertices[std::size_t(last_vertex)].value.subcurves.push_back(id);result.graph.vertices[std::size_t(end)].value.subcurves.push_back(id);last_vertex=end;
    }
    void finish(){
        if(result.graph.lines[std::size_t(line)].value.members.empty()){result.graph.lines.pop_back();result.lines.pop_back();return;}
        vector_geometry_detail::rebuild_lines(result.graph,{line},budget.work,budget.stop);
    }
};
// Both recovered callers fix width and set no temporary straight-line owner.
// Consequently5CC380 copies packets unchanged;5CAE40 and5CC1A0 return
// without profile/auxiliary-Line output. Keep per-instance native queues.
struct Sampler {
    Builder* out;Budget budget;SamplingSettings sampling;
    std::deque<Packet> raw,selected;
    Packet anchor{{},0,0,-1};P pruning{},previous{};float cumulative{};int serial{};
    StrokeStep* step{};bool fresh_model{true};
    static StrokeSample sample(const Packet& q){return {q.p,q.pressure,q.length,q.mode,q.serial};}
    void emit(const Packet& a,const Packet* c,const Packet& z){
        if(a.p==z.p)return;
        if(out)out->emit(a.p,c?&c->p:nullptr,z.p);
        if(step){const StrokeSpan span{sample(a),sample(z),c?std::optional(sample(*c)):std::nullopt,fresh_model};step->emitted.push_back(span);step->actions.push_back({StrokeActionKind::Main,span});}
        fresh_model=false;
    }
    void reset(){if(out)out->reset();fresh_model=true;}
    void pressure_segment(std::size_t last){
        if(!step||raw.empty())return;
        auto base=raw.front();
        for(std::size_t i=1;i<=last;++i){
            if(std::abs(binary64(double(raw[i].pressure)-base.pressure))>=double(.1f)){
                if(!raw[i].mode)raw[i].mode=4;
                if(!raw[i-1].mode)raw[i-1].mode=4;
                base=raw[i];
            }
        }
        for(std::size_t i=0;i<=last;++i)if(i==0||i==last||raw[i].mode==4){
            const auto q=sample(raw[i]);
            if(step->pressure_events.empty()||step->pressure_events.back().length!=q.length)step->pressure_events.push_back(q);
        }
    }
    void consume(Packet packet,bool& batch_flag){
        if(!selected.empty()&&selected.back().p==packet.p)return; // before mode2
        if(step){step->selected=step->clear_preview=true;step->actions.push_back({StrokeActionKind::ClearPreview,{}});}
        selected.push_back(packet);const auto n=selected.size();const bool terminal=packet.mode==2;if(!terminal&&n<=1)return;
        if(n>3)throw std::invalid_argument("Native contour packet window exceeds3");
        const auto copy=selected;std::size_t remaining=n;bool started=false,advanced=false;
        if(copy.front().mode==1){anchor=copy.front();pruning=anchor.p;started=true;selected.pop_front();--remaining;}
        if(!started&&!batch_flag&&n>=2&&corner(anchor.p,copy[0].p,copy[1].p,sampling)){
            emit(anchor,nullptr,copy[0]);anchor=copy[0];pruning=anchor.p;reset();advanced=true;selected.pop_front();--remaining;
        }
        batch_flag=false;
        if((terminal||n>2)&&remaining){
            if(n==3){
                if(corner(copy[0].p,copy[1].p,copy[2].p,sampling)){
                    if(!advanced&&!started){emit(anchor,&copy[0],copy[1]);anchor=copy[1];pruning=anchor.p;reset();selected.pop_front();selected.pop_front();remaining-=2;}
                    else{emit(copy[0],nullptr,copy[1]);anchor=copy[1];pruning=anchor.p;reset();selected.pop_front();--remaining;}
                }else if(!started&&!advanced){
                    auto midpoint=copy[1];midpoint.p=mean(copy[0].p,copy[1].p);midpoint.length=f32(binary64(binary64(double(copy[0].length)+copy[1].length)*.5));
                    emit(anchor,&copy[0],midpoint);if(step){step->preview=StrokeSpan{sample(midpoint),sample(copy[1]),std::nullopt,true};step->actions.push_back({StrokeActionKind::Preview,*step->preview});}anchor=midpoint;pruning=copy[1].p;selected.pop_front();--remaining;
                }
            }
            if(terminal){
                if(remaining==2){const auto& c=copy[n==3?1:0];const auto& e=copy[n==3?2:1];emit(anchor,&c,e);anchor=e;selected.pop_front();selected.pop_front();}
                else if(remaining==1){emit(anchor,nullptr,copy.back());anchor=copy.back();selected.pop_front();}
                batch_flag=true;
            }
        }
    }
    void point(P p,int mode,float pressure=1){
        budget.charge(128);raw_domain(p);if(mode==1)pruning=p;if(mode!=1)cumulative=f32(binary64(double(cumulative)+distance(previous,p)));previous=p;Packet packet{p,cumulative,mode,serial++,pressure};raw.push_back(packet);if(step)step->raw=sample(packet);
        std::vector<Packet> output;
        if(raw.size()==200){pressure_segment(raw.size()-1);raw.clear();output.push_back(packet);} //5CCD20 fixed0C8h cap, independent of+4C.
        else if(raw.size()>1&&distance(raw.front().p,raw.back().p)>=sampling.accuracy){
            //5CA0A0 walks backward, requires >trunc(4*accuracy^2) from
            //both ends and strict perpendicular>accuracy.
            for(std::size_t i=raw.size()-1;i>0;--i){budget.charge();const auto& q=raw[i];if(squared_distance(raw.front().p,q.p)>sampling.endpoint_distance_squared&&squared_distance(raw.back().p,q.p)>sampling.endpoint_distance_squared&&perpendicular(raw.front().p,raw.back().p,q.p)>sampling.accuracy){pressure_segment(i);output.push_back(raw[i]);raw.erase(raw.begin(),raw.begin()+std::ptrdiff_t(i));break;}}
        }
        if(mode){pressure_segment(raw.size()-1);output.push_back(packet);if(raw.size()>1)raw.erase(raw.begin(),std::prev(raw.end()));}
        bool batch=false;for(auto q:output)consume(q,batch);
        if(step){step->prune_raw=!output.empty();step->anchor_serial=anchor.serial;step->prune_point=pruning;}

    }
};
P mapped(P p,const Scope& scope){const P out{integer(binary64(std::floor(binary64(double(p.x)*scope.forward))+scope.offset[0])),integer(binary64(std::floor(binary64(double(p.y)*scope.forward))+scope.offset[1]))};raw_domain(out);return out;}
constexpr std::array<P,8> delta{{{1,0},{1,-1},{0,-1},{-1,-1},{-1,0},{-1,1},{0,1},{1,1}}};
void rectangle(Builder& b,const Scope& scope){
    //5C95E0→5CE0C0: four MCs, shared closure identity; top-left first.
    // The raw working rectangle is center +/- arithmetic half-extents.
    b.result.rectangle=true;const auto r=scope.bounds;const int cx=add(r.left,r.right)>>1,cy=add(r.top,r.bottom)>>1,hx=sub(r.right,r.left)>>1,hy=sub(r.bottom,r.top)>>1;
    const std::array<P,4> ps{{{sub(cx,hx),sub(cy,hy)},{add(cx,hx),sub(cy,hy)},{add(cx,hx),add(cy,hy)},{sub(cx,hx),add(cy,hy)}}};
    for(auto p:ps)b.vertex(p);
    for(int i=0;i<4;++i){b.reserve(2);const int z=(i+1)%4;const auto c=mean(ps[std::size_t(i)],ps[std::size_t(z)]);VectorModelCurve m;m.flags=3;m.line=-1;m.targets={-1,-1};m.crosspoints={-1,-1};m.subcurves={i};b.result.graph.models.push_back({i,std::move(m),{}});b.result.models.push_back(i);b.result.graph.subcurves.push_back({i,{i,z,c.x,c.y,i,{}},{}});b.result.graph.vertices[std::size_t(i)].value.subcurves.push_back(i);b.result.graph.vertices[std::size_t(z)].value.subcurves.push_back(i);}
    vector_geometry_detail::rebuild_models(b.result.graph,b.result.models,b.budget.work,b.budget.stop);
}
}
std::vector<StrokeStep> sample_stroke_packets(std::span<const StrokeSample> input,StrokeSamplingSettings requested,std::uint64_t& storage,Budget budget){
    if(!std::isfinite(requested.accuracy)||requested.accuracy<0||!std::isfinite(requested.max_perpendicular)||requested.max_perpendicular<0)throw std::invalid_argument("Invalid native stroke sampling settings");
    if(input.size()<2||input.size()>65536)throw std::length_error("Native stroke sample limit");
    for(std::size_t i=0;i<input.size();++i){budget.charge();const auto& q=input[i];raw_domain(q.point);
        if(q.mode!=(i==0?1:i+1==input.size()?2:0)||!std::isfinite(q.pressure)||q.pressure<0||q.pressure>1)throw std::invalid_argument("Invalid ordered accepted stroke packet");}

    const auto threshold=integer(binary64(4*binary64(double(requested.accuracy)*requested.accuracy)));
    Sampler sampler{nullptr,budget,{requested.accuracy,requested.max_perpendicular,threshold},{},{}};
    std::vector<StrokeStep> result;charge(storage,input.size()*sizeof(StrokeStep),512ull*1024*1024,"Stroke event storage exceeds512MiB");result.reserve(input.size());
    for(const auto& q:input){
        if(q.mode<0||q.mode>2||!std::isfinite(q.pressure)||q.pressure<0||q.pressure>1)throw std::invalid_argument("Invalid accepted stroke packet");
        result.emplace_back();sampler.step=&result.back();sampler.point(q.point,q.mode,q.pressure);
        charge(storage,(result.back().emitted.size()*sizeof(StrokeSpan)+result.back().pressure_events.size()*sizeof(StrokeSample)+result.back().actions.size()*sizeof(StrokeAction)),512ull*1024*1024,"Stroke event storage exceeds512MiB");
    }
    return result;
}
ContourGraph trace(const Scope& scope,std::uint64_t& storage,Budget budget){
    return trace(scope,storage,budget,ContourProfile::FaceColor);
}
static ContourGraph trace_impl(const Scope& scope,std::uint64_t& storage,Budget budget,ContourProfile profile,const Raster* raster){
    const auto sampling=settings(profile);
    Builder b{{},budget,storage};b.result.graph.high_water.fill(-1);if(!scope.configured||scope.bounds.left>=scope.bounds.right||scope.bounds.top>=scope.bounds.bottom)return std::move(b.result);
    if(!scope.mask)rectangle(b,scope);
    else{
        const auto r=scope.mask_bounds;const auto area=std::uint64_t(r.width)*std::uint64_t(r.height);charge(storage,area,512ull*1024*1024,"Mask contour2bit scratch exceeds512MiB");budget.charge(area);std::vector<unsigned char> pixels(std::size_t(area),0);
        const auto offset=[&](P p){return std::size_t(p.y-r.y)*std::size_t(r.width)+std::size_t(p.x-r.x);};const auto inside=[&](P p){return p.x>=r.x&&p.y>=r.y&&std::int64_t(p.x)<std::int64_t(r.x)+r.width&&std::int64_t(p.y)<std::int64_t(r.y)+r.height;};
        for(int y=r.y;y<std::int64_t(r.y)+r.height;++y){budget.charge(0);for(int x=r.x;x<std::int64_t(r.x)+r.width;++x){if(!raster){if(scope.mask->test(x,y))pixels[offset({x,y})]=3;}else if(raster->contains(x,y)&&raster->tiles.contains({x/256,y/256})){pixels[offset({x,y})]=raster->depth==2?selection_detail::raw_pixel(*raster,x,y):offscreen_detail::color_word(raster_stroke_surface_pixel(*raster,x,y),2);}}}
        //5C91A0/GetBlockArea scan native256 tiles in row/column order,
        // then rows inside each tile. CommonCopy leaves all exterior bits0.
        const auto pixel=[&](P p)->unsigned char{return inside(p)?pixels[offset(p)]:0;};unsigned char previous_left=0,previous_other=0;
        for(int by=(r.y/256)*256;by<std::int64_t(r.y)+r.height;by+=256)for(int bx=(r.x/256)*256;bx<std::int64_t(r.x)+r.width;bx+=256){const bool left_tile=bx==scope.bounds.left;unsigned char first=left_tile?previous_left:previous_other;const int bottom=std::min(by+256,scope.mask->height());
        for(int y=by;y<bottom;++y){unsigned char before=first;first=pixel({bx,y});for(int x=bx;x<std::min(bx+256,scope.mask->width());++x){budget.charge();const P seed{x,y};const auto v=pixel(seed);if(before==0&&v==3){
                b.begin();Sampler sampler{&b,budget,sampling,{},{}};const P first=mapped(seed,scope);sampler.point(first,1);P current=seed;int direction=1;bool previous_marked=false;std::deque<P> history;
                const auto smooth=[&](P p){history.push_back(p);if(history.size()>16)history.pop_front();if(history.size()==1)return p;const auto& a=history[history.size()-2];return P{add(p.x,a.x)/2,add(p.y,a.y)/2};};
                for(std::uint64_t steps=0;;++steps){budget.charge(32);if(steps>area*8+8)throw std::invalid_argument("Mask contour did not close");direction=(direction+3)%8;P next{};bool found=false;
                    for(int i=0;i<8;++i){const auto d=delta[std::size_t(direction)];next={add(current.x,d.x),add(current.y,d.y)};if(inside(next)&&pixels[offset(next)]){found=true;break;}direction=(direction+7)%8;}
                    if(!found)break;
                    if(direction>=1&&direction<=4){pixels[offset(next)]=2;if(!previous_marked)pixels[offset(current)]=2;previous_marked=true;}else previous_marked=false;
                    if(next==seed)break;sampler.point(smooth(mapped(next,scope)),0);current=next;
                }
                sampler.point(smooth(first),0);sampler.point(smooth(first),0);sampler.point(first,2);b.finish();
            }before=v;}}if(left_tile)previous_left=pixel({bx,bottom-1});else previous_other=0;}
    }
    auto& a=b.result.graph;for(std::size_t i=0;i<a.high_water.size();++i){const std::array<std::size_t,6> counts{a.vertices.size(),a.scales.size(),a.subcurves.size(),a.models.size(),a.crosspoints.size(),a.lines.size()};a.high_water[i]=int(counts[i])-1;}
    bool have=false;for(const auto& m:a.models){const auto& s=m.value.scalars;const VectorBounds q{s[0],s[1],s[2],s[3]};if(q.left>=q.right||q.top>=q.bottom)continue;if(!have){b.result.bounds=q;have=true;}else b.result.bounds={std::min(b.result.bounds.left,q.left),std::min(b.result.bounds.top,q.top),std::max(b.result.bounds.right,q.right),std::max(b.result.bounds.bottom,q.bottom)};}
    return std::move(b.result);
}
ContourGraph trace(const Scope& scope,std::uint64_t& storage,Budget budget,ContourProfile profile){return trace_impl(scope,storage,budget,profile,nullptr);}
ContourGraph trace_raster(const Scope& scope,const Raster& raster,std::uint64_t& storage,Budget budget){return trace_impl(scope,storage,budget,ContourProfile::FaceColor,&raster);}

}
