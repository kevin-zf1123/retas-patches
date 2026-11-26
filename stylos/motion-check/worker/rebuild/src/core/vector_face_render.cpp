#include "retas/vector_face_render.hpp"
#include "vector_face_program.hpp"
#include "vector_stored_render_costs.hpp"
#include "vector_scanline_router.hpp"
#include "vector_gradation_admission.hpp"
#include "vector_shape_texture.hpp"
#include "retas/fill.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <stdexcept>
#include <set>

namespace retas {
namespace {
using P=VectorFaceVertex;
constexpr std::uint64_t max_work=128ull*1024*1024,max_points=1024*1024,max_storage=512ull*1024*1024;
constexpr int coordinate_limit=1073741824;
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Vector face rendering cancelled");}
struct Budget {
    std::uint64_t work{},points{},sampled{},storage{};
    void charge(std::uint64_t n){if(n>max_work-work)throw std::length_error("Vector face work limit exceeded");work+=n;}
    void point(){if(++points>max_points)throw std::length_error("Vector face routed point limit exceeded");}
    void bytes(std::uint64_t n){if(n>max_storage-storage)throw std::length_error("Vector face storage limit exceeded");storage+=n;}
};
bool contains(VectorBounds b,P p){return p.x>=b.left&&p.x<b.right&&p.y>=b.top&&p.y<b.bottom;}
bool intersects(VectorBounds a,VectorBounds b){return a.left<b.right&&a.right>b.left&&a.top<b.bottom&&a.bottom>b.top;}
VectorBounds unite(VectorBounds a,VectorBounds b){return {std::min(a.left,b.left),std::min(a.top,b.top),std::max(a.right,b.right),std::max(a.bottom,b.bottom)};}
bool point_ok(P p){return std::abs(std::int64_t(p.x))<=coordinate_limit&&std::abs(std::int64_t(p.y))<=coordinate_limit;}
struct Span {
    P start,control,end;bool linear{};VectorBounds bounds;std::size_t steps{};
};
VectorBounds bounds(P s,P c,P e,bool linear){
    if(linear)c=s;
    // RVLinearLine/RVBezier bounds are the endpoint/control hull, max+1.
    return {std::min({s.x,c.x,e.x}),std::min({s.y,c.y,e.y}),
        std::max({s.x,c.x,e.x})+1,std::max({s.y,c.y,e.y})+1};
}
std::array<double,2> position(const Span& s,float t){
    using numeric_detail::binary64;
    const double u=binary64(1.0-double(t));
    if(s.linear)return {binary64(binary64(s.start.x*u)+binary64(s.end.x*double(t))),
        binary64(binary64(s.start.y*u)+binary64(s.end.y*double(t)))};
    // 551E50 retains u while storing one float32 copy, then stores the
    // middle and t*t weights as binary64 before coordinate evaluation.
    const double uu=binary64(u*float(u)),middle=binary64(binary64(double(float(u))*t)*2.0),tt=binary64(double(t)*t);
    const auto lane=[&](int a,int c,int e){return binary64(binary64(binary64(e*tt)+binary64(c*middle))+binary64(a*uu));};
    return {lane(s.start.x,s.control.x,s.end.x),lane(s.start.y,s.control.y,s.end.y)};
}
P integer_position(const Span& s,float t){const auto v=position(s,t);return {int(std::floor(v[0])),int(std::floor(v[1]))};}
double length(const Span& s){
    using numeric_detail::binary64;
    std::array<double,2> previous{double(s.start.x),double(s.start.y)};double result{};
    for(float t:{.25f,.5f,.75f,1.f}){
        const auto next=position(s,t);const double dx=binary64(next[0]-previous[0]),dy=binary64(next[1]-previous[1]);
        result=binary64(result+std::sqrt(binary64(binary64(dx*dx)+binary64(dy*dy))));previous=next;
    }
    return result;
}
struct Prepared {
    const VectorFaceGeometry& geometry;
    std::vector<std::vector<Span>> models;
    std::vector<VectorBounds> model_bounds,loop_bounds;
    VectorBounds face_bounds{};
    bool native_cached_gates{};
};
Prepared prepare_geometry(const VectorFaceGeometry& g,std::stop_token stop={},bool require_material=true){
    if(((g.material_flags&15)!=1&&(g.material_flags&15)!=2&&(g.material_flags&15)!=4)||(require_material&&((g.material_flags&15)==2&&!g.gradation))||(require_material&&((g.material_flags&15)==4&&!g.texture))||g.loops.empty()||g.outer_loop>=g.loops.size()||
       g.vertices.size()>100000||g.models.size()>100000||g.loops.size()>100000||g.holes.size()>100000)
        throw std::invalid_argument("Unsupported vector solid-face graph");
    Prepared p{g,{},{},{},{}};p.models.reserve(g.models.size());p.model_bounds.reserve(g.models.size());p.loop_bounds.reserve(g.loops.size());
    for(const auto& v:g.vertices)if(!point_ok(v))throw std::invalid_argument("Vector face coordinate outside the bounded raw domain");
    std::size_t spans{},members{};
    for(const auto& model:g.models){
        poll(stop);
        if((model.flags!=2&&model.flags!=3)||model.spans.empty()||model.spans.size()>100000-spans)
            throw std::invalid_argument("Unsupported vector face model flags or spans");
        const auto owner_ok=[&](int index){return index==-1||(index>=0&&std::size_t(index)<g.loops.size());};
        if(!owner_ok(model.outside_loop)||!owner_ok(model.inside_loop))throw std::invalid_argument("Invalid vector face adjacent loop reference");
        spans+=model.spans.size();std::vector<Span> ready;ready.reserve(model.spans.size());
        for(const auto& s:model.spans){
            if(s.start>=g.vertices.size()||s.end>=g.vertices.size()||!point_ok({s.control_x,s.control_y}))
                throw std::invalid_argument("Invalid vector face span reference or control");
            if(!ready.empty()&&model.spans[ready.size()-1].end!=s.start)
                throw std::invalid_argument("Disconnected vector face model spans");
            Span item{g.vertices[s.start],{s.control_x,s.control_y},g.vertices[s.end],bool(model.flags&1),{},0};
            for(const auto axis:{0,1}){
                const auto lane=[&](P q){return std::int64_t(axis?q.y:q.x);};
                const auto delta=lane(item.end)-lane(item.start);
                if(std::abs(delta)>std::numeric_limits<int>::max())
                    throw std::invalid_argument("Vector face signed endpoint difference overflow");
                const auto a=lane(item.end)-2*lane(item.control)+lane(item.start),b=2*(lane(item.control)-lane(item.start));
                if(!item.linear&&(a<std::numeric_limits<int>::min()||a>std::numeric_limits<int>::max()||b<std::numeric_limits<int>::min()||b>std::numeric_limits<int>::max()))
                    throw std::invalid_argument("Vector face quadratic coefficient overflow");
            }
            item.bounds=bounds(item.start,item.control,item.end,item.linear);ready.push_back(item);
        }
        auto box=ready.front().bounds;for(const auto& s:ready)box=unite(box,s.bounds);
        p.model_bounds.push_back(box);p.models.push_back(std::move(ready));
    }
    std::vector<std::array<std::size_t,2>> sides(g.models.size());
    for(std::size_t li=0;li<g.loops.size();++li){
        poll(stop);const auto& loop=g.loops[li];
        if((loop.members.empty()&&li!=g.outer_loop)||loop.members.size()>100000-members)throw std::invalid_argument("Unsupported vector face loop member count");
        members+=loop.members.size();std::uint32_t first{},last{};bool started{};VectorBounds box{};
        for(const auto& member:loop.members){
            if(member.model>=g.models.size())throw std::invalid_argument("Invalid vector face member model reference");
            const auto& model=g.models[member.model];const auto forward=member.selector!=0;
            const auto owner=forward?model.inside_loop:model.outside_loop;
            if(owner!=int(li)||++sides[member.model][forward?1:0]!=1)
                throw std::invalid_argument("Nonreciprocal or duplicate vector face loop ownership");
            const auto begin=forward?model.spans.front().start:model.spans.back().end;
            const auto end=forward?model.spans.back().end:model.spans.front().start;
            if(started&&last!=begin)throw std::invalid_argument("Disconnected vector face loop identities");
            if(!started){first=begin;box=p.model_bounds[member.model];started=true;}
            else box=unite(box,p.model_bounds[member.model]);last=end;
        }
        if(last!=first)throw std::invalid_argument("Open vector face loop");p.loop_bounds.push_back(box);
    }
    for(std::size_t i=0;i<g.models.size();++i)for(int side=0;side<2;++side){
        const auto owner=side?g.models[i].inside_loop:g.models[i].outside_loop;
        if((owner>=0)!=(sides[i][std::size_t(side)]==1))throw std::invalid_argument("Unbound vector face adjacent loop owner");
    }
    std::vector<bool> used(g.loops.size());used[g.outer_loop]=true;
    p.face_bounds=p.loop_bounds[g.outer_loop];
    for(auto hole:g.holes){
        if(hole>=g.loops.size()||used[hole])throw std::invalid_argument("Invalid or duplicate vector face hole");
        //5987E0 gates the Face by its outer alone. A hole outside that box
        // must not expand the set of visited physical blocks.
        used[hole]=true;
    }
    return p;
}
std::vector<int> crossings(const Span& s,bool horizontal,int query){
    const int axis=horizontal?1:0;const auto lane=[&](P q){return axis?q.y:q.x;};
    if(query<(horizontal?s.bounds.top:s.bounds.left)||query>=(horizontal?s.bounds.bottom:s.bounds.right))return {};
    const double start=lane(s.start),control=lane(s.control),end=lane(s.end);std::array<double,2> roots{};int n{};
    if(s.linear){
        const auto delta=end-start;
        if(delta==0){if(start==query){roots={0,1};n=2;}}
        else {roots[0]=(double(query)-start+.125)/delta;n=1;}
    }else{
        const double a=end-2*control+start,b=2*(control-start),c=start-(double(query)+.125);
        if(a==0){if(b!=0){roots[0]=-c/b;n=1;}}
        else {
            using numeric_detail::binary64;
            const auto d=binary64(binary64(b*b)-binary64(binary64(a*c)*4));
            if(d>=0){
                if(d==0)roots={-b/(a+a),-b/(a+a)};
                else {const auto z=std::sqrt(d),scale=.5/a;roots={binary64(binary64(z-b)*scale),binary64(binary64(-b-z)*scale)};}
                n=2;
            }
        }
    }
    std::vector<int> result;result.reserve(2);
    for(int i=0;i<n;++i)if(roots[std::size_t(i)]>=0&&roots[std::size_t(i)]<=1){
        const auto point=integer_position(s,float(roots[std::size_t(i)]));result.push_back(horizontal?point.x:point.y);
    }
    return result;
}
int classify_loop(const Prepared& p,std::size_t loop,P point,Budget* budget=nullptr){
    if(!contains(p.loop_bounds[loop],point))return 0;
    const auto ray=[&](bool horizontal){
        std::vector<int> values;
        for(const auto& member:p.geometry.loops[loop].members){
            if(budget&&p.native_cached_gates)budget->charge(1);
            const auto b=p.model_bounds[member.model];
            // 59E950 filters original-span rays by the saved MC center axis.
            if(p.native_cached_gates&&(horizontal?(point.y<b.top||point.y>=b.bottom):(point.x<b.left||point.x>=b.right)))continue;
            for(const auto& span:p.models[member.model]){
                if(budget)budget->charge(1);const auto next=crossings(span,horizontal,horizontal?point.y:point.x);
                values.insert(values.end(),next.begin(),next.end());
            }
        }
        return values;
    };
    const auto x=ray(true);if(x.empty())return 0;
    const bool odd=x.size()%2;const auto equal=std::count(x.begin(),x.end(),point.x);
    if(!odd&&!equal)return std::count_if(x.begin(),x.end(),[&](int v){return v<point.x;})%2?1:0;
    const auto y=ray(false);if(y.empty())return 0;
    if(odd||std::find(y.begin(),y.end(),point.y)!=y.end())return 2;
    return std::count_if(y.begin(),y.end(),[&](int v){return v<point.y;})%2?1:0;
}
bool region_contains(const Prepared& p,P point,Budget* budget=nullptr){
    //5A1CF0 skips the outer predicate when its native bbox is empty. This
    //unbounded query is distinct from5987E0's empty outer drawing gate.
    const auto outer=p.geometry.outer_loop;const auto box=p.loop_bounds[outer];
    const bool bounded=p.native_cached_gates?box.left<box.right&&box.top<box.bottom:!p.geometry.loops[outer].members.empty();
    if(bounded&&!classify_loop(p,outer,point,budget))return false;
    for(auto hole:p.geometry.holes)if(classify_loop(p,hole,point,budget))return false;
    return true;
}
using Router=vector_scan_detail::Router<Budget>;
// Shared geometric output retains the native24.8 crossings until the writer.
// A no-routed-point rectangle uses the full-sample writer, not649BD0.
struct Run {int y{};std::int64_t left{},right{};bool rectangle{};};
struct BlockPlan {VectorBounds block;std::vector<Run> runs;};
struct Plan {std::vector<BlockPlan> blocks;};
std::int64_t floor_fixed(std::int64_t v){return v>=0?v/256:-((-v+255)/256);}
void add_run(BlockPlan& plan,int y,std::int64_t left,std::int64_t right,bool rectangle,int mode,Budget& budget){
    if(left>=right||(mode==1&&floor_fixed(left)>=floor_fixed(right)))return;
    budget.charge(std::uint64_t(floor_fixed(right)-floor_fixed(left)+2));
    budget.bytes(2*sizeof(Run));plan.runs.push_back({y,left,right,rectangle});
}
struct Context {VectorBounds clip;int curve_step{};std::uint64_t logical_storage{};};
Context validate_context(const VectorRenderContext& ctx,int mode=1,int reference=0){
    if(ctx.width<=0||ctx.height<=0||ctx.width>100000||ctx.height>100000||std::uint64_t(ctx.width)*ctx.height>268435456||
       (ctx.depth!=32&&ctx.depth!=64)||!std::isfinite(ctx.resolution)||ctx.resolution<=0||(ctx.clipboard_affine||reference?(!std::isfinite(ctx.base_scale)||ctx.base_scale<=0):ctx.base_scale!=16)||ctx.oversample!=unsigned(mode)||
       (reference&&(ctx.converter_fine_step||(!ctx.clipboard_affine&&ctx.origin!=VectorPoint{})))||
       (reference==1&&(ctx.depth!=32||(mode!=1&&mode!=2)))||
       !std::isfinite(ctx.origin.x)||!std::isfinite(ctx.origin.y)||(!ctx.clipboard_affine&&(std::floor(ctx.origin.x)!=ctx.origin.x||std::floor(ctx.origin.y)!=ctx.origin.y)))
        throw std::invalid_argument("Vector face requires a normal same-DPI direct32/64 context");
    for(double edge:{ctx.origin.x*ctx.base_scale,ctx.origin.y*ctx.base_scale,(ctx.origin.x+ctx.width)*ctx.base_scale,(ctx.origin.y+ctx.height)*ctx.base_scale})
        if(std::abs(edge)>=coordinate_limit)throw std::invalid_argument("Vector face context raw bounds exceed supported domain");
    // 5E0340 uses dbl_9DA208 bits3FA0204081020408. A shorter decimal
    // rounding to the preceding double changes integer sampling thresholds.
    const auto step=std::trunc(numeric_detail::binary64(numeric_detail::binary64(ctx.resolution*ctx.base_scale)*0x1.0204081020408p-5)*(ctx.oversample==4?.25:1));
    if(step<1||step>std::numeric_limits<int>::max())throw std::invalid_argument("Unsupported vector face sampling resolution");
    VectorBounds clip{0,0,ctx.width,ctx.height};
    if(ctx.clip){
        if(ctx.clip->right<ctx.clip->left||ctx.clip->bottom<ctx.clip->top)throw std::invalid_argument("Invalid vector face clip");
        clip={std::clamp(ctx.clip->left,0,ctx.width),std::clamp(ctx.clip->top,0,ctx.height),std::clamp(ctx.clip->right,0,ctx.width),std::clamp(ctx.clip->bottom,0,ctx.height)};
    }
    const auto tiles=std::uint64_t((ctx.width+255)/256)*((ctx.height+255)/256)*256*256*(ctx.depth/8);
    const auto mask=std::uint64_t((ctx.width+31)/32)*ctx.height*4+std::uint64_t((ctx.width+255)/256)*((ctx.height+255)/256);
    if(tiles+mask>max_storage)throw std::length_error("Vector face logical tile/mask budget exceeds512MiB");
    return {clip,int(step),tiles+mask};
}
VectorBounds raw_block(VectorBounds block,const VectorRenderContext& ctx){
    if(ctx.clipboard_affine)return vector_render_detail::clipboard_raw_block(block,ctx);
    const int halo=int(std::trunc(ctx.base_scale*(ctx.oversample==4?.75:ctx.oversample==2?.5:0)));
    return {int((block.left+ctx.origin.x)*ctx.base_scale),int((block.top+ctx.origin.y)*ctx.base_scale),int((block.right+ctx.origin.x)*ctx.base_scale)+halo,int((block.bottom+ctx.origin.y)*ctx.base_scale)+halo};
}
Plan prepare_plan(Prepared& p,const VectorRenderContext& ctx,Budget& budget,std::stop_token stop,int mode=1,bool charge_context=true,const VectorBounds* gate=nullptr,const VectorBounds* region_gate=nullptr,int reference=0){
    const auto admission=validate_context(ctx,mode,reference);if(charge_context)budget.bytes(admission.logical_storage);
    //5987E0 intersects the embedded outer bbox before material setup or
    //5A2150 emission. An admitted empty outer has the canonical empty gate:
    //retain the Face and its certified holes, but never draw those holes as
    //positive contours or turn the Region's infinite query into pigment.
    if(p.geometry.loops[p.geometry.outer_loop].members.empty()){
        if(region_gate&&region_gate->left<region_gate->right&&region_gate->top<region_gate->bottom)
            throw std::invalid_argument("Empty vector outer has a positive stale drawing gate");
        poll(stop);return {};
    }
    for(auto& model:p.models)for(auto& s:model){
        poll(stop);s.steps=s.linear?1:std::size_t(std::max(2.0,std::trunc(length(s)/admission.curve_step)));
        if(s.steps>max_points||s.steps+1>max_points-budget.sampled)throw std::length_error("Vector face sampled point limit exceeded");budget.sampled+=s.steps+1;
        // Temporary samples/routing/map/edge arrays are bounded independently
        // of actual paint area before allocating output raster or mask.
        budget.bytes(128*(s.steps+1));
    }
    Plan plan;const auto c=admission.clip;
    if(c.right<=c.left||c.bottom<=c.top)return plan;
    for(int ty=c.top/256;ty*256<c.bottom;++ty)for(int tx=c.left/256;tx*256<c.right;++tx){
        poll(stop);budget.charge(1);const VectorBounds block{tx*256,ty*256,std::min(ctx.width,(tx+1)*256),std::min(ctx.height,(ty+1)*256)};
        // AA owner scratch must retain the entire physical block even for a
        // one-pixel query. Publication clipping happens after owner resolve.
        const VectorBounds publish=mode>1?VectorBounds{block.left*mode,block.top*mode,block.right*mode,block.bottom*mode}:
            VectorBounds{std::max(block.left,c.left),std::max(block.top,c.top),std::min(block.right,c.right),std::min(block.bottom,c.bottom)};
        BlockPlan program{block,{}};
        const auto raw=raw_block(block,ctx);
        // Shape5A86F0 uses a nonempty RCRect intersection before calling Face.
        if(gate&&(gate->left>=gate->right||gate->top>=gate->bottom||!intersects(*gate,raw)))continue;
        if(region_gate&&(region_gate->left>=region_gate->right||region_gate->top>=region_gate->bottom||!intersects(*region_gate,raw)))continue;
        if(!intersects(p.face_bounds,raw))continue;
        Router router{raw,block,ctx,budget,{},{},{},{},false,false,false,reference!=0};
        const auto emit=[&](std::uint32_t loop){
            if(!intersects(p.loop_bounds[loop],raw))return;
            for(const auto& member:p.geometry.loops[loop].members){
                poll(stop);budget.charge(1);const auto& owner=p.geometry.models[member.model];
                if(owner.outside_loop==owner.inside_loop){router.flush();continue;}
                if(!intersects(p.model_bounds[member.model],raw)){router.last_valid=false;continue;}
                const auto& model=p.models[member.model];
                const auto span=[&](const Span& s,bool forward){
                    if(!s.linear&&!intersects(s.bounds,raw)){router.last_valid=false;return;}
                    const auto send=[&](std::size_t i){
                        if(!(i%256))poll(stop);
                        if(i==0)router.route(s.start);else if(i==s.steps)router.route(s.end);
                        else router.route(integer_position(s,float(double(i)*float(1.0/double(s.steps)))));
                    };
                    if(forward)for(std::size_t i=0;i<=s.steps;++i)send(i);
                    else for(std::size_t i=s.steps+1;i-->0;)send(i);
                };
                if(member.selector)for(const auto& s:model)span(s,true);
                else for(auto i=model.rbegin();i!=model.rend();++i)span(*i,false);
            }
            router.flush();
        };
        emit(p.geometry.outer_loop);for(auto hole:p.geometry.holes)emit(hole);
        if(router.points.empty()){
            if(p.face_bounds.left<=raw.left&&p.face_bounds.top<=raw.top&&p.face_bounds.right>=raw.right&&p.face_bounds.bottom>=raw.bottom){
                const P center{int((std::int64_t(raw.left)+raw.right)/2),int((std::int64_t(raw.top)+raw.bottom)/2)};
                if(region_contains(p,center,&budget))for(int y=publish.top;y<publish.bottom;++y)add_run(program,y,std::int64_t(publish.left)*256,std::int64_t(publish.right)*256,true,mode,budget);
            }
            if(!program.runs.empty()){budget.bytes(2*sizeof(BlockPlan));plan.blocks.push_back(std::move(program));}
            continue;
        }
        router.finish([&](P q){return region_contains(p,q,&budget);});budget.bytes(router.points.size()*64);
        struct Edge {int y0{},y1{};std::int64_t x{},slope{};};std::vector<Edge> edges;edges.reserve(router.points.size());
        for(std::size_t i=0;i<router.points.size();++i){
            auto a=router.points[i],b=router.points[(i+1)%router.points.size()];
            if(a.y==b.y||a.y==std::numeric_limits<int>::max()||b.y==std::numeric_limits<int>::max())continue;
            if(a.y>b.y)std::swap(a,b);
            if(std::abs(std::int64_t(a.x))>1048576||std::abs(std::int64_t(b.x))>1048576)
                throw std::length_error("Vector face fixed-point coordinate limit exceeded");
            edges.push_back({a.y,b.y,std::int64_t(a.x)*256,std::int64_t(b.x-a.x)*256/(b.y-a.y)});
        }
        std::vector<std::int64_t> active;active.reserve(edges.size());
        for(int y=publish.top;y<publish.bottom;++y){
            poll(stop);budget.charge(edges.size());active.clear();
            for(const auto& e:edges)if(y>=e.y0&&y<e.y1)active.push_back(e.x+std::int64_t(y-e.y0)*e.slope);
            budget.charge(active.size()*std::bit_width(active.size()));std::sort(active.begin(),active.end());
            if(active.size()%2)throw std::invalid_argument("Unpaired vector face scanline crossings");
            for(std::size_t i=1;i<active.size();i+=2){
                const auto left=std::max<std::int64_t>(std::int64_t(publish.left)*256,active[i-1]);
                const auto right=std::min<std::int64_t>(std::int64_t(publish.right)*256,active[i]);
                add_run(program,y,left,right,false,mode,budget);
            }
        }
        if(ctx.clipboard_affine||!program.runs.empty()){budget.bytes(2*sizeof(BlockPlan));plan.blocks.push_back(std::move(program));}
    }
    poll(stop);return plan;
}
Pixel pigment(std::uint64_t color,int depth){
    Pixel p{std::uint16_t(color),std::uint16_t(color>>16),std::uint16_t(color>>32),std::uint16_t(color>>48)};
    return depth==32?Pixel::rgba8(p.r>>8,p.g>>8,p.b>>8,p.a>>8):p;
}
Pixel sample_pigment(std::uint64_t color,int depth){
    Pixel p{std::uint16_t(color),std::uint16_t(color>>16),std::uint16_t(color>>32),std::uint16_t(color>>48)};
    if(depth==32)p={std::uint16_t(p.r>>8),std::uint16_t(p.g>>8),std::uint16_t(p.b>>8),std::uint16_t(p.a>>8)};
    return p;
}
// 648180/6483C0 with a4=0: face edge coverage adds alpha without
// attenuating the previous face. Arithmetic products retain native DWORDs.
void partial_face(Pixel& dst,Pixel src,unsigned fraction,int depth){
    const unsigned bits=depth==32?8:16,max=depth==32?255:65535,unit=1u<<bits;
    const unsigned k=std::min(unit,unsigned((std::uint64_t(fraction)*unit)/256+1));
    const std::uint32_t q=std::uint32_t(src.a)*k,sa=q>>bits;
    if(!sa)return;
    if(!dst.a){dst=src;dst.a=std::uint16_t(sa);return;}
    const unsigned total=unsigned(dst.a)+sa,dw=(unsigned(dst.a)<<bits)/total,sw=q/total;
    const auto lane=[&](unsigned d,unsigned s){
        const auto value=(std::uint32_t(d*dw)+std::uint32_t(s*sw))>>bits;
        return std::uint16_t(depth==32?std::min(255u,value):value);
    };
    dst={lane(dst.r,src.r),lane(dst.g,src.g),lane(dst.b,src.b),std::uint16_t(std::min(max,total))};
}
// 647EB0/648000 compose1. The mixed64 path literally zero-extends each
// low byte after its WORD calculation, including alpha; the empty shortcut
// copies all WORDs. This quirk precedes AA64's high-byte alpha weighting.
void compose_sample(Pixel& dst,Pixel src,int depth){
    const unsigned bits=depth==32?8:16,max=depth==32?255:65535;
    if(!src.a||dst.a==max)return;
    if(!dst.a){dst=src;return;}
    const unsigned sa=unsigned((std::uint64_t(max-dst.a)*src.a)/max),total=unsigned(dst.a)+sa;
    const unsigned dw=(unsigned(dst.a)<<bits)/total,sw=(sa<<bits)/total;
    const auto lane=[&](unsigned d,unsigned s){
        const auto value=std::min(max,(std::uint32_t(d*dw)+std::uint32_t(s*sw))>>bits);
        return std::uint16_t(depth==64?value&255:value);
    };
    dst={lane(dst.r,src.r),lane(dst.g,src.g),lane(dst.b,src.b),std::uint16_t(depth==64?total&255:total)};
}
Pixel resolve_samples(const std::array<Pixel,4>& samples,int depth){
    unsigned weight{},alpha{};for(auto p:samples){weight+=depth==32?p.a:p.a>>8;alpha+=p.a;}
    if(!weight)return {};
    const auto lane=[&](auto member){
        unsigned sum{};for(auto p:samples)sum+=unsigned(p.*member)*(weight>=1020?1:(depth==32?p.a:p.a>>8));
        return std::uint16_t(weight>=1020?sum>>2:sum/weight);
    };
    Pixel result{lane(&Pixel::r),lane(&Pixel::g),lane(&Pixel::b),std::uint16_t(alpha>>2)};
    return depth==32?Pixel::rgba8(result.r,result.g,result.b,result.a):result;
}
// Native648280 gradient32 partial: WORD source precision, BYTE destination.
// Two RGB passes are literal ASM, including byte truncation after the first.
void partial_gradient32(Pixel& dst,Pixel src,unsigned fraction){
    const unsigned k=std::min(256u,fraction+1),q=(unsigned(src.a)*k)>>8,sa=q>>8;
    if(!sa)return;
    if(!dst.a){dst={std::uint16_t(src.r>>8),std::uint16_t(src.g>>8),std::uint16_t(src.b>>8),std::uint16_t(sa)};return;}
    const unsigned total=unsigned(dst.a)+sa,dw=(unsigned(dst.a)<<16)/total,sw=q/total;
    const auto lane=[&](unsigned d,unsigned word){
        const unsigned first=((d*dw+word*sw)>>16)&255;
        return std::uint16_t(std::min(255u,(first*dw+word*sw)>>16));
    };
    dst={lane(dst.r,src.r),lane(dst.g,src.g),lane(dst.b,src.b),std::uint16_t(std::min(255u,total))};
}
std::array<double,6> texture_inverse(const VectorTextureMaterial& material,const VectorRenderContext& ctx){
    auto inverse=vector_texture_inverse_matrix(material);if(ctx.clipboard_affine){
        const auto& m=ctx.clipboard_inverse;const auto old=inverse;using numeric_detail::binary64;
        for(unsigned row:{0u,3u})for(unsigned col:{0u,1u,2u}){
            const auto a=binary64(m[row]*0.0625),b=binary64(m[row+1]*0.0625),c=binary64(m[row+2]*0.0625);
            inverse[row+col]=binary64(binary64(binary64(a*old[col])+binary64(b*old[col+3]))+(col==2?c:0));
        }
    }return inverse;
}
void admit_texture_queries(const Plan& plan,const VectorRenderContext& ctx,const VectorTextureMaterial& material,int mode=2,int reference=0){
    const auto inverse=texture_inverse(material,ctx);
    const auto origin=ctx.clipboard_affine?VectorPoint{}:ctx.origin;
    for(const auto& block:plan.blocks)for(const auto& run:block.runs){
        const int first=int(floor_fixed(run.left)),last=int(floor_fixed(run.right));
        const auto admit=[&](int a,int b){if(a>b)return;
            const auto x0=(ctx.clipboard_affine||reference)?vector_render_detail::clipboard_material_coordinate(a,ctx):int(std::floor((double(a)/mode+origin.x)*ctx.base_scale)),x1=(ctx.clipboard_affine||reference)?vector_render_detail::clipboard_material_coordinate(b,ctx):int(std::floor((double(b)/mode+origin.x)*ctx.base_scale));
            const auto y=(ctx.clipboard_affine||reference)?vector_render_detail::clipboard_material_coordinate(run.y,ctx):int(std::floor((double(run.y)/mode+origin.y)*ctx.base_scale));
            admit_vector_texture_rectangle(inverse,x0,y,x1,y);
        };
        if(mode==1||run.rectangle){admit(first,last-1);continue;}
        //649BD0 samples every logical column I..J, including the zero-width
        // tail J before its pixel writer skips alpha0. Query admission keeps
        // that endpoint even when no tail byte is published at the clip edge.
        admit(first,last);
    }
}
struct FacePlan {Pixel color;std::optional<VectorGradationSampler> gradient;std::shared_ptr<const VectorTextureSampler> texture;std::shared_ptr<const VectorTextureMaterial> texture_source;Plan plan;std::shared_ptr<const VectorGradationMaterial> gradation_source;std::shared_ptr<const VectorShapeTextureRecipe> texture_recipe_source;};
struct OwnerPlan {Context context;std::vector<std::vector<FacePlan>> planes;std::vector<VectorBounds> blocks;vector_render_detail::StoredLineCacheCosts costs;};
bool block_less(VectorBounds a,VectorBounds b){return a.top<b.top||(a.top==b.top&&a.left<b.left);}
const BlockPlan* find_block(const Plan& plan,VectorBounds block){
    const auto i=std::lower_bound(plan.blocks.begin(),plan.blocks.end(),block,[](const BlockPlan& p,VectorBounds b){return block_less(p.block,b);});
    return i!=plan.blocks.end()&&i->block==block?&*i:nullptr;
}
OwnerPlan prepare_owner(std::span<const VectorFacePlaneView> planes,const VectorRenderContext& ctx,std::stop_token stop,
                        vector_render_detail::StoredLineCacheCosts previous={}){
    poll(stop);
    if(previous.storage>max_storage||previous.work>max_work)throw std::length_error("Vector face preceding resource limit exceeded");
    OwnerPlan result{validate_context(ctx,2),{},{},{}};Budget budget{previous.work,0,0,previous.storage};budget.bytes(result.context.logical_storage);
    if(planes.size()>100000)throw std::length_error("Vector face plane count limit exceeded");
    std::size_t material_stops{},material_faces{};
    std::map<const VectorTextureMaterial*,std::shared_ptr<const VectorTextureSampler>> texture_cache;
    std::array<std::size_t,6> preliminary{};
    for(const auto& plane:planes){
        if(plane.faces.size()>100000-material_faces)throw std::length_error("Vector face owner count limit exceeded");
        material_faces+=plane.faces.size();
        for(const auto* g:plane.faces){
        poll(stop);if(!g)throw std::invalid_argument("Null vector face owner reference");
        const auto bill=[&](std::size_t field,std::size_t n){if(n>100000-preliminary[field])throw std::length_error("Vector face owner aggregate graph limit exceeded");preliminary[field]+=n;};
        bill(0,g->vertices.size());bill(1,g->models.size());bill(2,g->loops.size());bill(3,g->holes.size());
        for(const auto& m:g->models)bill(4,m.spans.size());for(const auto& l:g->loops)bill(5,l.members.size());
        if((g->material_flags&15)==2){if(!g->gradation)throw std::invalid_argument("Null face gradation material");
            if(g->gradation->stops.size()>100000-material_stops)throw std::length_error("Vector face owner aggregate stop limit exceeded");
            material_stops+=g->gradation->stops.size();}
        if((g->material_flags&15)==4){if(!g->texture)throw std::invalid_argument("Null face texture material");
            if(texture_cache.emplace(g->texture.get(),nullptr).second)budget.bytes(2*vector_texture_storage(*g->texture));}
        }
    }
    budget.bytes(2*material_stops*sizeof(VectorGradationStop)); // admitted source plus immutable sampler snapshots
    budget.bytes(2*512ull*512*sizeof(Pixel)+planes.size()*sizeof(std::vector<FacePlan>));
    std::uint64_t vertices{},models{},loops{},holes{},spans{},members{},faces{},stops{};
    result.planes.reserve(planes.size());
    for(const auto& plane:planes){
        poll(stop);if(plane.faces.size()>100000-faces)throw std::length_error("Vector face owner count limit exceeded");
        faces+=plane.faces.size();std::vector<FacePlan> ready;ready.reserve(plane.faces.size());
        for(const auto* g:plane.faces){
            poll(stop);if(!g)throw std::invalid_argument("Null vector face owner reference");
            const auto admit=[&](std::uint64_t& count,std::size_t n){if(n>100000-count)throw std::length_error("Vector face owner graph count limit exceeded");count+=n;};
            if((g->material_flags&15)==2){
                if(!g->gradation)throw std::invalid_argument("Null face gradation material");
                admit(stops,g->gradation->stops.size());budget.charge(g->gradation->stops.size());

            }
            const auto previous_spans=spans;
            admit(vertices,g->vertices.size());admit(models,g->models.size());
            admit(loops,g->loops.size());admit(holes,g->holes.size());
            for(const auto& m:g->models)admit(spans,m.spans.size());for(const auto& l:g->loops)admit(members,l.members.size());
            budget.charge(g->vertices.size()+g->models.size()+g->loops.size()+g->holes.size()+spans-previous_spans);
            budget.bytes(g->vertices.size()*sizeof(P)+g->models.size()*sizeof(std::vector<Span>)+g->loops.size()*sizeof(VectorBounds)+
                (spans-previous_spans)*sizeof(Span)+2*sizeof(FacePlan));
            std::optional<VectorGradationSampler> gradient;
            if((g->material_flags&15)==2){
                gradient.emplace(g->gradation);
                // Include the final partial query at the physical block right
                // boundary. Context admission already bounds these raw sums.
                gradient->admit_rectangle(int(ctx.origin.x*16),int(ctx.origin.y*16),
                    int((ctx.origin.x+ctx.width)*16),int((ctx.origin.y+ctx.height)*16));
            }
            auto prepared=prepare_geometry(*g,stop);auto plan=prepare_plan(prepared,ctx,budget,stop,2,false);
            if(gradient)for(const auto& b:plan.blocks)for(const auto& run:b.runs)
                budget.charge(std::uint64_t(floor_fixed(run.right)-floor_fixed(run.left)+2)*std::uint64_t(16+std::bit_width(g->gradation->stops.size())));
            const auto source=(g->material_flags&15)==4?g->texture:nullptr;
            if(source){admit_texture_queries(plan,ctx,*source);for(const auto& b:plan.blocks)for(const auto& run:b.runs)budget.charge(std::uint64_t(floor_fixed(run.right)-floor_fixed(run.left)+2)*64);}
            for(const auto& block:plan.blocks){budget.bytes(2*sizeof(VectorBounds));result.blocks.push_back(block.block);}
            ready.push_back({sample_pigment(g->color,ctx.depth),std::move(gradient),{},source,std::move(plan),{},{}});
        }
        result.planes.push_back(std::move(ready));
    }
    budget.charge(result.blocks.size()*std::bit_width(result.blocks.size()));
    std::sort(result.blocks.begin(),result.blocks.end(),block_less);
    result.blocks.erase(std::unique(result.blocks.begin(),result.blocks.end()),result.blocks.end());
    // Bound the block/list searches and all subsequent scratch/companion and
    // owner-resolution work before any output Raster is allocated.
    budget.charge(result.blocks.size()*(faces+planes.size())*(1+std::bit_width(result.blocks.size())));
    for(auto block:result.blocks){
        poll(stop);const auto area=std::uint64_t(block.right-block.left)*(block.bottom-block.top);
        budget.charge(area);
        for(const auto& plane:result.planes){
            bool painted{};for(const auto& face:plane)if(find_block(face.plan,block)){painted=true;break;}
            if(painted)budget.charge(area*4);
        }
    }
    result.costs={budget.storage,budget.work};
    // All plans, actual query domains and owner budgets precede any image
    // snapshot. Each distinct admitted source is deep-copied only once.
    for(auto& plane:result.planes)for(auto& face:plane)if(face.texture_source){
        poll(stop);auto& cached=texture_cache.at(face.texture_source.get());
        if(!cached)cached=std::make_shared<VectorTextureSampler>(*face.texture_source,stop);
        face.texture=cached;face.texture_source.reset();
    }
    poll(stop);return result;
}
void paint_block(std::vector<Pixel>& scratch,const BlockPlan& plan,const FacePlan& face,const VectorRenderContext& ctx,std::stop_token stop){
    const int depth=ctx.depth;const auto color=face.color;
    const auto query=[&](int x,int y){const auto rx=int(std::int64_t(x)*8+std::int64_t(ctx.origin.x)*16),ry=int(std::int64_t(y)*8+std::int64_t(ctx.origin.y)*16);return face.texture?face.texture->sample(rx,ry):face.gradient->sample(rx,ry);};
    const auto full=[&](Pixel& dst,int x,int y){if(!face.gradient&&!face.texture){dst=color;return;}
        const auto p=query(x,y);dst=depth==32?Pixel{std::uint16_t(p.r>>8),std::uint16_t(p.g>>8),std::uint16_t(p.b>>8),std::uint16_t(p.a>>8)}:p;};
    const auto partial=[&](Pixel& dst,int x,int y,unsigned fraction){if(!face.gradient&&!face.texture){partial_face(dst,color,fraction,depth);return;}
        const auto p=query(x,y);if(depth==32&&face.gradient)partial_gradient32(dst,p,fraction);
        else if(depth==32)partial_face(dst,{std::uint16_t(p.r>>8),std::uint16_t(p.g>>8),std::uint16_t(p.b>>8),std::uint16_t(p.a>>8)},fraction,depth);
        else partial_face(dst,p,fraction,depth);};
    const int width=(plan.block.right-plan.block.left)*2,height=(plan.block.bottom-plan.block.top)*2;
    const int left=plan.block.left*2,top=plan.block.top*2;
    for(const auto& run:plan.runs){
        poll(stop);const int y=run.y-top;if(y<0||y>=height)throw std::invalid_argument("Vector face sample row outside scratch");
        auto* row=scratch.data()+std::size_t(y)*width;
        const int first=int(floor_fixed(run.left))-left,last=int(floor_fixed(run.right))-left;
        if(run.rectangle){for(int x=first;x<last;++x)full(row[x],x+left,run.y);continue;}
        // 649BD0 advances the physical pointer once after the first partial,
        // even when both endpoints have the same floor. The second partial
        // therefore writes the next cell in that case, with the original
        // logical floor used only by native non-solid pigment queries.
        if(first>=0&&first<width)partial(row[first],first+left,run.y,unsigned((std::int64_t(first+left+1)*256)-run.left));
        int pointer=first+1;
        for(;pointer<last;++pointer)full(row[pointer],pointer+left,run.y);
        if(pointer>=0&&pointer<width)partial(row[pointer],last+left,run.y,unsigned(run.right-std::int64_t(last+left)*256));
    }
}
Raster render_owner(const OwnerPlan& plan,const VectorRenderContext& ctx,std::stop_token stop){
    poll(stop);Raster result(ctx.width,ctx.height,ctx.depth,ctx.resolution);
    for(auto block:plan.blocks){
        poll(stop);const int width=(block.right-block.left)*2,height=(block.bottom-block.top)*2;
        const auto size=std::size_t(width)*height;const Pixel zero{0,0,0,0};
        std::vector<Pixel> scratch(size,zero),companion(size,zero);
        for(auto plane=plan.planes.rbegin();plane!=plan.planes.rend();++plane){
            poll(stop);bool painted{};
            for(const auto& face:*plane)if(const auto* program=find_block(face.plan,block)){
                painted=true;paint_block(scratch,*program,face,ctx,stop);
            }
            if(!painted)continue;
            for(std::size_t i=0;i<size;++i){if(!(i%512))poll(stop);compose_sample(companion[i],scratch[i],ctx.depth);scratch[i]=zero;}
        }
        const auto c=plan.context.clip;
        for(int y=std::max(block.top,c.top);y<std::min(block.bottom,c.bottom);++y){
            poll(stop);for(int x=std::max(block.left,c.left);x<std::min(block.right,c.right);++x){
                const auto offset=std::size_t((y-block.top)*2)*width+(x-block.left)*2;
                const auto pixel=resolve_samples({companion[offset],companion[offset+1],companion[offset+width],companion[offset+width+1]},ctx.depth);
                if(pixel.a)result.set_pixel(x,y,pixel);
            }
        }
    }
    poll(stop);result.prune_empty();poll(stop);return result;
}
}
bool vector_face_region_contains(const VectorFaceGeometry& geometry,VectorFaceVertex raw_point){
    if(!point_ok(raw_point))throw std::invalid_argument("Vector face query outside supported raw domain");
    const auto prepared=prepare_geometry(geometry);return region_contains(prepared,raw_point);
}
void admit_vector_face_texture_queries(const VectorFaceGeometry& geometry,const VectorRenderContext& context,const VectorTextureMaterial& material){
    auto prepared=prepare_geometry(geometry,{},false);Budget budget;
    const auto plan=prepare_plan(prepared,context,budget,{},2);
    admit_texture_queries(plan,context,material);
}
namespace vector_render_detail {
StoredLineCacheCosts stored_face_planes_costs(std::span<const VectorFacePlaneView> planes,const VectorRenderContext& context,
                                             StoredLineCacheCosts previous,std::stop_token stop){
    return prepare_owner(planes,context,stop,previous).costs;
}
bool cached_region_contains(const VectorShapeFaceGeometry& source,VectorFaceVertex point,std::stop_token stop){
    if(!point_ok(point))throw std::invalid_argument("Vector face query outside supported raw domain");
    auto p=prepare_geometry(source.geometry,stop,false);
    if(source.model_bounds.size()!=p.model_bounds.size()||source.loop_bounds.size()!=p.loop_bounds.size()||
       source.region_bounds!=source.loop_bounds[p.geometry.outer_loop])throw std::invalid_argument("BFIL cached Region gate disagreement");
    const auto valid=[](VectorBounds b){return b.left<b.right&&b.top<b.bottom;};
    if(!std::ranges::all_of(source.model_bounds,valid)||!std::ranges::all_of(source.loop_bounds,valid))throw std::invalid_argument("BFIL unbounded or empty saved gates are unsupported");
    p.model_bounds=source.model_bounds;p.loop_bounds=source.loop_bounds;p.face_bounds=source.region_bounds;p.native_cached_gates=true;
    return region_contains(p,point);
}
struct FacePrograms::Storage {std::vector<FacePlan> faces;VectorRenderContext context;bool reference{};};
namespace {
FacePrograms prepare_face_programs(std::span<const FaceOccurrence> faces,const VectorRenderContext& ctx,FaceProgramBudget initial,std::stop_token stop,bool saved,int reference=0){
    poll(stop);(void)validate_context(ctx,int(ctx.oversample),reference);
    if(ctx.oversample!=1&&ctx.oversample!=2&&ctx.oversample!=4)throw std::invalid_argument("Shape Face requires native1x/2x/4x context");
    if(faces.size()>100000)throw std::length_error("Shape Face occurrence limit exceeded");
    Budget budget{initial.work,initial.points,initial.sampled,initial.storage};
    if(budget.work>max_work||budget.storage>max_storage)throw std::length_error("Shape shared work/storage limit exceeded");
    if(budget.points>max_points||budget.sampled>max_points)throw std::length_error("Shape shared point limit exceeded");
    std::array<std::size_t,6> counts{};std::size_t stops{};std::set<const VectorGradationMaterial*> unique_gradations;
    std::map<const VectorTextureMaterial*,std::shared_ptr<const VectorTextureSampler>> textures;
    std::map<const VectorShapeTextureRecipe*,std::shared_ptr<const VectorTextureSampler>> recipes;
    for(const auto& occurrence:faces){poll(stop);const auto* g=occurrence.geometry;
        if(!g||((g->material_flags&15)!=1&&(g->material_flags&15)!=2&&(g->material_flags&15)!=4))throw std::invalid_argument("Shape Face program requires supported material");
        if((g->material_flags&15)!=4&&occurrence.texture_recipe)throw std::invalid_argument("Texture recipe on non-texture Shape Face");
        if((g->material_flags&15)==4){if(bool(g->texture)==bool(occurrence.texture_recipe))throw std::invalid_argument("Shape texture requires exactly one source or recipe");
            if(g->texture&&textures.emplace(g->texture.get(),nullptr).second){const auto bytes=vector_texture_storage(*g->texture);if(!initial.texture_sources_precharged)budget.bytes(2*bytes+128);budget.charge(bytes);}
            if(occurrence.texture_recipe&&recipes.emplace(occurrence.texture_recipe.get(),nullptr).second){const auto& recipe=*occurrence.texture_recipe;const auto bytes=shape_texture_storage(recipe);
                if(!initial.texture_sources_precharged)budget.bytes(2*bytes+recipe.leaf->size()+sizeof(recipe)+128);budget.charge(2*bytes+recipe.leaf->size());}}
        if((g->material_flags&15)==2){if(!g->gradation)throw std::invalid_argument("NULL Shape gradation");const auto n=g->gradation->stops.size();
            if(n>100000-stops)throw std::length_error("Shape gradient stop occurrence limit exceeded");stops+=n;unique_gradations.insert(g->gradation.get());}
        const auto bill=[&](std::size_t field,std::size_t n){if(n>100000-counts[field])throw std::length_error("Shape aggregate Face graph limit exceeded");counts[field]+=n;};
        bill(0,g->vertices.size());bill(1,g->models.size());bill(2,g->loops.size());bill(3,g->holes.size());
        for(const auto& m:g->models)bill(4,m.spans.size());for(const auto& l:g->loops)bill(5,l.members.size());
    }
    budget.bytes(2*faces.size()*sizeof(FacePlan));budget.bytes(2*stops*sizeof(VectorGradationStop));
    budget.bytes(unique_gradations.size()*(sizeof(VectorGradationMaterial)+sizeof(vector_render_detail::GradationAdmission)+128));
    budget.charge(stops);
    std::map<const VectorGradationMaterial*,vector_render_detail::GradationAdmission> admission;
    for(const auto& occurrence:faces)if((occurrence.geometry->material_flags&15)==2){const auto& source=occurrence.geometry->gradation;if(!admission.contains(source.get()))admission.emplace(source.get(),source);}
    budget.bytes(counts[0]*sizeof(P)+counts[1]*sizeof(std::vector<Span>)+counts[2]*sizeof(VectorBounds)+counts[4]*sizeof(Span));
    auto result=std::make_shared<FacePrograms::Storage>();result->context=ctx;result->reference=reference;result->faces.reserve(faces.size());
    for(const auto& occurrence:faces){poll(stop);auto p=prepare_geometry(*occurrence.geometry,stop,!occurrence.texture_recipe);
        if(saved){
            if(occurrence.model_gates.size()!=p.model_bounds.size()||occurrence.loop_gates.size()!=p.loop_bounds.size())throw std::invalid_argument("Shape Face cached gate count disagreement");
            if(occurrence.region_gate!=occurrence.loop_gates[p.geometry.outer_loop])throw std::invalid_argument("Shape Region and embedded outer Loop gates disagree");
            p.model_bounds.assign(occurrence.model_gates.begin(),occurrence.model_gates.end());p.loop_bounds.assign(occurrence.loop_gates.begin(),occurrence.loop_gates.end());
            p.face_bounds=occurrence.region_gate;p.native_cached_gates=true;
        }
        auto plan=prepare_plan(p,ctx,budget,stop,int(ctx.oversample),false,saved?&occurrence.gate:nullptr,saved?&occurrence.region_gate:nullptr,reference);
        // Each occurrence has its own flush at2x. Charge the whole physical
        // block, even if the actual scanlines occupy only a narrow sliver.
        for(const auto& block:plan.blocks){const auto area=std::uint64_t(block.block.right-block.block.left)*(block.block.bottom-block.block.top);
            budget.charge(area*(ctx.oversample>1?ctx.oversample*ctx.oversample+1:1));}
        const auto source=(occurrence.geometry->material_flags&15)==2?occurrence.geometry->gradation:nullptr;
        if(source)for(const auto& block:plan.blocks)for(const auto& run:block.runs){
            const int first=int(floor_fixed(run.left)),last=int(floor_fixed(run.right))-((ctx.oversample==1||run.rectangle)?1:0);
            if(first<=last){const auto raw=[&](int q,double origin){return (ctx.clipboard_affine||reference)?clipboard_material_coordinate(q,ctx):int(std::floor((double(q)/ctx.oversample+origin)*ctx.base_scale));};
                const auto y=raw(run.y,ctx.origin.y);admission.at(source.get()).rectangle(raw(first,ctx.origin.x),y,raw(last,ctx.origin.x),y);}
            budget.charge(std::uint64_t(floor_fixed(run.right)-floor_fixed(run.left)+2)*(16+std::bit_width(source->stops.size())));
        }
        const auto texture=(occurrence.geometry->material_flags&15)==4?occurrence.geometry->texture:nullptr;
        if(texture||occurrence.texture_recipe){const auto metadata=texture?VectorTextureMaterial{}:shape_texture_metadata(*occurrence.texture_recipe);
            admit_texture_queries(plan,ctx,texture?*texture:metadata,int(ctx.oversample),reference);
            for(const auto& block:plan.blocks)for(const auto& run:block.runs)budget.charge(std::uint64_t(floor_fixed(run.right)-floor_fixed(run.left)+2)*64);}
        result->faces.push_back({sample_pigment(occurrence.geometry->color,ctx.depth),{},{},texture,std::move(plan),source,occurrence.texture_recipe});
    }
    poll(stop);return {std::move(result),{budget.storage,budget.work,budget.points,budget.sampled,initial.texture_sources_precharged}};
}
} // namespace
void complete_face_programs(FacePrograms& programs,std::stop_token stop){
    auto& result=programs.storage;
    std::map<const VectorTextureMaterial*,std::shared_ptr<const VectorTextureSampler>> textures;
    std::map<const VectorShapeTextureRecipe*,std::shared_ptr<const VectorTextureSampler>> recipes;
    for(const auto& face:result->faces){if(face.texture_source)textures.emplace(face.texture_source.get(),nullptr);if(face.texture_recipe_source)recipes.emplace(face.texture_recipe_source.get(),nullptr);}
    // Source+snapshot metadata, complete routed domains and all executor costs
    // are closed before constructing even the first immutable stop chain.
    for(auto& face:result->faces)if(face.gradation_source){poll(stop);face.gradient.emplace(face.gradation_source);face.gradation_source.reset();}
    // Query certificates and the shared Line/Face/output budgets precede all
    // image decompression. Each unique real image and immutable snapshot is
    // built once, including cross-plane borrows and repeated occurrences.
    for(auto& face:result->faces){poll(stop);
        if(face.texture_source){auto& snapshot=textures.at(face.texture_source.get());if(!snapshot)snapshot=std::make_shared<VectorTextureSampler>(*face.texture_source,texture_inverse(*face.texture_source,result->context),stop);face.texture=snapshot;face.texture_source.reset();}
        if(face.texture_recipe_source){const auto& recipe=*face.texture_recipe_source;auto& snapshot=recipes.at(&recipe);
            if(!snapshot){auto material=shape_texture_metadata(recipe);Reader encoded(shape_texture_image(recipe));material.image=decode_offscreen(encoded);poll(stop);
                if(vector_texture_storage(material)!=shape_texture_storage(recipe))throw std::invalid_argument("Decoded Shape texture disagrees with image admission");
                snapshot=std::make_shared<VectorTextureSampler>(material,texture_inverse(material,result->context),stop);}
            face.texture=snapshot;face.texture_recipe_source.reset();}
    }
    poll(stop);
}
FacePrograms prepare_shape_face_programs(std::span<const FaceOccurrence> faces,const VectorRenderContext& ctx,FaceProgramBudget initial,std::stop_token stop){auto programs=prepare_face_programs(faces,ctx,initial,stop,true);complete_face_programs(programs,stop);return programs;}
FacePrograms prepare_paint_face_programs(std::span<const FaceOccurrence> faces,const VectorRenderContext& ctx,FaceProgramBudget initial,std::stop_token stop){return prepare_face_programs(faces,ctx,initial,stop,false);}
FacePrograms prepare_paint_reference_face_programs(std::span<const FaceOccurrence> faces,const VectorRenderContext& ctx,FaceProgramBudget initial,std::stop_token stop){return prepare_face_programs(faces,ctx,initial,stop,false,true);}
FacePrograms prepare_paint_direct_face_programs(std::span<const FaceOccurrence> faces,const VectorRenderContext& ctx,FaceProgramBudget initial,std::stop_token stop){return prepare_face_programs(faces,ctx,initial,stop,false,2);}
std::vector<VectorBounds> face_program_blocks(const FacePrograms& programs){
    std::set<VectorBounds,decltype(&block_less)> blocks(&block_less);
    for(const auto& face:programs.storage->faces)for(const auto& block:face.plan.blocks)blocks.insert(block.block);
    return {blocks.begin(),blocks.end()};
}
bool face_program_has_block(const FacePrograms& programs,std::size_t index,VectorBounds block){return find_block(programs.storage->faces.at(index).plan,block)!=nullptr;}
bool paint_shape_face_program(const FacePrograms& programs,std::size_t index,VectorBounds block,std::span<Pixel> scratch,const VectorRenderContext& ctx,std::stop_token stop,bool overwrite1x,bool fractional4){
    poll(stop);const auto& face=programs.storage->faces.at(index);const auto* plan=find_block(face.plan,block);if(!plan)return false;
    const int mode=int(ctx.oversample),width=(block.right-block.left)*mode,height=(block.bottom-block.top)*mode;
    if(scratch.size()!=std::size_t(width)*height)throw std::invalid_argument("Shape Face scratch extent disagreement");
    const int left=block.left*mode,top=block.top*mode;
    const auto write=[&](int x,int y,bool partial,unsigned fraction,int query_x){
        // Material queries precede zero-coverage/physical-pointer rejection.
        // A same-floor tail writes the next sample but still queries floor(J).
        const auto offset=ctx.clipboard_affine?VectorPoint{}:ctx.origin;
        const auto raw_x=(ctx.clipboard_affine||programs.storage->reference)?clipboard_material_coordinate(query_x,ctx):int(std::floor((double(query_x)/mode+offset.x)*ctx.base_scale)),raw_y=(ctx.clipboard_affine||programs.storage->reference)?clipboard_material_coordinate(y+top,ctx):int(std::floor((double(y+top)/mode+offset.y)*ctx.base_scale));
        const auto sample=face.texture?face.texture->sample(raw_x,raw_y):face.gradient?face.gradient->sample(raw_x,raw_y):Pixel{};
        const auto source=(face.gradient||face.texture)?(ctx.depth==32?Pixel{std::uint16_t(sample.r>>8),std::uint16_t(sample.g>>8),std::uint16_t(sample.b>>8),std::uint16_t(sample.a>>8)}:sample):face.color;
        if(x<0||x>=width||y<0||y>=height)return;
        auto& dst=scratch[std::size_t(y)*width+x];
        Pixel native=ctx.depth==32?Pixel{std::uint16_t(dst.r/257),std::uint16_t(dst.g/257),std::uint16_t(dst.b/257),std::uint16_t(dst.a/257)}:dst;
        // 5DEB60/5DDCF0 retain normal ctx+630=1 at1x, propagated to
        // inner+468 by 649B70/64A090. The Region descriptor only selects
        // additive partial arithmetic; it does not disable full composition.
        if(!partial){if(mode==1&&!overwrite1x)compose_sample(native,source,ctx.depth);else native=source;}
        else if(face.gradient&&ctx.depth==32)partial_gradient32(native,sample,fraction);
        else partial_face(native,source,fraction,ctx.depth);
        dst=ctx.depth==32?Pixel::rgba8(native.r,native.g,native.b,native.a):native;
    };
    for(const auto& run:plan->runs){poll(stop);const int y=run.y-top,first=int(floor_fixed(run.left))-left,last=int(floor_fixed(run.right))-left;
        if(mode==1||(mode==4&&!fractional4)||run.rectangle){for(int x=first;x<last;++x)write(x,y,false,0,x+left);continue;}
        write(first,y,true,unsigned(std::int64_t(first+left+1)*256-run.left),first+left);
        int pointer=first+1;for(;pointer<last;++pointer)write(pointer,y,false,0,pointer+left);
        write(pointer,y,true,unsigned(run.right-std::int64_t(last+left)*256),last+left);
    }
    return true;
}
}
bool can_render_vector_face_geometry(const VectorFaceGeometry& geometry,const VectorRenderContext& context){
    try{if((geometry.material_flags&15)!=1)throw std::invalid_argument("Direct AA1 face API requires solid material");auto prepared=prepare_geometry(geometry);Budget budget;prepare_plan(prepared,context,budget,{});return true;}
    catch(const std::invalid_argument&){return false;}catch(const std::length_error&){return false;}
}
VectorFaceRaster render_vector_face_geometry(const VectorFaceGeometry& geometry,const VectorRenderContext& context,std::stop_token stop){
    poll(stop);if((geometry.material_flags&15)!=1)throw std::invalid_argument("Direct AA1 face API requires solid material");auto prepared=prepare_geometry(geometry,stop);Budget budget;const auto plan=prepare_plan(prepared,context,budget,stop);
    poll(stop);VectorFaceRaster result;result.pixels=Raster(context.width,context.height,context.depth,context.resolution);
    auto mask=std::make_shared<BinaryMask>(context.width,context.height);const auto color=pigment(geometry.color,context.depth);
    for(const auto& block:plan.blocks)for(const auto& run:block.runs){
        poll(stop);
        for(int x=int(floor_fixed(run.left));x<floor_fixed(run.right);++x){
            if(!(x%256))poll(stop);
            if(!mask->test(x,run.y)){++result.covered_pixels;mask->set(x,run.y);}
            result.pixels.set_pixel(x,run.y,color);
        }
    }
    poll(stop);result.dirty=mask->extent({0,0,context.width,context.height});
    const RegionSelection region{SelectionLayerState::Mask,result.dirty,{},std::move(mask),{context.width,context.height}};
    result.coverage=*project_selection(region,{},context.width,context.height);
    result.pixels.prune_empty();poll(stop);return result;
}
bool can_render_vector_face_planes(std::span<const VectorFacePlaneView> planes,const VectorRenderContext& context){
    try{(void)prepare_owner(planes,context,{});return true;}
    catch(const std::invalid_argument&){return false;}catch(const std::length_error&){return false;}
}
Raster render_vector_face_planes(std::span<const VectorFacePlaneView> planes,const VectorRenderContext& context,std::stop_token stop){
    const auto plan=prepare_owner(planes,context,stop);return render_owner(plan,context,stop);
}
Pixel vector_face_planes_pixel(std::span<const VectorFacePlaneView> planes,const VectorRenderContext& context,int x,int y,std::stop_token stop){
    auto ctx=context;
    const auto admission=validate_context(ctx,2);
    if(x<0||y<0||x>=ctx.width||y>=ctx.height)throw std::invalid_argument("Vector face owner pixel outside context");
    const auto c=admission.clip;
    ctx.clip=VectorBounds{std::max(x,c.left),std::max(y,c.top),std::min(x+1,c.right),std::min(y+1,c.bottom)};
    // A query outside an existing publication clip still validates the owner.
    if(ctx.clip->right<ctx.clip->left||ctx.clip->bottom<ctx.clip->top)ctx.clip=VectorBounds{0,0,0,0};
    return render_vector_face_planes(planes,ctx,stop).pixel(x,y);
}
}
