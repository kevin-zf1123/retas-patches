#include "vector_line_detach.hpp"

namespace retas::vector_line_detail {
namespace {
using vector_graph_detail::Editor;
// Parse only the Line list: pure Line planes share Border geometry, so using
// OwnerLists::publish here would overwrite staged Border vertices/models.
struct LineOwner {
    bool vertex_plane{};
    std::vector<int> lines;
    Bytes topology,line_tail,tail;
    LineOwner(Editor& e,const Plane& p){
        e.budget.charge(8*p.specific.size()+32);
        vector_material_detail::charge(e.storage,4*p.specific.size()+256,
            vector_material_detail::max_bytes,"Vector LC detach owner storage limit");
        vertex_plane=p.family==0x21210001||p.family==0x21220001;
        if(!vertex_plane&&p.family!=0x21110001&&p.family!=0x21120001)throw std::invalid_argument("Vector LC detach requires its actual Line-plane owner");
        Reader r(p.specific);
        if(vertex_plane){auto t=r.section();topology=t.rest();auto l=r.section();lines=vector_graph_detail::OwnerLists::read(l);line_tail=l.rest();tail=r.rest();}
        else{lines=vector_graph_detail::OwnerLists::read(r);tail=r.rest();}
    }
    void publish(Plane& p)const{
        Writer w;
        if(vertex_plane){Writer l;vector_graph_detail::OwnerLists::write(l,lines);l.raw(line_tail);w.section(topology);w.section(l.data);}
        else vector_graph_detail::OwnerLists::write(w,lines);
        w.raw(tail);p.specific=std::move(w.data);
    }
};
}
LineDetachResult detach_model_from_line(Editor& e,int model_id,std::map<int,std::size_t>& planes,std::vector<int>* mirror){
    // Codec-valid same-LC repeated pointer nodes do not have a recovered
    // safe persistent detach strategy: removing the target leaves another
    // backlink, and moving a repeated peer across the split gives two parents.
    // Bound/validate this modern admission before any Editor/allocator/counter
    // mutation. Rejected candidates retain even their budget/cursor state.
    auto admission_work=e.budget.work,admission_storage=e.storage;
    const vector_probe_detail::Budget admission{admission_work,e.budget.stop};
    admission.charge(e.a.models.size());
    const int source=vector_graph_detail::record(e.a.models,model_id).value.line;
    LineDetachResult result;result.source_line=source;if(source<0)return result;
    admission.charge(e.a.lines.size());
    const auto& candidate=vector_graph_detail::record(e.a.lines,source).value;
    if(candidate.members.size()>8192)throw std::length_error("Vector LC detach member count limit");
    vector_material_detail::charge(admission_storage,64*candidate.members.size()+256,
        vector_material_detail::max_bytes,"Vector LC detach admission storage limit");
    {
        std::set<int> unique;
        for(const auto member:candidate.members){admission.charge(32);if(!unique.insert(member.model).second)throw std::invalid_argument("Vector LC detach repeated MC members are unsupported");}
    }
    e.budget.work=admission_work;e.storage=admission_storage;
    const auto& saved=e.line(source);
    if(saved.members.size()>8192)throw std::length_error("Vector LC detach member count limit");
    e.budget.charge(4*saved.members.size()+1);
    vector_material_detail::charge(e.storage,32*saved.members.size()+256,
        vector_material_detail::max_bytes,"Vector LC detach member storage limit");
    const auto original=saved;
    if(original.members.empty())throw std::invalid_argument("Vector LC detach requires a nonempty source LC");
    //59BB10 searches the first pointer occurrence; a missing MC is a native
    // null return with no detach/cache write, distinct from a released LC.
    const auto found=std::ranges::find(original.members,model_id,&VectorLineMember::model);
    if(found==original.members.end())return result;
    const auto owner_at=planes.find(source);
    if(owner_at==planes.end()||owner_at->second>=e.layer.planes.size())throw std::invalid_argument("Vector LC detach has no actual Line-plane owner");
    auto& plane=e.layer.planes[owner_at->second];LineOwner owner(e,plane);
    e.budget.charge(4*owner.lines.size()+1);
    const auto owner_member=std::ranges::find(owner.lines,source);
    if(owner_member==owner.lines.end()||std::ranges::count(owner.lines,source)!=1)throw std::invalid_argument("Vector LC detach has a dead or competing Line-plane membership");
    if(mirror&&*mirror!=owner.lines)throw std::invalid_argument("Vector LC detach mirrored Line-plane list is stale");
    const auto position=std::size_t(found-original.members.begin());const bool only=original.members.size()==1;
    if(e.refs<(only?2u:1u)||(only&&e.nodes==0))throw std::invalid_argument("Vector LC detach accounting has no live member/owner");
    if(position!=0&&position+1!=original.members.size()){
        VectorLine fresh;fresh.half_width_bits=original.half_width_bits;fresh.color=original.color;
        const int created=e.create(e.a.lines,5,std::move(fresh));e.reference();
        e.line(created).members.assign(std::next(found),original.members.end());
        for(const auto member:e.line(created).members){e.budget.charge();e.model(member.model).line=created;}
        e.line(source).members.resize(position);
        owner.lines.insert(owner.lines.begin()+std::ptrdiff_t(owner_member-owner.lines.begin()),created);
        if(!planes.emplace(created,owner_at->second).second)throw std::invalid_argument("Vector LC detach new identity has a stale plane-map entry");result.created_line=created;
        e.model(model_id).line=-1;--e.refs;e.cache(source,false);e.cache(created,false);
    }else{
        e.line(source).members.erase(e.line(source).members.begin()+std::ptrdiff_t(position));e.model(model_id).line=-1;--e.refs;
        if(only){Editor::remove(owner.lines,source);--e.refs;e.erase(e.a.lines,5,source);planes.erase(source);result.source_released=true;}
        else e.cache(source,false);
    }
    e.topology_changed=true;result.detached=true;owner.publish(plane);if(mirror)*mirror=owner.lines;
    e.budget.charge();return result;
}
}
