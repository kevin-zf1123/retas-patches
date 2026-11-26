#include "vector_mask_region.hpp"
#include "vector_fill_boundary.hpp"
#include "vector_line_detach.hpp"
#include <bit>

namespace retas::vector_mask_detail {
namespace {
using vector_graph_detail::Editor;
using vector_boundary_detail::Integrator;
using vector_boundary_detail::RepairState;
using vector_probe_detail::Budget;
struct Group {std::vector<VectorLineMember> members;bool positive{};VectorBounds bounds;};
bool overlaps(VectorBounds a,VectorBounds b){return a.left<b.right&&a.right>b.left&&a.top<b.bottom&&a.bottom>b.top;}
unsigned visited_mask(bool forward){return forward?0x10000000u:0x20000000u;}
int start_vertex(Editor& e,int model){const auto& m=e.model(model);if(m.subcurves.empty())throw std::invalid_argument("Scratch Region has an empty model");return e.sub(m.subcurves.front()).vertex_a;}
std::vector<int> line_models(Editor& e,int line){std::vector<int> out;for(auto member:e.line(line).members){e.budget.charge();if(e.model(member.model).line!=line)throw std::invalid_argument("Scratch LC has competing model ownership");out.push_back(member.model);}return out;}
void accumulate(RepairState& all,RepairState phase){all.events_found|=phase.events_found;for(auto [old,next]:phase.redirected_models)all.redirected_models[old]=next;}
RepairState normalize(Integrator& target,const std::vector<int>& lines,const std::map<int,std::size_t>& line_planes){
    auto& e=target.geometry();RepairState all;
    for(std::size_t i=0;i<lines.size();++i){
        e.budget.charge();const auto frozen=e.line(lines[i]).archived_bounds;auto current=line_models(e,lines[i]);
        auto self=vector_boundary_detail::intersect_pass(e,current,current,true,line_planes,target.face_planes());
        const bool changed=self.events_found;accumulate(all,std::move(self));
        std::vector<int> peers;
        for(std::size_t j=i+1;j<lines.size();++j){e.budget.charge();if(!overlaps(frozen,e.line(lines[j]).archived_bounds))continue;for(int id:line_models(e,lines[j])){const auto& m=e.model(id);if(overlaps(frozen,{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]}))peers.push_back(id);}}
        if(peers.empty())continue;if(changed)current=line_models(e,lines[i]);
        accumulate(all,vector_boundary_detail::intersect_pass(e,current,peers,false,line_planes,target.face_planes()));
    }
    return all;
}
Group walk(Editor& e,int model,bool side,const RepairState& repair){
    Group group;
    for(;;){
        e.budget.charge(128);e.model(model).flags|=visited_mask(side);group.members.push_back({model,side?1:0});
        vector_material_detail::charge(e.storage,16,512ull*1024*1024,"Scratch contour storage exceeds512MiB");
        const auto m=e.model(model);if(m.subcurves.empty())throw std::invalid_argument("Scratch contour has no original spans");const int incoming=side?m.subcurves.back():m.subcurves.front();const auto edge=e.sub(incoming);const int vertex=side?edge.vertex_b:edge.vertex_a;const auto adjacent=e.vertex(vertex).subcurves;
        if(adjacent.empty())throw std::invalid_argument("Scratch contour has an empty incident fan");int next;bool direction;
        if(adjacent.size()==1){next=model;direction=!side;}
        else{const int outgoing=adjacent.size()==2?(adjacent.front()==incoming?adjacent.back():adjacent.front()):vector_fill_detail::boundary_predecessor(e.a,vertex,incoming,e.budget);const auto span=e.sub(outgoing);next=span.model;direction=outgoing==e.model(next).subcurves.front()&&span.vertex_a==vertex;
            if(e.model(next).flags&0x02000000u){const int old=next;const auto found=repair.redirected_models.find(old);if(found==repair.redirected_models.end())throw std::invalid_argument("Scratch redirected model has no native representative");next=found->second;if(start_vertex(e,old)!=start_vertex(e,next))direction=!direction;}}
        if(e.model(next).flags&visited_mask(direction))break;model=next;side=direction;
    }
    group.positive=vector_fill_detail::temporary_orientation(e.a,group.members,e.budget);
    for(auto member:group.members){const auto& m=e.model(member.model);vector_graph_detail::unite(group.bounds,{m.scalars[0],m.scalars[1],m.scalars[2],m.scalars[3]});}
    return group;
}
std::vector<Group> discover(Editor& e,const std::vector<int>& lines,const RepairState& repair){
    std::vector<Group> groups;
    //5C1170/5BDAC0 restarts the first-unvisited search within each LC.
    //Fresh contiguous LC identity order is the supported72-byte stride domain.
    for(int line:lines)for(;;){e.budget.charge();std::optional<VectorLineMember> seed;for(auto member:e.line(line).members){const auto flags=e.model(member.model).flags;if(!(flags&0x13000000u)){seed=VectorLineMember{member.model,1};break;}if(!(flags&0x23000000u)){seed=VectorLineMember{member.model,0};break;}}if(!seed)break;groups.push_back(walk(e,seed->model,seed->selector!=0,repair));}
    //5C3DD0 visits representative values in the modifier's key order.
    for(auto [old,next]:repair.redirected_models){(void)old;for(bool side:{true,false})if(!(e.model(next).flags&visited_mask(side)))groups.push_back(walk(e,next,side,repair));}
    return groups;
}
std::int32_t extent_key(VectorBounds box){return std::bit_cast<std::int32_t>((std::uint32_t(box.right)-std::uint32_t(box.left))+(std::uint32_t(box.bottom)-std::uint32_t(box.top)));}
void seed_domain(Editor& e,const std::vector<int>& lines,const std::map<int,std::size_t>& planes){
    for(std::size_t i=0;i<lines.size();++i){e.budget.charge();if(lines[i]<0||std::int64_t(lines[i])!=std::int64_t(lines.front())+std::int64_t(i)||(lines[i]>>12)!=(lines.front()>>12)||!planes.contains(lines[i]))throw std::invalid_argument("Scratch LC seed walk requires contiguous records within one native page");(void)e.line(lines[i]);}
}
RegionConversion emit_groups(Integrator& target,const std::vector<Group>& groups){
    auto& e=target.geometry();RegionConversion result;result.regions.push_back(target.create_region());
    for(const auto& group:groups)if(group.positive){const int region=target.create_region();target.populate_loop(true,region,group.members);result.regions.push_back(region);vector_graph_detail::unite(result.context_dirty,group.bounds);}
    std::vector<std::size_t> negative;for(std::size_t i=0;i<groups.size();++i)if(!groups[i].positive)negative.push_back(i);
    std::ranges::sort(negative,[&](std::size_t a,std::size_t b){const auto ka=extent_key(groups[a].bounds),kb=extent_key(groups[b].bounds);return ka!=kb?ka>kb:a>b;});
    for(auto index:negative){e.budget.charge();const auto& group=groups[index];const auto& point=e.vertex(start_vertex(e,group.members.front().model));for(int region:result.regions)if(vector_fill_detail::region_contains(target.paint(),region,{point.x,point.y},e.budget,true)){const int loop=target.create_hole(region);target.populate_loop(false,loop,group.members);vector_graph_detail::unite(result.context_dirty,group.bounds);break;}}
    return result;
}
struct TemporaryLines {
    struct Slot {int previous{-1},next{-1};};
    Editor& e;TemporaryLineCleanup state;std::map<int,Slot> slots;
    TemporaryLines(Editor& editor,const std::vector<int>& lines,const std::map<int,std::size_t>& planes):e(editor){
        if(lines.empty())return;vector_material_detail::charge(e.storage,96*lines.size()+256,vector_material_detail::max_bytes,"Scratch temporary LC slot storage limit");state.plane=planes.at(lines.front());
        for(int id:lines)if(planes.at(id)!=state.plane)throw std::invalid_argument("Scratch LC array requires one actual Line plane");
        vector_graph_detail::OwnerLists owner(e.layer.planes.at(state.plane),e.paint);
        if(owner.lines!=lines||e.owner.lines!=lines)throw std::invalid_argument("Scratch LC array requires its complete actual Line-plane list");
        state.count=int(lines.size());synchronize();
    }
    void synchronize(){
        const auto& list=e.owner.lines;state.head=list.empty()?-1:list.front();state.tail=list.empty()?-1:list.back();
        for(std::size_t i=0;i<list.size();++i){e.budget.charge();slots[list[i]]={i?list[i-1]:-1,i+1<list.size()?list[i+1]:-1};}
    }
    void detached(const vector_line_detail::LineDetachResult& result){
        vector_material_detail::charge(e.storage,128,vector_material_detail::max_bytes,"Scratch temporary LC replacement storage limit");
        if(result.source_released){--state.count;state.released_sources.push_back(result.source_line);}
        if(result.created_line>=0){++state.count;if(slots.contains(result.created_line))state.reused_slots.push_back(result.created_line);slots[result.created_line]={};}
        //Released LC slots keep their last next/previous fields. A later
        //545770 allocation at the same address replaces that tombstone.
        synchronize();
    }
    TemporaryLineCleanup release(std::vector<int>& lines){
        vector_material_detail::charge(e.storage,(sizeof(TemporaryLineRelease)+sizeof(int))*lines.size()+256,vector_material_detail::max_bytes,"Scratch temporary LC cleanup storage limit");state.array=lines;
        for(int id:lines){e.budget.charge(128);const auto link=slots.at(id);const auto found=std::ranges::find(e.a.lines,id,&VectorPoolRecord<VectorLine>::index);const bool live=found!=e.a.lines.end();int members{};
            if(live){const auto source=found->value.members;members=int(source.size());if(e.refs<source.size()+1)throw std::invalid_argument("Scratch LC soft release has no live member/owner accounting");for(auto member:source){e.budget.charge();e.model(member.model).line=-1;}e.refs-=source.size()+1;found->value.members.clear();found->value.archived_bounds={};Editor::remove(e.owner.lines,id);}
            //5C9570 does not test occupancy: every original pointer and
            //appended suffix aliases the current72-byte slot contents.
            if(link.previous<0)state.head=link.next;else slots.at(link.previous).next=link.next;
            if(link.next<0)state.tail=link.previous;else slots.at(link.next).previous=link.previous;
            --state.count;
            if(live)e.erase(e.a.lines,5,id);
            else{auto& pool=e.allocator(e.a.lines,5);pool.live.erase(id);if(pool.cursor>id){pool.cursor=id;if(pool.high==id)--pool.high;}e.publish(e.a.lines,5);}
            const auto& pool=*e.slots[5];state.releases.push_back({id,link.previous,link.next,members,state.count,pool.cursor,pool.high,live});
        }
        if(!e.owner.lines.empty())throw std::invalid_argument("Scratch temporary cleanup left a live LC");
        if(!lines.empty()){
            const auto& pool=*e.slots[5];state.cursor=pool.cursor;state.high=pool.high;
            state.native_archive_would_fail=state.count!=0||state.head>=0||state.tail>=0;
            //CEL owner lists encode live identities, not a cached native DLL
            //count. Keep the exact ephemeral header in the result; serialize
            //the modern source-copy carrier from the now-empty live list.
            vector_graph_detail::OwnerLists owner(e.layer.planes.at(state.plane),e.paint);owner.lines.clear();owner.publish(e.layer.planes.at(state.plane),e.paint);
        }
        lines.clear();return state;
    }
};
}
RegionConversion materialize_samples(Integrator& target,std::vector<int>& lines,const std::map<int,std::size_t>& line_planes,std::size_t paint_plane,std::uint64_t color,bool create_faces){
    auto& e=target.geometry();e.budget.charge();
    if(paint_plane>=e.layer.planes.size()||(e.layer.planes[paint_plane].family!=0x22100001&&e.layer.planes[paint_plane].family!=0x22100002))throw std::invalid_argument("Scratch output requires a Paint plane");
    seed_domain(e,lines,line_planes);
    TemporaryLines temporary(e,lines,line_planes);
    auto repair=normalize(target,lines,line_planes);
    auto result=emit_groups(target,discover(e,lines,repair));const auto emitted=result.regions;
    auto live_line_planes=line_planes;
    //5C3DD0 releases classified geometry after installing its representative
    //loops. New suffix LCs enter the pointer array at its tail, independently
    //of their insert-before position in the actual Line-plane owner.
    for(auto [old,next]:repair.redirected_models){(void)next;e.budget.charge();const auto detached=vector_line_detail::detach_model_from_line(e,old,live_line_planes,&e.owner.lines);
        if(!detached.detached)throw std::invalid_argument("Scratch classified model has no source LC member");
        temporary.detached(detached);
        if(detached.created_line>=0)lines.push_back(detached.created_line);
        target.erase_model_after_repair(old);
    }
    for(int region:emitted){const auto& outer=e.value(target.paint().regions,region).outer;if(outer.members.empty())continue;const auto first=outer.members.front();const auto& model=e.model(first.model);if(model.line<0)continue;const auto& source=e.line(model.line).members;const auto member=std::ranges::find(source,first.model,&VectorLineMember::model);if(member==source.end())throw std::invalid_argument("Scratch outer model is absent from its source LC");if(member->selector==first.selector)result.selected_regions.push_back(region);}
    for(int id:e.owner.models)e.model(id).flags&=0xccffffffu;
    target.merge_exteriors_once(result.context_dirty);
    auto cleanup=temporary.release(lines);if(!cleanup.array.empty())result.temporary_lines=std::move(cleanup);
    if(create_faces)for(int region:result.selected_regions)result.faces.push_back(target.create_face(paint_plane,region,{region,1,color,-1}));
    result.regions=target.paint().border.regions;target.publish();e.budget.charge();return result;
}
RegionConversion materialize_retained_lines(Integrator& target,std::vector<int>& lines,std::map<int,std::size_t>& line_planes){
    auto& e=target.geometry();e.budget.charge();seed_domain(e,lines,line_planes);
    auto repair=normalize(target,lines,line_planes);auto result=emit_groups(target,discover(e,lines,repair));
    for(auto [old,next]:repair.redirected_models){(void)next;e.budget.charge();const auto detached=vector_line_detail::detach_model_from_line(e,old,line_planes);
        if(!detached.detached)throw std::invalid_argument("Scratch classified model has no source LC member");
        if(detached.created_line>=0)lines.push_back(detached.created_line);
        target.erase_model_after_repair(old);
    }
    //5C6900 clears all three modifier trees;643E20 then clears temporary
    //marker lanes on the complete destination Border model chain.
    for(int id:e.owner.models)e.model(id).flags&=0xccffffffu;
    target.publish();e.budget.charge();return result;
}
}
