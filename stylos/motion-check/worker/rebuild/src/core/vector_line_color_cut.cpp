#include "vector_line_color_cut.hpp"
#include "vector_graph_edit.hpp"
#include "vector_edit_profile.hpp"
#include "vector_document_geometry.hpp"
#include "vector_material_edit.hpp"
#include "retas/vector_color_metadata.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>

namespace retas::vector_color_detail {
namespace {
using namespace vector_probe_detail;
using numeric_detail::binary64;
using vector_material_detail::charge;
using vector_material_detail::VectorSlots;
using P=VectorFaceVertex;
using namespace vector_graph_detail;
using Event=LineSplitEvent;
int edge_distance(int point,int edge){
    //5C8660 subtracts/negates DWORDs before its signed minimum comparison.
    // ABS(INTMIN) stays INTMIN and can select the first boundary.
    const auto delta=std::bit_cast<std::int32_t>(std::uint32_t(point)-std::uint32_t(edge));
    return delta<0?std::bit_cast<std::int32_t>(0u-std::uint32_t(delta)):delta;
}
std::array<VectorPoint,2> hints(const Scope& s,P now,P previous,bool inside,Budget budget){
    if(!s.mask){const std::array<int,4> ds{edge_distance(now.x,s.bounds.left),edge_distance(now.x,s.bounds.right),edge_distance(now.y,s.bounds.top),edge_distance(now.y,s.bounds.bottom)};unsigned edge{};for(unsigned i=1;i<4;++i)if(ds[i]<ds[edge])edge=i;
        const auto x=double(now.x),y=double(now.y);switch(edge){case 0:return {{{x,y-10},{x,y+10}}};case 1:return {{{x,y+10},{x,y-10}}};case 2:return {{{x+10,y},{x-10,y}}};default:return {{{x-10,y},{x+10,y}}};}}
    const auto inverse=[&](P p){return P{integer(std::floor(binary64(double(std::int64_t(p.x)-s.offset[0])*s.inverse))),integer(std::floor(binary64(double(std::int64_t(p.y)-s.offset[1])*s.inverse)))};};
    const auto a=inverse(inside?now:previous),z=inverse(inside?previous:now);int d;
    if(a.x>z.x)d=a.y<z.y?1:a.y>z.y?7:0;else if(a.x<z.x)d=a.y<z.y?3:a.y>z.y?5:4;else d=a.y>z.y?6:2;
    constexpr std::array<P,8> delta{{{1,0},{1,-1},{0,-1},{-1,-1},{-1,0},{-1,1},{0,1},{1,1}}};std::array<VectorPoint,2> out;
    for(int lane=0;lane<2;++lane){auto p=a;int direction=lane?2*(d/2):2*((d+1)/2);P candidate{};
        for(int step=0;step<10;++step){direction=(direction+(lane?6:2))%8;for(int attempt=0;attempt<(lane?8:4);++attempt){budget.charge();const auto v=delta[std::size_t(direction)];candidate={integer(double(p.x)+v.x),integer(double(p.y)+v.y)};
                const auto& r=s.mask_bounds;if(candidate.x>=r.x&&candidate.y>=r.y&&std::int64_t(candidate.x)<std::int64_t(r.x)+r.width&&std::int64_t(candidate.y)<std::int64_t(r.y)+r.height&&s.mask->test(candidate.x,candidate.y))break;direction=(direction+(lane?2:6))%8;}
            p=candidate;}
        out[std::size_t(lane)]={double(integer(binary64(std::floor(binary64(double(candidate.x)*s.forward))+s.offset[0]))),double(integer(binary64(std::floor(binary64(double(candidate.y)*s.forward))+s.offset[1])))};
    }return out;
}
std::vector<Event> events(const VectorLineArchive& a,int model_id,bool side,const Scope& scope,Budget budget,bool collect_hints=true){
    std::vector<Event> out;if(!scope.configured)return out;Graph g(a,budget);const auto& m=*g.models.at(model_id);const auto& s=m.scalars;if(!overlap({s[0],s[1],s[2],s[3]},scope.bounds)&&!side)return out;
    for(auto id:m.subcurves){const auto c=g.curve(m,*g.subcurves.at(id),budget);VectorBounds hull{std::min(c.start.x,c.end.x),std::min(c.start.y,c.end.y),std::max(c.start.x,c.end.x)+1,std::max(c.start.y,c.end.y)+1};if(!c.linear){hull={std::min(hull.left,c.control.x),std::min(hull.top,c.control.y),std::max(hull.right,c.control.x+1),std::max(hull.bottom,c.control.y+1)};}
        if(!overlap(hull,scope.bounds)&&!side)continue;
        const auto count=!scope.mask&&hull.left>scope.bounds.left&&hull.top>scope.bounds.top&&hull.right<scope.bounds.right&&hull.bottom<scope.bounds.bottom?1.0:std::trunc(binary64(binary64(four_chord_length(c,budget)*scope.inverse)*2));
        if(!std::isfinite(count)||count<0||count>1000000)throw std::length_error("LCOL event sample limit");if(count==0)continue;const float h=f32(binary64(1/count));float t{};P previous{};
        for(std::size_t j=0;j<std::size_t(count);){const auto p=integer_position(c,t,budget);const bool hit=scope.point(p,budget);if(hit!=side){charge(budget.work,128,128ull*1024*1024,"LCOL event work limit");if(out.size()>=8192)throw std::length_error("LCOL event reference limit");out.push_back({!hit,id,t,collect_hints?hints(scope,p,previous,hit,budget):std::array<VectorPoint,2>{}});side=hit;j+=4;t=f32(binary64(double(t)+binary64(double(h)*4)));}else{++j;t=f32(binary64(double(t)+h));}previous=p;}
    }return out;
}
}
MaterializedLineSpans materialize_line_split_events(Editor& edit,int mc,const std::vector<LineSplitEvent>& input,bool side){MaterializedLineSpans out;int previous_id=-1;float previous_t{};bool terminal=false;int residual=mc;bool state=side;
        for(const auto& e:input){state=e.code==0;int sc=e.sub;float q=e.t;if(sc==previous_id){if(terminal)continue;sc=edit.model(residual).subcurves.front();q=f32(binary64(binary64(double(e.t)-previous_t)/binary64(1-double(previous_t))));}else terminal=false;
            const auto last_vertex=edit.sub(sc).vertex_b;const auto s=edit.split(residual,sc,q);if(s.cut){terminal=s.vertex==last_vertex;auto h=e.hints;if(!state)std::swap(h[0],h[1]);vector_edit_detail::cut_endpoint(edit.a,edit.layer,s.left,false,h,edit.wire,edit.nodes,edit.refs,edit.storage,edit.budget.work,edit.budget.stop);std::swap(h[0],h[1]);vector_edit_detail::cut_endpoint(edit.a,edit.layer,s.right,true,h,edit.wire,edit.nodes,edit.refs,edit.storage,edit.budget.work,edit.budget.stop);edit.slots[4].reset();out.parts.push_back({s.left,!state});residual=s.right;
                if(edit.model(s.left).subcurves.back()==sc)terminal=true;
            }else{const auto& m=edit.model(residual);if(sc==m.subcurves.front()&&s.vertex==edit.sub(sc).vertex_a)out.start=true;if(sc==m.subcurves.back()&&s.vertex==last_vertex){terminal=true;out.end=true;state=!state;}}
            previous_id=e.sub;previous_t=e.t;
        }out.parts.push_back({residual,state});return out;
}
namespace {
struct Engine:vector_graph_detail::Editor {
    const Scope& scope;CutResult result;
    Engine(VectorLineArchive& graph,VectorPaintArchive* p,Layer& l,std::size_t plane,const Scope& s,std::uint64_t& w,std::uint64_t& n,std::uint64_t& r,std::uint64_t& memory,std::uint64_t& work,std::stop_token stop):Editor(graph,p,l,plane,w,n,r,memory,work,stop),scope(s){}
    int detach(int source,int mc){const auto old=line(source);const auto it=std::ranges::find(old.members,mc,&VectorLineMember::model);if(it==old.members.end())throw std::invalid_argument("LCOL selected MC no longer belongs to source LC");const auto pos=std::size_t(it-old.members.begin());int suffix=-1;
        if(pos&&pos+1<old.members.size()){suffix=new_line(old,source);line(suffix).members.assign(std::next(it),old.members.end());for(auto v:line(suffix).members)model(v.model).line=suffix;line(source).members.resize(pos);}else line(source).members.erase(line(source).members.begin()+std::ptrdiff_t(pos));model(mc).line=-1;
        if(line(source).members.empty()){remove(owner.lines,source);erase(a.lines,5,source);}else cache(source,false);if(suffix>=0)cache(suffix,false);return suffix;
    }
    void run(int id){Graph graph(a,budget);const auto original=line(id);if(!graph.line_matches(original,scope,false,budget))return;unite(result.dirty,original.archived_bounds);if(original.members.empty())return;
        const auto first=original.members.front();const auto& m=model(first.model);if(m.subcurves.empty())throw std::invalid_argument("LCOL source LC requires nonempty MC");const auto& s=sub(first.selector?m.subcurves.front():m.subcurves.back());const auto& v=vertex(first.selector?s.vertex_a:s.vertex_b);bool side=scope.point({v.x,v.y},budget),pending{};int current=-1,source=id;
        const auto selected=[&](){const int lc=new_line(original);result.selected.push_back(lc);return lc;};if(side)current=selected();
        const auto close=[&](){if(current<0)throw std::invalid_argument("LCOL invalid selected-run state");cache(current,true);unite(result.dirty,line(current).archived_bounds);current=-1;};
        for(auto member:original.members){const auto collected=events(a,member.model,side,scope,budget);auto info=materialize_line_split_events(*this,member.model,collected,side);if(!member.selector)std::reverse(info.parts.begin(),info.parts.end());for(auto [mc,inside]:info.parts){
                if(inside){if(!side||pending){side=true;pending=false;current=selected();}const int suffix=detach(source,mc);if(suffix>=0)source=suffix;if(current<0)throw std::invalid_argument("LCOL missing selected LC");line(current).members.push_back({mc,member.selector});model(mc).line=current;reference();}
                else if(side||pending){side=false;pending=false;close();}
            }if((member.selector&&info.end)||(!member.selector&&info.start)){side=!side;pending=true;}
        }if(current>=0)close();
    }
};
}
std::vector<LineSplitEvent> collect_line_split_events(const VectorLineArchive& a,int model,bool side,const Scope& scope,Budget budget,bool collect_hints){return events(a,model,side,scope,budget,collect_hints);}
CutResult cut_lines(VectorLineArchive& a,VectorPaintArchive* paint,Layer& layer,std::size_t plane,const std::vector<int>& lines,const Scope& scope,std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& refs,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
    Engine e(a,paint,layer,plane,scope,wire,nodes,refs,storage,work,stop);for(auto id:lines)e.run(id);e.owner.publish(layer.planes.at(plane),paint);e.budget.charge();e.result.topology_changed=e.topology_changed;return std::move(e.result);
}
}
