#include "vector_graph_edit.hpp"
#include "vector_curve_math.hpp"

namespace retas::vector_graph_detail {
namespace {
VectorPoint point(const VectorVertex& p){return {double(p.x),double(p.y)};}
int sar(int a,int b){const auto n=std::bit_cast<std::int32_t>(std::uint32_t(a)+std::uint32_t(b));return n>=0?n/2:-int((-(std::int64_t)n+1)/2);}
P mid(P a,P b){return {sar(a.x,b.x),sar(a.y,b.y)};}
P endpoint(Editor& e,int id){const auto& v=e.vertex(id);return {v.x,v.y};}
std::array<P,3> split_controls(const Curve& c,float q){const auto u=binary64(1-double(q));const auto mix=[&](double a,double z){return binary64(binary64(a*u)+binary64(z*q));};
    const VectorPoint l{mix(c.start.x,c.control.x),mix(c.start.y,c.control.y)},r{mix(c.control.x,c.end.x),mix(c.control.y,c.end.y)};
    return {{{integer(std::floor(l.x)),integer(std::floor(l.y))},{integer(std::floor(mix(l.x,r.x))),integer(std::floor(mix(l.y,r.y)))},{integer(std::floor(r.x)),integer(std::floor(r.y))}}};
}
void release_sub(Editor& e,int id){const auto s=e.sub(id);for(int v:{s.vertex_a,s.vertex_b}){Editor::remove(e.vertex(v).subcurves,id);if(e.vertex(v).subcurves.empty()){Editor::remove(e.owner.vertices,v);e.erase(e.a.vertices,0,v);}}for(auto n:s.scales)e.erase(e.a.scales,1,n);Editor::remove(e.model(s.model).subcurves,id);e.erase(e.a.subcurves,2,id);}
void release_cp(Editor& e,int mc,int lane){const int id=e.model(mc).crosspoints[lane];if(id<0)return;std::size_t users{};for(const auto& m:e.a.models){e.budget.charge();users+=std::ranges::count(m.value.crosspoints,id);}if(users!=1)throw std::invalid_argument("Vector graph cannot release a shared CP");e.erase(e.a.crosspoints,4,id);e.model(mc).crosspoints[lane]=-1;}
void release_model(Editor& e,int mc){const auto m=e.model(mc);if(m.targets[0]>=0||m.targets[1]>=0)throw std::invalid_argument("Vector graph deletion requires native live Loop repair");for(int lane=0;lane<2;++lane)release_cp(e,mc,lane);for(auto id:m.subcurves){const auto s=e.sub(id);release_sub(e,id);for(int v:{s.vertex_a,s.vertex_b})if(std::ranges::find(e.a.vertices,v,&VectorPoolRecord<VectorVertex>::index)!=e.a.vertices.end()&&e.vertex(v).subcurves.empty()){Editor::remove(e.owner.vertices,v);e.erase(e.a.vertices,0,v);}}
    Editor::remove(e.owner.models,mc);e.erase(e.a.models,3,mc);
}
void remove_degenerate(Editor& e,int lc){const auto members=e.line(lc).members;for(auto m:members){if(std::ranges::find(e.a.models,m.model,&VectorPoolRecord<VectorModelCurve>::index)==e.a.models.end())continue;const auto subs=e.model(m.model).subcurves;for(auto id:subs){const auto s=e.sub(id);if(sq(endpoint(e,s.vertex_a),endpoint(e,s.vertex_b))>16)continue;
        if(e.model(m.model).subcurves.size()==1){if(e.line(lc).members.size()==1){release_model(e,m.model);Editor::remove(e.owner.lines,lc);e.erase(e.a.lines,5,lc);return;}if(s.vertex_a!=s.vertex_b)weld(e,s.vertex_a,s.vertex_b);auto& list=e.line(lc).members;const auto it=std::ranges::find(list,m.model,&VectorLineMember::model);list.erase(it);release_model(e,m.model);e.cache(lc,true);break;}
        if(s.vertex_a!=s.vertex_b)weld(e,s.vertex_a,s.vertex_b);release_sub(e,id);e.cache(lc,true);
    }}
}
void normalize_sub(Editor& e,int id,unsigned depth){e.budget.charge(128);if(depth>64)throw std::length_error("Vector graph normalization recursion limit");const auto s=e.sub(id);const auto a=endpoint(e,s.vertex_a),b=endpoint(e,s.vertex_b);const VectorPoint c{double(s.control_x),double(s.control_y)};
    const float heading=vector_curve_detail::heading({double(a.x),double(a.y)},c);double delta=std::abs(binary64(double(vector_curve_detail::heading(c,{double(b.x),double(b.y)}))-heading));if(delta>=180)delta=binary64(360-delta);if(delta<30)return;
    const auto pieces=split_controls({a,{s.control_x,s.control_y},b,false},.5f);if(sq(a,pieces[1])<64||sq(pieces[1],b)<64){const auto p=mid(a,b);e.sub(id).control_x=p.x;e.sub(id).control_y=p.y;e.topology_changed=true;return;}
    const int old_next_pos=int(std::ranges::find(e.model(s.model).subcurves,id)-e.model(s.model).subcurves.begin());
    split_subcurve(e,id,.5f,pieces[1],true);const auto children=e.model(s.model).subcurves;normalize_sub(e,children.at(old_next_pos),depth+1);normalize_sub(e,children.at(old_next_pos+1),depth+1);
}
}
void clear_endpoint(Editor& e,int mc,bool first){release_cp(e,mc,first?0:1);e.model(mc).flags&=~(first?0x10u:0x20u);}
void erase_line(Editor& e,int lc){const auto members=e.line(lc).members;for(auto m:members)release_model(e,m.model);Editor::remove(e.owner.lines,lc);e.erase(e.a.lines,5,lc);}
void weld(Editor& e,int keep,int drop){if(keep==drop)return;const auto old=point(e.vertex(drop)),replacement=point(e.vertex(keep));const auto adjacency=e.vertex(drop).subcurves;
    for(auto id:adjacency){e.budget.charge(128);const auto s=e.sub(id);const bool first=s.vertex_a==drop;if(!first&&s.vertex_b!=drop)throw std::invalid_argument("Vector weld has nonincident adjacency");const auto other=point(e.vertex(first?s.vertex_b:s.vertex_a));const auto c=vector_curve_detail::reattach(other,old,replacement,{double(s.control_x),double(s.control_y)});auto& changed=e.sub(id);changed.control_x=integer(c.x);changed.control_y=integer(c.y);if(first)changed.vertex_a=keep;else changed.vertex_b=keep;e.vertex(keep).subcurves.push_back(id);e.reference();vector_edit_detail::incremental_subcurve(e.a,e.layer,id,e.wire,e.nodes,e.refs,e.storage,e.budget.work,e.budget.stop);}
    Editor::remove(e.owner.vertices,drop);e.erase(e.a.vertices,0,drop);
}
void split_subcurve(Editor& e,int id,float q,P p,bool stored){const auto s=e.sub(id);const auto m=e.model(s.model);const auto a=endpoint(e,s.vertex_a),b=endpoint(e,s.vertex_b);const auto cp=split_controls({a,{s.control_x,s.control_y},b,false},q);
    const int v=e.create(e.a.vertices,0,VectorVertex{p.x,p.y,{}});e.owner.vertices.push_back(v);e.reference();const int left=e.create(e.a.subcurves,2,VectorSubcurve{}),right=e.create(e.a.subcurves,2,VectorSubcurve{});
    e.sub(left)={s.vertex_a,v,cp[0].x,cp[0].y,s.model,{}};e.sub(right)={v,s.vertex_b,cp[2].x,cp[2].y,s.model,{}};e.vertex(s.vertex_a).subcurves.push_back(left);e.vertex(v).subcurves={left,right};e.vertex(s.vertex_b).subcurves.push_back(right);e.reference(6);
    if((stored&&!s.scales.empty())||(!stored&&!(m.flags&2)))e.width_split(s,left,right,q);
    const auto at=std::ranges::find(e.model(s.model).subcurves,id);if(at==e.model(s.model).subcurves.end())throw std::invalid_argument("Vector SC split has no owner");const auto pos=std::size_t(at-e.model(s.model).subcurves.begin());e.model(s.model).subcurves.insert(e.model(s.model).subcurves.begin()+std::ptrdiff_t(pos),{left,right});release_sub(e,id);
}
void normalize(Editor& e,int lc,const std::map<int,std::size_t>& planes){const auto original=e.line(lc).members;for(auto m:original){const auto subs=e.model(m.model).subcurves;for(auto id:subs)normalize_sub(e,id,0);}remove_degenerate(e,lc);if(std::ranges::find(e.a.lines,lc,&VectorPoolRecord<VectorLine>::index)==e.a.lines.end())return;
    const auto members=e.line(lc).members;for(auto member:members){int mc=member.model;for(;;){const auto m=e.model(mc);if(m.subcurves.size()<2)break;double threshold=binary64(double(std::bit_cast<float>(e.line(lc).half_width_bits))*.0625+150);const float limit=threshold>177?177.f:f32(threshold);bool cut{};
        for(std::size_t i=1;i<m.subcurves.size();++i){const auto a=e.sub(m.subcurves[i-1]),b=e.sub(m.subcurves[i]);const auto v=endpoint(e,b.vertex_a);P ca{a.control_x,a.control_y},cb{b.control_x,b.control_y};if(ca==v)ca=endpoint(e,a.vertex_a);if(cb==v)cb=endpoint(e,b.vertex_b);
            const float h=vector_curve_detail::heading({double(v.x),double(v.y)},{double(ca.x),double(ca.y)});double delta=binary64(double(vector_curve_detail::heading({double(v.x),double(v.y)},{double(cb.x),double(cb.y)}))-h);if(delta<0)delta=binary64(delta+360);if(delta>180)delta=binary64(360-delta);
            if(delta<limit){const auto s=e.split(mc,m.subcurves[i],0);if(s.cut){mc=s.right;cut=true;break;}}
        }if(!cut)break;
    }}
    vector_edit_detail::rebuild_line_endpoints(e.a,e.layer,lc,planes,e.wire,e.nodes,e.refs,e.storage,e.budget.work,e.budget.stop);e.slots[4].reset();e.cache(lc,true);
}
}
