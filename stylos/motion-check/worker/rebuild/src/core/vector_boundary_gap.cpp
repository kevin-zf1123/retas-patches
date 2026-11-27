#include "vector_boundary_gap.hpp"
#include "vector_boundary_source_copy.hpp"
#include "vector_curve_query.hpp"
#include "vector_curve_math.hpp"
#include "vector_fill_boundary.hpp"
#include <set>
namespace retas::vector_boundary_detail {
namespace {
using namespace vector_probe_detail;
using namespace vector_graph_detail;
using numeric_detail::binary64;
P point(Editor& e,int id){const auto& v=e.vertex(id);return {v.x,v.y};}
Curve curve(Editor& e,int id,bool force_bezier=false){const auto s=e.sub(id);return {point(e,s.vertex_a),{s.control_x,s.control_y},point(e,s.vertex_b),!force_bezier&&bool(e.model(s.model).flags&1)};}
VectorBounds box(Editor& e,int id){const auto m=e.model(id);return {m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]};}
int wrap_add(int a,int z){return std::bit_cast<std::int32_t>(std::uint32_t(a)+std::uint32_t(z));}
int wrap_sub(int a,int z){return std::bit_cast<std::int32_t>(std::uint32_t(a)-std::uint32_t(z));}
int half(int a,int z){const auto n=wrap_add(a,z);return n>=0?n/2:-int((-(std::int64_t)n+1)/2);}
P middle(P a,P z){return {half(a.x,z.x),half(a.y,z.y)};}
bool contains(VectorBounds b,P p){return b.left<=p.x&&b.top<=p.y&&p.x<b.right&&p.y<b.bottom;}
VectorBounds expand(VectorBounds b,int radius){return {wrap_sub(b.left,radius),wrap_sub(b.top,radius),wrap_add(b.right,radius),wrap_add(b.bottom,radius)};}
std::map<float,int> fan(Editor& e,int vertex){std::map<float,int> out;for(int id:e.vertex(vertex).subcurves){e.budget.charge(128);const auto s=e.sub(id);if(s.vertex_a!=vertex&&s.vertex_b!=vertex)throw std::invalid_argument("Gap nonincident physical adjacency");out[vector_fill_detail::boundary_heading(e.a,id,vertex,e.budget)]=id;}return out;}
int side_region(Integrator& context,int model,int side){auto& e=context.geometry();const auto m=e.model(model);const int id=m.targets[side];if(id<0)return -1;return m.target_kinds[side]?e.value(context.paint().regions,id).outer.region:e.value(context.paint().loops,id).region;}
double folded(float a,float z){const auto delta=std::abs(binary64(double(a)-z));return delta>180?binary64(360-delta):delta;}
bool direct_edge(Editor& e,int a,int z){for(auto [v,target]:{std::pair{a,z},std::pair{z,a}}){for(int id:e.vertex(v).subcurves){e.budget.charge();const auto s=e.sub(id);const auto p=point(e,s.vertex_a==v?s.vertex_b:s.vertex_a);if(p==point(e,target))return true;}}return false;}
bool cone(float heading,const GapWedge& w){const auto lo=std::min(w.previous,w.current),hi=std::max(w.previous,w.current);const bool big=binary64(double(hi)-lo)>180;return heading>lo&&heading<=hi?big:!big;}
//5B1290: obstacle append and best-radius shrink deliberately precede later
//rejection gates. LOCATION.t is replaced only after both target-fan passes.
bool accept(Integrator& context,int source,const GapWedge& wedge,GapTarget& target,int& radius,std::vector<int>& obstacles,const Scope& scope){
    auto& e=context.geometry();e.budget.charge(128);const auto s=e.sub(target.subcurve);const auto c=curve(e,target.subcurve);const auto from=point(e,source);const auto at=position(c,target.parameter,e.budget);
    const auto dx=binary64(at.x-from.x),dy=binary64(at.y-from.y);if(std::abs(dx)<1e-8&&std::abs(dy)<1e-8)return false;
    const auto distance=binary64(std::sqrt(binary64(binary64(dx*dx)+binary64(dy*dy))));if(distance>radius)return false;
    obstacles.push_back(target.subcurve);charge(e.storage,4,512ull*1024*1024,"Gap candidate storage limit");const P truncated{integer(at.x),integer(at.y)};if(!scope.point(truncated,e.budget))return false;
    const float heading=vector_curve_detail::heading({double(from.x),double(from.y)},at);if(folded(heading,wedge.previous)<20)return false;
    if(wedge.two_sided&&(folded(heading,wedge.current)<20||!cone(heading,wedge)))return false;
    for(int id:e.vertex(source).subcurves){const auto edge=e.sub(id);const int other=edge.vertex_a==source?edge.vertex_b:edge.vertex_a;if(other==s.vertex_a||other==s.vertex_b)return false;}
    radius=integer(distance);int endpoint=-1,endpoint_distance{};float snapped{};
    const int first=sq(truncated,c.start),last=sq(truncated,c.end);
    if(first<64){endpoint=s.vertex_a;endpoint_distance=first;snapped=0;}else if(last<64){endpoint=s.vertex_b;endpoint_distance=last;snapped=1;}
    if(endpoint>=0&&direct_edge(e,source,endpoint))return false;
    if(distance<16)target.special=true;if(endpoint<0)return true;
    const auto target_fan=fan(e,endpoint);float back=f32(binary64(double(heading)+180));if(back>360)back=f32(binary64(double(back)-360));
    for(auto [h,id]:target_fan){(void)id;if(folded(h,back)<20)return false;}
    const auto endpoint_point=point(e,endpoint);const auto actual_heading=vector_curve_detail::heading({double(endpoint_point.x),double(endpoint_point.y)},{double(from.x),double(from.y)});
    for(auto [h,id]:target_fan){(void)id;if(folded(h,actual_heading)<20)return endpoint_distance>16;}
    target.parameter=snapped;return true;
}
std::vector<GapTarget> gather(Editor& e,int model,int source,int radius){
    std::vector<GapTarget> out;const auto from=point(e,source);if(!contains(expand(box(e,model),radius),from))return out;
    const auto ids=e.model(model).subcurves;const int maximum=std::bit_cast<std::int32_t>(std::uint32_t(radius)*std::uint32_t(radius));
    for(int id:ids){e.budget.charge(128);const auto s=e.sub(id);if(s.vertex_a==source||s.vertex_b==source)continue;const auto c=curve(e,id);VectorBounds hull{std::min(c.start.x,c.end.x),std::min(c.start.y,c.end.y),wrap_add(std::max(c.start.x,c.end.x),1),wrap_add(std::max(c.start.y,c.end.y),1)};
        if(!c.linear){hull.left=std::min(hull.left,c.control.x);hull.top=std::min(hull.top,c.control.y);hull.right=std::max(hull.right,wrap_add(c.control.x,1));hull.bottom=std::max(hull.bottom,wrap_add(c.control.y,1));}
        if(!contains(expand(hull,radius),from))continue;const auto hit=vector_query_detail::nearest(c,from,e.budget);if(sq(from,hit.point)<=maximum){out.push_back({model,id,hit.parameter,false});charge(e.storage,16,512ull*1024*1024,"Gap LOCATION storage limit");}}
    return out;
}
std::vector<VectorLineMember> region_members(Integrator& context,int id){auto& e=context.geometry();const auto r=e.value(context.paint().regions,id);auto members=r.outer.members;for(int hole:r.holes){const auto more=e.value(context.paint().loops,hole).members;members.insert(members.end(),more.begin(),more.end());e.budget.charge(more.size());}return members;}
int new_vertex(Editor& e,P p){const int id=e.create(e.a.vertices,0,VectorVertex{p.x,p.y,{}});e.owner.vertices.push_back(id);e.reference();return id;}
std::pair<int,int> new_span(Editor& e,P a,P z,P control,unsigned flags){const int first=new_vertex(e,a),last=new_vertex(e,z);VectorModelCurve m;m.flags=flags;m.line=-1;m.targets={-1,-1};m.crosspoints={-1,-1};const int model=e.create(e.a.models,3,m);e.owner.models.push_back(model);e.reference();const int sub=e.create(e.a.subcurves,2,VectorSubcurve{first,last,control.x,control.y,model,{}});e.model(model).subcurves.push_back(sub);e.vertex(first).subcurves.push_back(sub);e.vertex(last).subcurves.push_back(sub);e.reference(3);return {model,sub};}
void cache_model(Editor& e,int id){vector_geometry_detail::rebuild_models(e.a,{id},e.budget.work,e.budget.stop);}
void normalize_span(Editor& e,int id,unsigned depth=0){e.budget.charge(128);if(depth>64)throw std::length_error("Gap SC normalization recursion limit");const auto s=e.sub(id);const auto c=curve(e,id,true);const float heading=vector_curve_detail::heading({double(c.start.x),double(c.start.y)},{double(c.control.x),double(c.control.y)});double delta=std::abs(binary64(double(vector_curve_detail::heading({double(c.control.x),double(c.control.y)},{double(c.end.x),double(c.end.y)}))-heading));if(delta>=180)delta=binary64(360-delta);if(delta<30)return;
    const auto cut=integer_position(c,.5f,e.budget);if(sq(c.start,cut)<64||sq(cut,c.end)<64){const auto p=middle(c.start,c.end);e.sub(id).control_x=p.x;e.sub(id).control_y=p.y;e.topology_changed=true;return;}
    const auto position=std::size_t(std::ranges::find(e.model(s.model).subcurves,id)-e.model(s.model).subcurves.begin());split_subcurve(e,id,.5f,cut,true);const auto ids=e.model(s.model).subcurves;normalize_span(e,ids.at(position),depth+1);normalize_span(e,ids.at(position+1),depth+1);
}
void release_span(Editor& e,int id){const auto s=e.sub(id);if(s.vertex_a==s.vertex_b)throw std::invalid_argument("Gap source self-loop release is unsupported");for(int v:{s.vertex_a,s.vertex_b}){Editor::remove(e.vertex(v).subcurves,id);if(e.vertex(v).subcurves.empty()){Editor::remove(e.owner.vertices,v);e.erase(e.a.vertices,0,v);}}for(int scale:s.scales)e.erase(e.a.scales,1,scale);Editor::remove(e.model(s.model).subcurves,id);e.erase(e.a.subcurves,2,id);}
GapResult convert(Integrator& context,int model){auto& e=context.geometry();GapResult out;out.context_dirty=box(e,model);auto affected=query_candidates(context,out.context_dirty);const int region=context.create_region(),hole=context.create_hole(region);context.populate_loop(false,hole,{{model,1},{model,0}});
    if(!affected.regions.empty()){auto repair=run(e,{model},affected.models,affected.regions,context.line_planes(),context.face_planes());unite(out.context_dirty,repair.context_dirty);unite(out.context_dirty,context.commit_regions(repair,{region},affected.regions,false,nullptr,&out.regions));}
    context.merge_exteriors_once(out.context_dirty);context.publish();out.processed=true;out.storage_changed=true;return out;
}
}
GapWedge source_gap_wedge(Integrator& context,int vertex){auto& e=context.geometry();const auto ordered=fan(e,vertex);GapWedge out;if(ordered.empty())return out;if(e.vertex(vertex).subcurves.size()==1){out.previous=ordered.begin()->first;out.region=side_region(context,e.sub(ordered.begin()->second).model,0);return out;}
    auto previous=std::prev(ordered.end());for(auto current=ordered.begin();current!=ordered.end();++current){double delta=binary64(double(current->first)-previous->first);if(delta<0)delta=binary64(delta+360);if(delta>180){out.two_sided=true;out.previous=previous->first;out.current=current->first;const auto s=e.sub(current->second);out.region=side_region(context,s.model,s.vertex_a==vertex?0:1);return out;}previous=current;}return out;
}
std::optional<GapTarget> find_gap_target(Integrator& context,int source,const GapWedge& wedge,int radius,const Scope& scope){auto& e=context.geometry();if(radius<=0||wedge.region<0)return {};std::optional<GapTarget> chosen;std::vector<int> obstacles;for(auto member:region_members(context,wedge.region)){const auto locations=gather(e,member.model,source,radius);for(auto location:locations){const bool accepted=accept(context,source,wedge,location,radius,obstacles,scope);if(accepted||(location.special&&e.vertex(source).subcurves.size()<=1))chosen=location;}}
    if(!chosen)return {};const auto source_point=point(e,source),target_point=node_position(curve(e,chosen->subcurve),chosen->parameter,e.budget);const Curve connector{source_point,middle(source_point,target_point),target_point,true};for(int obstacle:obstacles){if(obstacle==chosen->subcurve)continue;for(const auto& hit:vector_query_detail::cross_obstacle(curve(e,obstacle,true),connector,e.budget))if(hit.point!=source_point&&hit.point!=target_point)return {}; }return chosen;
}
GapResult join_gap(Integrator& context,int source,const GapTarget& target,bool special,bool joint_line_extra){auto& e=context.geometry();GapResult out;const auto original_box=box(e,target.model);const auto from=point(e,source);const auto split=e.split(target.model,target.subcurve,target.parameter);out.context_dirty=original_box;if(!special&&split.cut){unite(out.context_dirty,box(e,split.left));unite(out.context_dirty,box(e,split.right));}out.storage_changed=split.cut;const auto to=point(e,split.vertex);
    if(!special){if(from==to)return out;const auto [model,sub]=new_span(e,from,to,middle(from,to),joint_line_extra?0x41u:1u);(void)sub;cache_model(e,model);auto converted=convert(context,model);unite(out.context_dirty,converted.context_dirty);out.processed=true;out.storage_changed=true;out.regions=std::move(converted.regions);return out;}
    if(e.vertex(source).subcurves.size()!=1)throw std::invalid_argument("Gap special branch requires degree-one source");const int source_sub=e.vertex(source).subcurves.front();const auto original=e.sub(source_sub);const int source_model=original.model;const auto old=e.model(source_model);const bool is_a=original.vertex_a==source;const int line=old.line;bool aligned{};if(line>=0&&!e.line(line).members.empty()){const auto member=e.line(line).members.front();if(member.model==source_model){const int first=member.selector?old.subcurves.front():old.subcurves.back();const auto s=e.sub(first);aligned=(member.selector?s.vertex_a:s.vertex_b)==source&&first==source_sub;}}
    const auto a=is_a?to:point(e,original.vertex_a),z=is_a?point(e,original.vertex_b):to;const auto [model,sub]=new_span(e,a,z,{original.control_x,original.control_y},old.flags);
    for(int scale:original.scales){const auto value=e.value(e.a.scales,scale);const int id=e.create(e.a.scales,1,value);e.sub(sub).scales.push_back(id);e.reference();}normalize_span(e,sub);cache_model(e,model);release_span(e,source_sub);if(e.model(source_model).crosspoints[is_a?0:1]>=0)clear_endpoint(e,source_model,is_a);cache_model(e,source_model);
    if(e.model(source_model).subcurves.empty()){if(line>=0){auto& members=e.line(line).members;const auto it=std::ranges::find(members,source_model,&VectorLineMember::model);if(it==members.end())throw std::invalid_argument("Gap source MC lacks its LC member");members.erase(it);}
        const int loop=old.targets[1];const bool embedded=old.target_kinds[1]!=0;if(loop<0||old.targets[0]!=loop||old.target_kinds[0]!=old.target_kinds[1])throw std::invalid_argument("Gap exhausted source requires the same two-sided Loop");auto& members=embedded?e.value(context.paint().regions,loop).outer.members:e.value(context.paint().loops,loop).members;for(int side:{1,0}){const auto it=std::ranges::find_if(members,[&](auto m){return m.model==source_model&&bool(m.selector)==bool(side);});if(it==members.end())throw std::invalid_argument("Gap exhausted source lacks Loop occurrence");members.erase(it);}context.cache_loop(embedded,loop);if(!embedded&&members.empty())context.retire_empty_hole(loop);
        for(int lane=0;lane<2;++lane){if(e.model(model).crosspoints[lane]>=0)clear_endpoint(e,model,lane==0);e.model(model).crosspoints[lane]=e.model(source_model).crosspoints[lane];e.model(source_model).crosspoints[lane]=-1;}Editor::remove(e.owner.models,source_model);e.erase(e.a.models,3,source_model);}
    if(line>=0){auto& members=e.line(line).members;const VectorLineMember member{model,int(aligned?is_a:!is_a)};if(aligned&&!members.empty())members.insert(members.begin(),member);else members.push_back(member);e.reference();e.model(model).line=line;cache_model(e,model);e.cache(line,false);}
    auto converted=convert(context,model);unite(out.context_dirty,converted.context_dirty);out.processed=true;out.storage_changed=true;out.regions=std::move(converted.regions);return out;
}
static GapResult join_raster_gap(Integrator& context,int seed,int source,const GapTarget& target,int gap){
    auto& e=context.geometry();GapResult out;out.context_dirty=box(e,target.model);const auto split=e.split(target.model,target.subcurve,target.parameter);out.storage_changed=split.cut;const auto from=point(e,source),to=point(e,split.vertex);if(from==to)return out;
    VectorModelCurve curve;curve.flags=0x41;curve.line=-1;curve.targets={-1,-1};curve.crosspoints={-1,-1};const int model=e.create(e.a.models,3,curve);e.owner.models.push_back(model);e.reference();const auto c=middle(from,to);const int sub=e.create(e.a.subcurves,2,VectorSubcurve{source,split.vertex,c.x,c.y,model,{}});e.model(model).subcurves.push_back(sub);e.vertex(source).subcurves.push_back(sub);e.vertex(split.vertex).subcurves.push_back(sub);e.reference(3);cache_model(e,model);
    out.processed=context.convert_raster_connector(seed,model,gap,out.regions,out.context_dirty);out.storage_changed=true;if(!out.processed)context.erase_model_after_repair(model);return out;
}
static GapResult close_gaps_impl(Integrator& context,int seed,int radius,const Scope& scope,bool joint_line_extra,bool raster){auto& e=context.geometry();e.budget.charge();if(radius==0||(raster&&radius<0))throw std::invalid_argument("Gap driver requires nonzero raw radius; raster gaps require positive radius");std::vector<int> original_vertices;for(auto member:region_members(context,seed)){const auto m=e.model(member.model);if(m.subcurves.empty())throw std::invalid_argument("Gap snapshot has empty MC");const auto s=e.sub(member.selector?m.subcurves.front():m.subcurves.back());original_vertices.push_back(member.selector?s.vertex_a:s.vertex_b);charge(e.storage,4,512ull*1024*1024,"Gap snapshot storage limit");}
    std::set<int> live{seed};GapResult out;for(int vertex:original_vertices){e.budget.charge(128);const auto found=std::ranges::find(e.a.vertices,vertex,&VectorPoolRecord<VectorVertex>::index);if(found==e.a.vertices.end()||found->value.subcurves.empty())continue;if(!scope.point(point(e,vertex),e.budget))continue;const auto wedge=source_gap_wedge(context,vertex);if(!live.contains(wedge.region))continue;const auto target=find_gap_target(context,vertex,wedge,radius,scope);if(!target)continue;auto joined=raster?join_raster_gap(context,wedge.region,vertex,*target,radius):join_gap(context,vertex,*target,target->special&&e.vertex(vertex).subcurves.size()==1,joint_line_extra);unite(out.context_dirty,joined.context_dirty);out.storage_changed|=joined.storage_changed;if(!joined.processed)continue;live.erase(wedge.region);live.insert(joined.regions.begin(),joined.regions.end());out.processed=true;}
    if(!live.empty()){const int page=*live.begin()>>5;for(int region:live)if(region<0||region>>5!=page)throw std::invalid_argument("Gap live Region pointer order crosses pool pages");}out.regions.assign(live.begin(),live.end());context.publish();return out;
}
GapResult close_gaps(Integrator& c,int seed,int radius,const Scope& scope,bool extra){return close_gaps_impl(c,seed,radius,scope,extra,false);}
GapResult close_raster_gaps(Integrator& c,int seed,int radius,const Scope& scope){return close_gaps_impl(c,seed,radius,scope,true,true);}
GapResult join_gap(Integrator& c,int source,const GapTarget& target,bool special){return join_gap(c,source,target,special,false);}
GapResult close_gaps(Integrator& c,int seed,int radius,const Scope& scope){return close_gaps(c,seed,radius,scope,false);}

}
