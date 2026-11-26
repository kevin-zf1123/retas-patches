#include "vector_region_probe.hpp"
#include <algorithm>
#include <bit>
#include <stdexcept>

namespace retas::vector_probe_detail {
namespace {
template<class Map> auto live(const Map& m,int id){const auto found=m.find(id);if(found==m.end())throw std::invalid_argument("Dead vector Region probe reference");return found->second;}
bool axis_gate(VectorBounds b,int q,bool horizontal){return horizontal?q>=b.top&&q<b.bottom:q>=b.left&&q<b.right;}
bool point_gate(VectorBounds b,VectorFaceVertex p){return p.x>=b.left&&p.x<b.right&&p.y>=b.top&&p.y<b.bottom;}
int center(int a,int b){return std::bit_cast<std::int32_t>(std::uint32_t(a)+std::uint32_t(b))/2;}
}
PaintGraph::PaintGraph(const VectorPaintArchive& a,Budget budget):lines(a.base,budget){
    const auto count=a.loops.size()+a.regions.size();budget.charge(count*(1+std::bit_width(count))*8);
    const auto add=[](auto& out,const auto& pool){if(pool.size()>100000)throw std::length_error("Vector Region probe record limit");for(const auto& r:pool)if(r.index<0||!out.emplace(r.index,&r.value).second)throw std::invalid_argument("Ambiguous vector Region probe identity");};
    add(loops,a.loops);add(regions,a.regions);
}
std::vector<int> PaintGraph::axis_values(const VectorArchiveLoop& loop,int q,bool horizontal,Budget budget) const {
    budget.charge();std::vector<int> values;if(!axis_gate(loop.archived_bounds,q,horizontal))return values;
    for(auto member:loop.members){budget.charge();const auto& model=*live(lines.models,member.model);const auto& b=model.scalars;
        if(!axis_gate({b[0],b[1],b[2],b[3]},q,horizontal))continue;
        for(auto id:model.subcurves){const auto c=lines.curve(model,*live(lines.subcurves,id),budget);
            int low=horizontal?std::min(c.start.y,c.end.y):std::min(c.start.x,c.end.x),high=horizontal?std::max(c.start.y,c.end.y):std::max(c.start.x,c.end.x);
            if(!c.linear){low=std::min(low,horizontal?c.control.y:c.control.x);high=std::max(high,horizontal?c.control.y:c.control.x);}
            if(q<low||std::int64_t(q)>=std::int64_t(high)+1)continue;
            for(auto t:axis_roots(c,horizontal,q,true,budget)){const auto point=integer_position(c,t,budget);values.push_back(horizontal?point.x:point.y);}
        }
    }
    return values; // Original occurrence/root order, including duplicates.
}
std::vector<int> PaintGraph::axis_values(const VectorArchiveRegion& region,int q,bool horizontal,Budget budget) const {
    budget.charge();std::vector<int> out;const auto b=region.outer.archived_bounds;
    if(b.left<b.right&&b.top<b.bottom){out=axis_values(region.outer,q,horizontal,budget);if(out.empty())return out;}
    for(auto id:region.holes){budget.charge();auto part=axis_values(*live(loops,id),q,horizontal,budget);out.insert(out.end(),part.begin(),part.end());}return out;
}
int PaintGraph::classify(const VectorArchiveLoop& loop,VectorFaceVertex point,Budget budget) const {
    budget.charge();if(!point_gate(loop.archived_bounds,point))return 0;
    const auto x=axis_values(loop,point.y,true,budget);if(x.empty())return 0;
    budget.charge(x.size());const bool odd=x.size()%2;const auto equal=std::ranges::count(x,point.x);
    if(!odd&&!equal)return std::ranges::count_if(x,[&](int n){return n<point.x;})%2?1:0;
    const auto y=axis_values(loop,point.x,false,budget);if(y.empty())return 0;budget.charge(y.size());
    if(odd||std::ranges::find(y,point.y)!=y.end())return 2;
    return std::ranges::count_if(y,[&](int n){return n<point.y;})%2?1:0;
}
bool PaintGraph::contains(const VectorArchiveRegion& region,VectorFaceVertex point,Budget budget) const {
    budget.charge();const auto b=region.outer.archived_bounds;
    //5A1CF0: an empty saved outer bbox represents the exterior domain.
    if(b.left<b.right&&b.top<b.bottom&&!classify(region.outer,point,budget))return false;
    for(auto id:region.holes){budget.charge();if(classify(*live(loops,id),point,budget))return false;}return true;
}
bool PaintGraph::rectangle_matches(const VectorArchiveRegion& region,const Scope& scope,bool all,Budget budget) const {
    budget.charge();if(!scope.configured)return true;if(scope.mask)throw std::invalid_argument("Masked vector Region requires native one-bit stencil query");
    const auto b=region.outer.archived_bounds;if(b.left>=b.right||b.top>=b.bottom)return false;
    if(all)return b.left>=scope.bounds.left&&b.top>=scope.bounds.top&&b.right<=scope.bounds.right&&b.bottom<=scope.bounds.bottom;
    if(!overlap(b,scope.bounds))return false;
    //56D7E0 follows the first member's original selector orientation.
    if(!region.outer.members.empty()){const auto member=region.outer.members.front();const auto& model=*live(lines.models,member.model);
        if(model.subcurves.empty())throw std::invalid_argument("Vector Region probe has no directed head span");
        const auto& sub=*live(lines.subcurves,member.selector!=0?model.subcurves.front():model.subcurves.back());const auto p=live(lines.vertices,member.selector!=0?sub.vertex_a:sub.vertex_b);
        if(scope.point({p->x,p->y},budget))return true;}
    const auto r=scope.bounds;
    if(contains(region,{center(r.left,r.right),center(r.top,r.bottom)},budget)||contains(region,{r.left,r.top},budget))return true;
    for(bool horizontal:{true,false})for(int q:horizontal?std::array{r.top,r.bottom}:std::array{r.left,r.right}){
        const auto values=axis_values(region,q,horizontal,budget);budget.charge(values.size());
        const int low=horizontal?r.left:r.top,high=horizontal?r.right:r.bottom;
        if(std::ranges::any_of(values,[&](int value){return value>=low&&value<high;}))return true;
    }
    return false;
}
}
