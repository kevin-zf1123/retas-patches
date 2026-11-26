#include "vector_grouped_face_delete.hpp"
#include <set>
namespace retas::vector_grouped_face_detail {
DeleteResult delete_region_faces(vector_boundary_detail::Integrator& context,
    const std::vector<int>& regions,const std::vector<std::size_t>& selected){
    auto& e=context.geometry();auto& a=context.paint();e.budget.charge();
    vector_material_detail::charge(e.storage,regions.size()*4+selected.size()*32,512ull*1024*1024,"Grouped Face input storage limit");
    std::set<std::size_t> chosen;std::size_t total{};
    for(const auto& p:e.layer.planes){e.budget.charge();if(p.family==0x22100001||p.family==0x22100002)++total;}
    for(auto id:selected){e.budget.charge();if(id>=e.layer.planes.size()||!chosen.insert(id).second)throw std::invalid_argument("Grouped Face selection requires unique live Paint children");const auto& p=e.layer.planes[id];if(p.opaque||(p.family!=0x22100001&&p.family!=0x22100002))throw std::invalid_argument("Grouped Face selected child is not Paint");}
    const bool all=chosen.size()==total;DeleteResult out;
    for(int id:regions){e.budget.charge();const auto& region=e.value(a.regions,id);const auto bounds=region.outer.archived_bounds;
        vector_material_detail::charge(e.storage,region.faces.size()*4,512ull*1024*1024,"Grouped Face occurrence storage limit");const auto faces=region.faces;bool erased{};
        for(int face:faces){e.budget.charge();const auto plane=context.face_planes().at(face);if(!all&&!chosen.contains(plane))continue;context.erase_face(face);++out.erased_faces;erased=true;}
        if(erased){out.storage_changed=true;vector_graph_detail::unite(out.immediate_dirty,bounds);}
    }
    return out;
}
}
