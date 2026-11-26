#include "vector_eraser_point_trim.hpp"
#include "vector_boundary_source_copy.hpp"
#include "vector_line_color_cut.hpp"
#include "vector_line_detach.hpp"
#include "retas/vector_color_metadata.hpp"

namespace retas::vector_eraser_detail {
namespace {
using namespace vector_graph_detail;
using namespace vector_probe_detail;
using vector_color_detail::collect_line_split_events;
using vector_color_detail::materialize_line_split_events;
using Event=vector_color_detail::LineSplitEvent;
struct TrimReplay {bool initial{};std::map<int,std::vector<Event>> events;};
bool start_side(Editor& e,int model,bool forward,const Scope& scope){
    const auto& m=e.model(model);if(m.subcurves.empty())throw std::invalid_argument("Vector Eraser requires nonempty MC chains");
    const auto& sc=e.sub(forward?m.subcurves.front():m.subcurves.back());const auto& v=e.vertex(forward?sc.vertex_a:sc.vertex_b);return scope.point({v.x,v.y},e.budget);
}
void source_admission(Editor& e,const VectorLine& line){
    if(line.members.empty())throw std::invalid_argument("Vector Eraser requires nonempty LC chains");std::set<int> unique;
    for(auto member:line.members){e.budget.charge(32);if(!unique.insert(member.model).second)throw std::invalid_argument("Vector Eraser repeated source MC members are unsupported");}
}
void pending_regions(Editor& e,int model,std::vector<int>& pending){
    const auto& m=e.model(model);for(int lane:{0,1}){if(m.targets[lane]<0)throw std::invalid_argument("Vector Eraser requires recovered complete side ownership");const int region=m.target_kinds[lane]?m.targets[lane]:e.value(e.paint->loops,m.targets[lane]).region;
        if(region<0)throw std::invalid_argument("Vector Eraser pending Region has no recovered live prelude");(void)e.value(e.paint->regions,region);pending.push_back(region);e.budget.charge();}
}
void publish_line_owner(Editor& e,std::size_t plane){
    if(!e.owner.border_owner){e.owner.publish(e.layer.planes.at(plane),nullptr);return;}
    OwnerLists links(e.layer.planes.at(plane),e.paint);links.lines=e.owner.lines;links.vertices=e.owner.vertices;links.models=e.owner.models;links.publish(e.layer.planes.at(plane),e.paint);
}
void point_direct(Editor& e,int source,const Scope& scope,std::map<int,std::size_t>& planes,std::vector<int>& pending,PointSelectionResult* extracted=nullptr,std::size_t plane=0){
    const auto original=e.line(source);source_admission(e,original);
    bool inside=extracted?start_side(e,original.members.front().model,original.members.front().selector!=0,scope):false,force{};int current=-1;
    const auto create=[&](){current=e.new_line(original);planes.emplace(current,plane);extracted->selected_lines.push_back(current);};
    const auto finish=[&](){if(current>=0){e.cache(current,true);unite(extracted->context_dirty,e.line(current).archived_bounds);current=-1;}};
    if(extracted){extracted->source_lines.push_back(source);if(inside)create();}
    //5D7E90 arg_C=0 keeps detached MCs alive until5C1E10. Physical
    //direction changes traversal only; materialized side labels stay intact.
    for(auto member:original.members){const bool side=start_side(e,member.model,true,scope);const auto events=collect_line_split_events(e.a,member.model,side,scope,e.budget);auto info=materialize_line_split_events(e,member.model,events,side);
        if(!member.selector)std::reverse(info.parts.begin(),info.parts.end());for(auto [model,selected]:info.parts){if(extracted&&(inside!=selected||force)){finish();inside=selected;force=false;if(inside)create();}
            if(selected){if(e.paint)pending_regions(e,model,pending);if(extracted)publish_line_owner(e,plane);const auto detached=vector_line_detail::detach_model_from_line(e,model,planes,&e.owner.lines);if(!detached.detached)throw std::invalid_argument("Vector Eraser Point lost its actual source LC");
                if(extracted){if(detached.created_line>=0)extracted->source_lines.push_back(detached.created_line);e.line(current).members.push_back({model,member.selector});e.model(model).line=current;e.reference();}}
        }
        if(extracted&&((member.selector&&info.end)||(!member.selector&&info.start))){inside=!inside;force=true;}
    }if(extracted){finish();std::erase_if(extracted->source_lines,[&](int id){return !planes.contains(id);});}
}
void point_vertex(Editor& e,int source,const Scope& scope,SplitReleaseResult& result,const TrimReplay* replay=nullptr,Editor* destination=nullptr,bool complement=false,std::vector<int>* destination_lines=nullptr,VectorBounds* replacement_dirty=nullptr){
    const auto original=e.line(source);source_admission(e,original);const auto first=original.members.front();bool inside=(replay?replay->initial:start_side(e,first.model,first.selector!=0,scope))!=complement,pending{};int current=-1,carry=-1;Editor* owner=nullptr;
    const auto create=[&](){owner=inside?destination:&e;if(!owner)return -1;const int line=owner->new_line(original);if(owner==&e)result.new_lines.push_back(line);else destination_lines->push_back(line);carry=-1;return line;};
    const auto close=[&](){if(current<0)return;if(owner->line(current).members.empty()){Editor::remove(owner->owner.lines,current);owner->erase(owner->a.lines,5,current);}else{owner->cache(current,true);unite(result.context_dirty,owner->line(current).archived_bounds);if(replacement_dirty)unite(*replacement_dirty,owner->line(current).archived_bounds);}current=-1;};
    current=create();
    //5D74F0 copies kept spans with5D0D30 and selector1. All original
    //split spans remain in source until the final59C4E0 deep release.
    for(auto member:original.members){bool side=replay?inside:start_side(e,member.model,true,scope);std::vector<Event> events;if(replay){if(const auto found=replay->events.find(member.model);found!=replay->events.end())events=found->second;if(!member.selector&&!events.empty()){side=events.back().code==0;std::reverse(events.begin(),events.end());for(auto& event:events)event.code=event.code==0?1:0;}}else events=collect_line_split_events(e.a,member.model,side,scope,e.budget);auto info=materialize_line_split_events(e,member.model,events,side);
        if(!member.selector)std::reverse(info.parts.begin(),info.parts.end());for(auto [model,selected]:info.parts){selected=selected!=complement;if(selected!=inside||pending){close();inside=selected;pending=false;current=create();}
            if(current>=0){const int copied=vector_boundary_detail::copy_directed_model(*owner,e.a,model,member.selector!=0,carry);owner->line(current).members.push_back({copied,1});owner->model(copied).line=current;owner->reference();}}
        if((member.selector&&info.end)||(!member.selector&&info.start)){inside=!inside;pending=true;}
    }close();erase_line(e,source);
}
bool shared_endpoint(Editor& e,const vector_boundary_detail::ModelLocationPair& hit){
    const auto a=e.sub(hit.first_sub),b=e.sub(hit.second_sub);const int first=hit.first_parameter==0?a.vertex_a:hit.first_parameter==1?a.vertex_b:-1,second=hit.second_parameter==0?b.vertex_a:hit.second_parameter==1?b.vertex_b:-1;return first>=0&&first==second;
}
std::array<VectorPoint,2> peer_hints(Editor& e,int sub,float t){
    const auto source=e.sub(sub);const bool linear=bool(e.model(source.model).flags&1);
    const auto evaluate=[&](int id,float q){const auto sc=e.sub(id);const auto a=e.vertex(sc.vertex_a),b=e.vertex(sc.vertex_b);const P point=node_position({{a.x,a.y},{sc.control_x,sc.control_y},{b.x,b.y},linear},q,e.budget);return VectorPoint{double(point.x),double(point.y)};};
    const auto neighbor=[&](int vertex){for(int id:e.vertex(vertex).subcurves){e.budget.charge();if(id!=sub)return id;}return -1;};VectorPoint before{},after{};bool missing{};
    //5C7240/5ABB10: first non-self physical Vertex adjacency, retaining the
    //current SC's linear selector. At a missing first neighbor the observed
    //reflection uses source.VertexB, including the t<=0 branch.
    if(t<=0){const int prior=neighbor(source.vertex_a);if(prior>=0)before=evaluate(prior,.8f);else missing=true;}else before=evaluate(sub,f32(binary64(double(t)-double(.2f))));
    if(t>=1){const int next=neighbor(source.vertex_b);if(next>=0)after=evaluate(next,.2f);else{const auto end=e.vertex(source.vertex_b);after={double(integer(2*double(end.x)-before.x)),double(integer(2*double(end.y)-before.y))};}}
    else{after=evaluate(sub,f32(binary64(double(t)+double(.2f))));if(missing){const auto end=e.vertex(source.vertex_b);before={double(integer(2*double(end.x)-after.x)),double(integer(2*double(end.y)-after.y))};}}
    return {after,before};
}
TrimReplay trim_replay(Editor& e,int source,std::size_t plane,const Scope& scope){
    const auto original=e.line(source);source_admission(e,original);std::vector<int> peers;
    for(std::size_t i=0;i<e.layer.planes.size();++i){const auto& child=e.layer.planes[i];if(child.opaque||!child.common.flags[1]||(child.family!=0x21210001&&child.family!=0x21220001))continue;const auto lines=i==plane?e.owner.lines:vector_line_plane_references(child);for(int line:lines){e.budget.charge();if(overlap(original.archived_bounds,e.line(line).archived_bounds))peers.push_back(line);}}
    struct Tagged {int model;Event event;};std::vector<Tagged> ordered;
    const auto append=[&](int model,Event event){e.budget.charge(128);charge(e.storage,sizeof(Tagged),vector_material_detail::max_bytes,"Vector Trim replay storage limit");if(ordered.size()>=8192)throw std::length_error("Vector Trim ordered event limit");ordered.push_back({model,event});};
    for(auto member:original.members){const bool side=start_side(e,member.model,true,scope);auto events=collect_line_split_events(e.a,member.model,side,scope,e.budget,false);const auto model=e.model(member.model);const VectorBounds saved{model.scalars[0],model.scalars[1],model.scalars[2],model.scalars[3]};
        for(int line:peers){const auto peer=e.line(line);if(!overlap(saved,peer.archived_bounds))continue;for(auto other:peer.members){const auto mc=e.model(other.model);if(!overlap(saved,{mc.scalars[0],mc.scalars[1],mc.scalars[2],mc.scalars[3]}))continue;
                for(const auto hit:vector_boundary_detail::collect_model_locations(e,member.model,other.model)){if(shared_endpoint(e,hit))continue;if(events.size()>=8192)throw std::length_error("Vector Trim projected event limit");charge(e.storage,sizeof(Event),vector_material_detail::max_bytes,"Vector Trim projected storage limit");events.push_back({2,hit.first_sub,hit.first_parameter,peer_hints(e,hit.second_sub,hit.second_parameter)});}}}
        const auto scan=[&](int sub){std::map<float,Event> by_parameter;for(const auto& event:events){e.budget.charge();if(event.sub!=sub)continue;if(!std::isfinite(event.t))throw std::invalid_argument("Vector Trim has a nonfinite scalar key");if(event.code==2)by_parameter.try_emplace(event.t,event);else by_parameter[event.t]=event;}
            if(member.selector){for(const auto& [t,event]:by_parameter){(void)t;append(member.model,event);}}else for(auto it=by_parameter.rbegin();it!=by_parameter.rend();++it){auto event=it->second;if(event.code!=2)event.code=event.code==0?1:0;append(member.model,event);}};
        if(member.selector)for(int sub:model.subcurves)scan(sub);else for(auto it=model.subcurves.rbegin();it!=model.subcurves.rend();++it)scan(*it);
    }
    TrimReplay result;const auto first=original.members.front();result.initial=start_side(e,first.model,first.selector!=0,scope);bool inside=result.initial,pending{};
    const auto output=[&](const Tagged& tagged,int code){auto event=tagged.event;event.code=code;result.events[tagged.model].push_back(event);};
    //5D95E1..5D977B extends raster enter/exit to neighboring code2 nodes.
    //No peer boundary means the hit interval reaches the source LC endpoint.
    for(std::size_t i=0;i<ordered.size();++i){const auto& current=ordered[i];const int code=current.event.code;if(inside){if(code==1){inside=false;pending=true;}continue;}
        if(pending){if(code==2&&(i+1==ordered.size()||ordered[i+1].event.code==2)){output(current,1);pending=false;}else if(code==0){pending=false;inside=true;}continue;}
        if(code==0){if(i&&ordered[i-1].event.code==2)output(ordered[i-1],0);else if(i==0)result.initial=true;inside=true;}
    }return result;
}
void trim_direct(Editor& e,int source,const Scope& scope,std::vector<int>& pending,SplitReleaseResult& result){
    const auto original=e.line(source);source_admission(e,original);bool inside{};int current=-1;bool first=true;
    const auto finish=[&](){if(current<0)return;e.cache(current,false);unite(result.context_dirty,e.line(current).archived_bounds);current=-1;};
    //5D85B0 partitions complete MC members using5C7E70, never SC events.
    //The initial retained LC appends; later retained runs insert before source.
    for(auto member:original.members){Graph graph(e.a,e.budget);const bool selected=graph.model_matches(e.model(member.model),scope,false,e.budget);const bool entering=!first&&selected!=inside;
        if(first){if(!selected){current=e.new_line(original);result.new_lines.push_back(current);}first=false;}
        else if(entering){if(current>=0){vector_edit_detail::partition_endpoint(e.a,e.layer,member.model,false,member.selector==0,original.half_width_bits,e.wire,e.nodes,e.refs,e.storage,e.budget.work,e.budget.stop);}finish();if(!selected){current=e.new_line(original,source);result.new_lines.push_back(current);}}
        if(selected){pending_regions(e,member.model,pending);e.model(member.model).line=-1;}
        else{e.line(current).members.push_back(member);e.model(member.model).line=current;e.reference();if(entering){vector_edit_detail::partition_endpoint(e.a,e.layer,member.model,true,member.selector!=0,original.half_width_bits,e.wire,e.nodes,e.refs,e.storage,e.budget.work,e.budget.stop);}}
        e.slots[4].reset();inside=selected;
    }finish();e.line(source).members.clear();Editor::remove(e.owner.lines,source);e.erase(e.a.lines,5,source);
}
}
PairedPointResult replace_point_pair(Editor& source,std::size_t source_plane,int source_line,Editor& destination,std::size_t destination_plane,const Scope& scope,bool complement,bool trim){
    source.budget.charge();destination.budget.charge();
    const auto valid=[&](Editor& e,std::size_t plane){if(e.paint||e.owner.border_owner||!e.owner.vertex_plane||plane>=e.layer.planes.size())return false;const auto& p=e.layer.planes[plane];return !p.opaque&&(p.family==0x21210001||p.family==0x21220001)&&p.common.resolution==numeric_detail::binary64(e.layer.common.resolution*16);};
    if(&source.a==&destination.a||&source.storage!=&destination.storage||&source.budget.work!=&destination.budget.work||!valid(source,source_plane)||!valid(destination,destination_plane))throw std::invalid_argument("Paired Point requires distinct native16 real Vert owners with shared budgets");
    if(std::ranges::count(source.owner.lines,source_line)!=1)throw std::invalid_argument("Paired Point source LC is not uniquely owned");
    PairedPointResult result;result.immediate_dirty=source.line(source_line).archived_bounds;SplitReleaseResult replay;replay.context_dirty=result.immediate_dirty;const auto trimmed=trim?std::optional<TrimReplay>(trim_replay(source,source_line,source_plane,scope)):std::nullopt;point_vertex(source,source_line,scope,replay,trimmed?&*trimmed:nullptr,&destination,complement,&result.destination_lines,&result.replacement_dirty);result.source_lines=std::move(replay.new_lines);result.context_dirty=replay.context_dirty;publish_line_owner(source,source_plane);publish_line_owner(destination,destination_plane);return result;
}
void retain_point_selection(Editor& e,std::size_t plane,int line,const Scope& scope){
    if(e.paint||!e.owner.vertex_plane)throw std::invalid_argument("COPY refinement requires an ordinary Vert owner");
    SplitReleaseResult result;point_vertex(e,line,scope,result,nullptr,nullptr,true);publish_line_owner(e,plane);
}
PointSelectionResult extract_point_selection(Editor& e,std::size_t plane,int source,const Scope& scope){
    e.budget.charge();if(e.paint||e.owner.border_owner||!e.owner.vertex_plane||plane>=e.layer.planes.size()||(e.layer.planes[plane].family!=0x21210001&&e.layer.planes[plane].family!=0x21220001)||std::ranges::count(e.owner.lines,source)!=1)throw std::invalid_argument("Point selection extraction requires its real ordinary Vert owner");
    PointSelectionResult result;result.immediate_dirty=e.line(source).archived_bounds;result.context_dirty=result.immediate_dirty;std::map<int,std::size_t> planes;for(int id:e.owner.lines)planes.emplace(id,plane);std::vector<int> pending;point_direct(e,source,scope,planes,pending,&result,plane);publish_line_owner(e,plane);return result;
}
PointSelectionResult extract_border_point_selection(vector_boundary_detail::Integrator& context,std::size_t plane,int source,const Scope& scope){
    auto& e=context.geometry();const auto& p=e.layer.planes.at(plane);
    if(!e.paint||!e.owner.border_owner||(p.family!=0x21110001&&p.family!=0x21120001))throw std::invalid_argument("Border Point extraction requires a real Pure child");
    OwnerLists links(p,e.paint);e.owner.lines=links.lines;auto owners=context.line_planes();std::vector<int> pending;PointSelectionResult result;
    result.immediate_dirty=e.line(source).archived_bounds;result.context_dirty=result.immediate_dirty;
    point_direct(e,source,scope,owners,pending,&result,plane);result.pending_regions=std::move(pending);publish_line_owner(e,plane);
    context.publish();return result;
}
PointSelectionResult extract_border_marked_trim(vector_boundary_detail::Integrator& context,std::size_t plane,int source){
    auto& e=context.geometry();const auto& p=e.layer.planes.at(plane);
    if(!e.paint||!e.owner.border_owner||(p.family!=0x21110001&&p.family!=0x21120001))throw std::invalid_argument("Border marked trim requires a real Pure child");
    OwnerLists links(p,e.paint);e.owner.lines=links.lines;const auto original=e.line(source);source_admission(e,original);
    PointSelectionResult result;result.immediate_dirty=result.context_dirty=original.archived_bounds;
    bool first=true,previous{};int current=-1;
    const auto finish=[&](){if(current>=0){e.cache(current,false);unite(result.context_dirty,e.line(current).archived_bounds);}};
    const auto endpoint=[&](VectorLineMember member,bool start){
        e.slots[4].reset();try{vector_edit_detail::partition_endpoint(e.a,e.layer,member.model,start,start?member.selector!=0:member.selector==0,original.half_width_bits,e.wire,e.nodes,e.refs,e.storage,e.budget.work,e.budget.stop);}catch(...){e.slots[4].reset();throw;}e.slots[4].reset();
    };
    for(auto member:original.members){e.budget.charge();const bool selected=bool(e.model(member.model).flags&0x10000u),transition=!first&&selected!=previous;
        if(transition){endpoint(member,false);finish();}
        if(first||transition){current=e.new_line(original,first?-1:source);(selected?result.selected_lines:result.source_lines).push_back(current);}
        e.line(current).members.push_back(member);e.model(member.model).line=current;e.reference();
        if(selected)pending_regions(e,member.model,result.pending_regions);
        if(transition)endpoint(member,true);first=false;previous=selected;
    }
    finish();if(e.refs<original.members.size()+1)throw std::invalid_argument("Border marked trim reference ledger underflow");
    e.refs-=original.members.size()+1;e.line(source).members.clear();Editor::remove(e.owner.lines,source);e.erase(e.a.lines,5,source);
    publish_line_owner(e,plane);context.publish();return result;
}
SplitReleaseResult erase_point_trim(Editor& e,vector_boundary_detail::Integrator* context,std::size_t plane,const std::vector<int>& input,const Scope& scope,int mode){
    if(mode!=0&&mode!=2)throw std::invalid_argument("Vector Eraser split release mode is invalid");const bool direct=e.owner.border_owner;if(direct&&(!context||!e.paint))throw std::invalid_argument("Vector Eraser split release requires its live Paint prelude");if(!direct&&context)throw std::invalid_argument("Vector Eraser Vert-Line paired Paint prelude is not recovered");SplitReleaseResult result;std::vector<int> pending;auto planes=context?context->line_planes():std::map<int,std::size_t>{};
    for(int source:input){Graph graph(e.a,e.budget);const auto original=e.line(source);if(!graph.line_matches(original,scope,false,e.budget))continue;unite(result.immediate_dirty,original.archived_bounds);unite(result.context_dirty,original.archived_bounds);
        if(mode==0){if(direct)point_direct(e,source,scope,planes,pending);else point_vertex(e,source,scope,result);}else{if(direct)trim_direct(e,source,scope,pending,result);else{const auto replay=trim_replay(e,source,plane,scope);point_vertex(e,source,scope,result,&replay);}}result.processed_lines.push_back(source);e.budget.charge();}
    if(result.processed_lines.empty())return result;publish_line_owner(e,plane);if(context){unite(result.context_dirty,context->materialize_regions(pending));context->publish();}e.budget.charge();return result;
}
}
