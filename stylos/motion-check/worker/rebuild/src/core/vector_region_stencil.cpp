#include "vector_region_probe.hpp"
#include "vector_scanline_router.hpp"
#include "retas/numeric.hpp"
#include <bit>
#include <cmath>
#include <limits>

namespace retas::vector_probe_detail {
namespace {
using P=VectorFaceVertex;using numeric_detail::binary64;
constexpr std::uint64_t max_storage=512ull*1024*1024,max_points=1024*1024;
void bytes(std::uint64_t& storage,std::uint64_t n){if(storage>max_storage||n>max_storage-storage)throw std::length_error("Vector Region stencil storage limit");storage+=n;}
int integer(std::int64_t n){if(n<INT32_MIN||n>INT32_MAX)throw std::invalid_argument("Vector Region stencil conversion outside signed32 domain");return int(n);}
int floor_div(std::int64_t n,int scale){return integer(n>=0?n/scale:-((-n+scale-1)/scale));}
int center(int a,int b){return std::bit_cast<std::int32_t>(std::uint32_t(a)+std::uint32_t(b))/2;}
bool empty(VectorBounds b){return b.left>=b.right||b.top>=b.bottom;}
struct RouteBudget {
    Budget budget;std::uint64_t& storage;std::uint64_t points{};
    void charge(std::uint64_t n){budget.charge(n);}
    void point(){budget.charge(8);if(++points>max_points)throw std::length_error("Vector Region stencil routed point limit");bytes(storage,128);}
};
void move(BinaryMask& mask,int x,int y,Budget budget){budget.charge();if(x>=0&&y>=0&&x<mask.width()&&y<mask.height())mask.materialize_block(x/256,y/256);}
void rect_fill(BinaryMask& mask,VectorBounds rect,Budget budget){
    if(empty(rect))return;budget.charge(std::uint64_t(rect.right-rect.left)*(rect.bottom-rect.top));
    // Native RectFill touches blocks directly and restores its cursor. It has
    // no polygon callback's right/bottom cursor tail allocations.
    for(int y=rect.top;y<rect.bottom;++y)for(int x=rect.left;x<rect.right;++x)mask.set(x,y);
}
void polygon_fill(BinaryMask& mask,const std::vector<P>& points,VectorBounds clip,std::uint64_t& storage,Budget budget){
    struct Edge{int low,high;std::int64_t x,slope;};std::vector<Edge> edges;bytes(storage,64*points.size());
    for(std::size_t i=0;i<points.size();++i){budget.charge();auto a=points[i],b=points[(i+1)%points.size()];if(a.y==b.y||a.y==INT32_MAX||b.y==INT32_MAX)continue;if(a.y>b.y)std::swap(a,b);
        const auto dx=(std::int64_t(b.x)-a.x)*256;if(dx<INT32_MIN||dx>INT32_MAX||std::abs(std::int64_t(a.x))>1048576||std::abs(std::int64_t(b.x))>1048576)throw std::length_error("Vector Region stencil fixed point limit");
        edges.push_back({a.y,b.y,std::int64_t(a.x)*256,dx/(std::int64_t(b.y)-a.y)});
    }
    if(edges.empty())return;
    const int low=std::ranges::min(edges,{},&Edge::low).low,high=std::ranges::max(edges,{},&Edge::high).high;int cursor_x=-1,cursor_y=low;
    move(mask,cursor_x,cursor_y,budget);std::vector<std::int64_t> active;active.reserve(edges.size());
    //5DEB60 includes maxY's empty row. CursorY advances only for eligible
    // rows; cursorX is retained from the final inclusive span's x1+1.
    for(int y=low;y<=high;++y){budget.charge(edges.size()+1);if(y>=clip.bottom)break;active.clear();
        for(const auto& e:edges)if(y>=e.low&&y<e.high)active.push_back(e.x+std::int64_t(y-e.low)*e.slope);
        budget.charge(active.size()*(std::bit_width(active.size())+1));std::sort(active.begin(),active.end());if(active.size()%2)throw std::invalid_argument("Unpaired vector Region stencil scanline");
        if(y<clip.top)continue;
        const auto floor_fixed=[](std::int64_t n){return n>=0?n/256:-((-n+255)/256);};
        for(std::size_t i=1;i<active.size();i+=2){const auto x0=std::max<std::int64_t>(floor_fixed(active[i-1]),clip.left),x1=std::min<std::int64_t>({floor_fixed(active[i])-1,clip.right,mask.width()-1});if(x0>x1)continue;
            cursor_x=integer(x0);move(mask,cursor_x,cursor_y,budget);budget.charge(std::uint64_t(x1-x0+1));
            for(int x=cursor_x;x<=x1;++x)mask.set(x,cursor_y);
            cursor_x=integer(x1+1);move(mask,cursor_x,cursor_y,budget);
        }
        ++cursor_y;move(mask,cursor_x,cursor_y,budget);
    }
}
VectorBounds envelope(const BinaryMask& mask,Budget budget){
    VectorBounds out{};for(int ty=0;ty*256<mask.height();++ty)for(int tx=0;tx*256<mask.width();++tx){budget.charge();if(!mask.block_present(tx,ty))continue;const VectorBounds b{tx*256,ty*256,std::min(mask.width(),(tx+1)*256),std::min(mask.height(),(ty+1)*256)};if(empty(out))out=b;else out={std::min(out.left,b.left),std::min(out.top,b.top),std::max(out.right,b.right),std::max(out.bottom,b.bottom)};}return out;
}
bool compare(const BinaryMask& a,const BinaryMask& b,bool all,Budget budget){
    const auto ea=envelope(a,budget),eb=envelope(b,budget);if(empty(ea)||empty(eb))return false;
    if(all){if(ea.left<eb.left||ea.top<eb.top||ea.right>eb.right||ea.bottom>eb.bottom)return false;}else if(!overlap(ea,eb))return false;
    for(int ty=ea.top/256;ty*256<ea.bottom;++ty)for(int tx=ea.left/256;tx*256<ea.right;++tx){budget.charge();if(!a.block_present(tx,ty)||(!all&&!b.block_present(tx,ty)))continue;
        const int right=std::min(a.width(),(tx+1)*256),bottom=std::min(a.height(),(ty+1)*256);budget.charge(std::uint64_t(right-tx*256)*(bottom-ty*256));
        for(int y=ty*256;y<bottom;++y)for(int x=tx*256;x<right;++x){if(!a.test(x,y))continue;const bool covered=b.test(x,y);if(all&&!covered)return false;if(!all&&covered)return true;}
    }
    return all;
}
template<class Map> auto live(const Map& m,int id){const auto it=m.find(id);if(it==m.end())throw std::invalid_argument("Dead vector Region stencil reference");return it->second;}
bool equal_owners(const VectorModelCurve& m){if(m.targets[0]<0||m.targets[1]<0)return m.targets[0]<0&&m.targets[1]<0;return (m.target_kinds[0]!=0)==(m.target_kinds[1]!=0)&&m.targets[0]==m.targets[1];}
}
BinaryMask PaintGraph::stencil(const VectorArchiveRegion& region,const Scope& scope,double raw_dpi,std::uint64_t& storage,Budget budget,double geometry_scale)const{
    budget.charge();
    if(scope.bitmap_dpi!=0){
        if(!scope.mask||!std::isfinite(scope.bitmap_dpi)||scope.bitmap_dpi<=0||!std::isfinite(scope.forward)||scope.forward<=0||scope.inverse!=binary64(1/scope.forward)||!std::isfinite(raw_dpi)||raw_dpi<=0)throw std::invalid_argument("Invalid actual Region stencil DPI");
        const double geometry=geometry_scale==0?binary64(raw_dpi/scope.bitmap_dpi):geometry_scale;
        const double reciprocal=binary64(1/geometry),sampling_dpi=binary64(scope.bitmap_dpi*geometry);
        if(!std::isfinite(geometry)||geometry<=0||!std::isfinite(reciprocal)||reciprocal<=0||!std::isfinite(sampling_dpi)||sampling_dpi<=0)throw std::invalid_argument("Region stencil geometry scale exceeds bounds");
        const double step=std::trunc(binary64(sampling_dpi*0x1.0204081020408p-5));if(step<1||step>INT32_MAX)throw std::invalid_argument("Invalid vector Region stencil sampling step");
        const auto& source=*scope.mask;const auto logical=((std::uint64_t(source.width())+31)/32)*source.height()*4+((std::uint64_t(source.width())+255)/256)*((std::uint64_t(source.height())+255)/256);bytes(storage,2*logical);
        const auto& b=region.outer.archived_bounds;
        const auto inverse_edge=[&](int edge,int axis,int limit){const auto delta=std::bit_cast<std::int32_t>(std::uint32_t(edge)-std::uint32_t(scope.offset[axis]));return std::clamp(vector_render_detail::clipboard_integer(std::floor(binary64(double(delta)*scope.inverse))),0,limit);};
        const VectorBounds draw{inverse_edge(b.left,0,source.width()),inverse_edge(b.top,1,source.height()),inverse_edge(b.right,0,source.width()),inverse_edge(b.bottom,1,source.height())};
        VectorRenderContext context{source.width(),source.height(),1,scope.bitmap_dpi,geometry,{},std::nullopt,1};
        if(scope.offset[0]||scope.offset[1]){
            const double tx=vector_render_detail::clipboard_integer(binary64(-double(scope.offset[0])*reciprocal)),ty=vector_render_detail::clipboard_integer(binary64(-double(scope.offset[1])*reciprocal));
            const double determinant=binary64(reciprocal*reciprocal),inverse=binary64(1/determinant),diagonal=binary64(reciprocal*inverse);
            if(!std::isfinite(determinant)||determinant<=0||!std::isfinite(inverse)||!std::isfinite(diagonal))throw std::invalid_argument("Region stencil matrix exceeds bounds");
            context.clipboard_affine=true;context.clipboard_matrix={reciprocal,0,tx,0,reciprocal,ty};
            context.clipboard_inverse={diagonal,-0.,binary64(binary64(-tx*reciprocal)*inverse),-0.,diagonal,binary64(binary64(-ty*reciprocal)*inverse)};
        }
        BinaryMask mask(source.width(),source.height());if(!empty(draw))stencil_into(region,draw,16,{0,0},sampling_dpi,mask,storage,budget,&context);return mask;
    }
    const int scale=scope.forward==4?4:16;const double geometry=geometry_scale==0?scope.forward:geometry_scale;
    if(!scope.mask||(scope.forward!=4&&scope.forward!=16)||scope.inverse!=1.0/scale||(geometry!=4&&geometry!=16)
        ||(geometry_scale==0&&(scope.offset[0]%scale||scope.offset[1]%scale))||!std::isfinite(raw_dpi)||raw_dpi<=0)throw std::invalid_argument("Vector Region stencil requires native raw4/raw16 selection and geometry scales");
    const auto& source=*scope.mask;const auto logical=((std::uint64_t(source.width())+31)/32)*source.height()*4+((std::uint64_t(source.width())+255)/256)*((std::uint64_t(source.height())+255)/256);bytes(storage,2*logical);
    const auto step=std::trunc(binary64(raw_dpi*0x1.0204081020408p-5));if(step<1||step>INT32_MAX)throw std::invalid_argument("Invalid vector Region stencil sampling step");
    BinaryMask mask(source.width(),source.height());const auto b=region.outer.archived_bounds;
    //53A570 performs a DWORD SUB before loading the result into x87. Saved
    // bbox caches can overflow that subtraction even with bounded geometry.
    const auto inverse_edge=[&](int edge,int axis,int limit){const auto delta=std::bit_cast<std::int32_t>(std::uint32_t(edge)-std::uint32_t(scope.offset[std::size_t(axis)]));return std::clamp(floor_div(delta,scale),0,limit);};
    const VectorBounds draw{inverse_edge(b.left,0,source.width()),inverse_edge(b.top,1,source.height()),inverse_edge(b.right,0,source.width()),inverse_edge(b.bottom,1,source.height())};if(empty(draw))return mask;
    //5C810A keeps the selection bbox transform but truncates the geometry
    //matrix's pixel translation using its independent topology/carrier ratio.
    const int geometry_units=int(geometry);std::array<int,2> offset;
    for(unsigned i=0;i<2;++i)offset[i]=integer(std::int64_t(scope.offset[i]/geometry_units)*geometry_units);
    stencil_into(region,draw,geometry_units,offset,raw_dpi,mask,storage,budget);return mask;
}
void PaintGraph::stencil_into(const VectorArchiveRegion& region,VectorBounds draw,int scale,std::array<int,2> raw_offset,double raw_dpi,BinaryMask& mask,std::uint64_t& storage,Budget budget,const VectorRenderContext* transformed)const{
    budget.charge();if((!transformed&&((scale!=4&&scale!=16)||raw_offset[0]%scale||raw_offset[1]%scale))||!std::isfinite(raw_dpi)||raw_dpi<=0
        ||draw.left<0||draw.top<0||draw.right>mask.width()||draw.bottom>mask.height()||draw.left>draw.right||draw.top>draw.bottom)throw std::invalid_argument("Invalid vector Region stencil target");
    const auto step=std::trunc(binary64(raw_dpi*0x1.0204081020408p-5));if(step<1||step>INT32_MAX)throw std::invalid_argument("Invalid vector Region stencil sampling step");
    if(empty(draw))return;const auto b=region.outer.archived_bounds;
    VectorRenderContext ctx;ctx.width=mask.width();ctx.height=mask.height();ctx.base_scale=scale;ctx.oversample=1;ctx.origin={double(raw_offset[0]/scale),double(raw_offset[1]/scale)};if(transformed)ctx=*transformed;
    RouteBudget route_budget{budget,storage};
    for(int ty=draw.top/256;ty*256<draw.bottom;++ty)for(int tx=draw.left/256;tx*256<draw.right;++tx){budget.charge();const VectorBounds block{std::max(tx*256,draw.left),std::max(ty*256,draw.top),std::min((tx+1)*256,draw.right),std::min((ty+1)*256,draw.bottom)};
        const auto raw_edge=[&](int n,int axis){return integer(std::int64_t(n)*scale+raw_offset[std::size_t(axis)]);};VectorBounds raw;
        if(!transformed)raw={raw_edge(block.left,0),raw_edge(block.top,1),raw_edge(block.right,0),raw_edge(block.bottom,1)};
        else if(ctx.clipboard_affine)raw=vector_render_detail::clipboard_raw_block(block,ctx);
        else{
            const auto forward=[&](int n){return vector_render_detail::clipboard_integer(std::floor(binary64(double(n)*ctx.base_scale)));};
            raw={forward(block.left),forward(block.top),forward(block.right),forward(block.bottom)};
            const double inverse=binary64(1/ctx.base_scale);
            if(std::floor(binary64(raw.right*inverse))<block.right||std::floor(binary64(raw.bottom*inverse))<block.bottom){raw.right=integer(std::int64_t(raw.right)+1);raw.bottom=integer(std::int64_t(raw.bottom)+1);}
            for(int edge:{raw.left,raw.top,raw.right,raw.bottom})if(std::abs(std::int64_t(edge))>=1073741824)throw std::invalid_argument("Region stencil block exceeds routing domain");
        }
        vector_scan_detail::Router<RouteBudget> router{raw,block,ctx,route_budget,{},{},{},{},false,false,false,transformed!=nullptr};
        const auto emit=[&](const VectorArchiveLoop& loop){if(!overlap(loop.archived_bounds,raw))return;for(auto member:loop.members){budget.charge();const auto& model=*live(lines.models,member.model);if(equal_owners(model)){router.flush();continue;}const auto& s=model.scalars;if(!overlap({s[0],s[1],s[2],s[3]},raw)){router.last_valid=false;continue;}
                const auto span=[&](int id,bool forward){budget.charge();const auto curve=lines.curve(model,*live(lines.subcurves,id),budget);const auto start=node_position(curve,0,budget),end=node_position(curve,1,budget);std::size_t n=1;
                    if(!curve.linear){const VectorBounds hull{std::min({curve.start.x,curve.control.x,curve.end.x}),std::min({curve.start.y,curve.control.y,curve.end.y}),integer(std::int64_t(std::max({curve.start.x,curve.control.x,curve.end.x}))+1),integer(std::int64_t(std::max({curve.start.y,curve.control.y,curve.end.y}))+1)};
                        if(!overlap(hull,raw)){router.last_valid=false;return;}const auto count=std::max(2.0,std::trunc(binary64(four_chord_length(curve,budget)/step)));if(!std::isfinite(count)||count>max_points)throw std::length_error("Vector Region stencil sample limit");n=std::size_t(count);}
                    const float h=float(binary64(1.0/double(n)));const auto send=[&](std::size_t i){budget.charge();router.route(i==0?start:i==n?end:integer_position(curve,float(binary64(double(i)*h)),budget));};if(forward)for(std::size_t i=0;i<=n;++i)send(i);else for(std::size_t i=n+1;i-->0;)send(i);
                };if(member.selector)for(auto id:model.subcurves)span(id,true);else for(auto i=model.subcurves.rbegin();i!=model.subcurves.rend();++i)span(*i,false);
            }router.flush();};
        emit(region.outer);for(auto id:region.holes)emit(*live(loops,id));
        const auto contains_point=[&](P p){return contains(region,p,budget);};
        if(router.points.empty()){if(b.left<=raw.left&&b.top<=raw.top&&b.right>=raw.right&&b.bottom>=raw.bottom&&contains_point({center(raw.left,raw.right),center(raw.top,raw.bottom)}))rect_fill(mask,block,budget);}
        else{router.finish(contains_point);polygon_fill(mask,router.points,block,storage,budget);}
    }
    budget.charge();
}
bool PaintGraph::matches(const VectorArchiveRegion& region,const Scope& scope,bool all,double raw_dpi,std::uint64_t& storage,Budget budget,double geometry_scale)const{
    budget.charge();if(!scope.mask)return rectangle_matches(region,scope,all,budget);if(!scope.configured)return true;const auto b=region.outer.archived_bounds;if(empty(b))return false;
    if(all){if(b.left<scope.bounds.left||b.top<scope.bounds.top||b.right>scope.bounds.right||b.bottom>scope.bounds.bottom)return false;}else if(!overlap(b,scope.bounds))return false;
    const auto selected=stencil(region,scope,raw_dpi,storage,budget,geometry_scale);return compare(selected,*scope.mask,all,budget);
}
}
