#include "vector_line_paint_convert.hpp"
#include "vector_mask_region.hpp"

namespace retas::vector_line_paint_detail {
namespace {
using vector_boundary_detail::Integrator;
const VectorLine& source_line(const VectorLineArchive& source,int id,vector_probe_detail::Budget budget){
    budget.charge(source.lines.size()+1);const auto found=std::ranges::find(source.lines,id,&VectorPoolRecord<VectorLine>::index);
    if(found==source.lines.end())throw std::invalid_argument("Line-to-Paint source LC is not live");return found->value;
}
ConversionResult convert(Integrator& target,const VectorLineArchive& source,const Layer& owner,const std::vector<int>& lines,const ConversionSettings& settings,bool rerun){
    auto& e=target.geometry();e.budget.charge();if(&source==&e.a)throw std::invalid_argument("Line-to-Paint conversion requires a distinct immutable source");if(lines.size()>8192)throw std::length_error("Line-to-Paint input LC array exceeds8192");
    //5D0B50 queries the frozen UNION of original source LC caches before
    //allocating anything, independently of fresh geometry-derived caches.
    VectorBounds bounds;for(int id:lines){e.budget.charge();vector_graph_detail::unite(bounds,source_line(source,id,e.budget).archived_bounds);}
    bounds=vector_copy_detail::bounds(bounds,vector_copy_detail::transform(settings.translation,settings.matrix));ConversionResult result;result.context_dirty=bounds;result.affected=vector_boundary_detail::query_candidates(target,bounds);
    const auto cloned=vector_boundary_detail::copy_line_occurrences(e,source,owner,lines,settings.planes,settings.translation,settings.matrix);
    result.line_occurrences=cloned.copied_lines;
    auto line_planes=target.line_planes();
    const auto grouped=vector_mask_detail::materialize_retained_lines(target,result.line_occurrences,line_planes);
    result.new_regions=grouped.regions;
    //5C3DD0 clears the modifier work trees before the target pass. The
    //original LC array still has released entries:5459F0 cleared their
    //member heads. Reused identities are read anew for EVERY occurrence.
    if(!result.affected.regions.empty()){
        std::vector<int> copied_models;
        for(int id:result.line_occurrences){e.budget.charge();const auto live=std::ranges::find(e.a.lines,id,&VectorPoolRecord<VectorLine>::index);if(live==e.a.lines.end())continue;for(auto member:live->value.members){e.budget.charge();vector_material_detail::charge(e.storage,32,vector_material_detail::max_bytes,"Line-to-Paint MC array exceeds512MiB");if(copied_models.size()>=8192)throw std::length_error("Line-to-Paint MC array exceeds8192");copied_models.push_back(member.model);}}
        line_planes=target.line_planes();auto repair=vector_boundary_detail::run(e,copied_models,result.affected.models,result.affected.regions,line_planes,target.face_planes());
        vector_graph_detail::unite(result.context_dirty,target.commit_regions(repair,result.new_regions,result.affected.regions,rerun,nullptr,nullptr));
    }
    //5D4F10/5D52B0 do not union the standalone5C3DD0 context box. Only
    //5D0B50, optional5C2410 and5D15F0 contribute converter damage.
    target.merge_exteriors_once(result.context_dirty);target.publish();e.budget.charge();return result;
}
}
ConversionResult convert_lines(Integrator& target,const VectorLineArchive& source,const Layer& owner,const std::vector<int>& lines,const ConversionSettings& settings){return convert(target,source,owner,lines,settings,true);}
ConversionResult convert_line(Integrator& target,const VectorLineArchive& source,const Layer& owner,int line,const ConversionSettings& settings,bool rerun){
    if(source_line(source,line,target.geometry().budget).members.empty())return {};
    return convert(target,source,owner,{line},settings,rerun);
}
}
