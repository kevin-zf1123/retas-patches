#include "../core/vector_stored_line.hpp"
#include "retas/vector_color_metadata.hpp"
#include "vector_line_projection.hpp"
#include <algorithm>
#include <bit>
#include <map>
#include <stdexcept>

namespace retas::vector_render_detail {
namespace {
bool line_family(std::uint32_t f){return f==0x21110001||f==0x21120001||f==0x21210001||f==0x21220001;}
std::vector<StoredLinePlane> project(const Layer& layer,const VectorLineArchive& archive,bool paint){
    constexpr std::uint64_t max_bytes=512ull*1024*1024;
    std::uint64_t source=layer.specific.size();std::optional<std::size_t> first;
    for(std::size_t i=0;i<layer.planes.size();++i){const auto& p=layer.planes[i];
        if(p.opaque||p.raster||p.specific.size()>64U*1024U*1024U||
           (!line_family(p.family)&&!(paint&&(p.family==0x22100001||p.family==0x22100002))))
            throw std::invalid_argument("Unsupported stored Line sibling");
        if(line_family(p.family)&&!first)first=i;source+=p.specific.size();}
    if(source>max_bytes/16)throw std::length_error("Stored Line wire storage exceeds limits");
    // Validate the entire shared graph once, even if every LC reference list
    // is empty. Per-model projection otherwise sees only consumed endpoints
    // and could miss an unrelated live Vertex claiming another SC's edge.
    vector_archive_detail::validate_stored_line_attachments(archive);
    // One complete ownership walk also validates hidden/empty siblings and
    // Paint Border/Vert topology. Retain later plane occurrence order directly.
    if(first)(void)validated_vector_plane_line_references(layer,*first);
    std::map<int,const VectorLine*> lines;for(const auto& r:archive.lines)lines.emplace(r.index,&r.value);
    std::map<int,const VectorModelCurve*> models;for(const auto& r:archive.models)models.emplace(r.index,&r.value);
    std::map<int,const VectorSubcurve*> curves;for(const auto& r:archive.subcurves)curves.emplace(r.index,&r.value);
    std::uint64_t storage=source*16,work=source,spans{},nodes{},commands{};
    const std::uint64_t records=archive.vertices.size()+archive.scales.size()+archive.subcurves.size()+archive.models.size()+archive.crosspoints.size()+archive.lines.size();
    const auto indexing=records*(1+std::bit_width(records))*8;
    const auto charge=[](std::uint64_t& n,std::uint64_t more,std::uint64_t limit){if(more>limit-n)throw std::length_error("Stored Line projected source exceeds limits");n+=more;};
    std::vector<StoredLinePlane> result(layer.planes.size());
    for(std::size_t i=0;i<result.size();++i){auto& p=result[i];p.color=layer.planes[i].common.color;if(!line_family(layer.planes[i].family))continue;const auto refs=vector_line_plane_references(layer.planes[i]);
        if(refs.empty())continue;
        charge(work,indexing,128ull*1024*1024);
        charge(commands,refs.size(),100000);charge(storage,refs.size()*(sizeof(VectorLineStroke)+sizeof(VectorBounds)),max_bytes);
        std::uint64_t plane_spans{};bool has_profiles=false;
        for(auto id:refs){const auto& line=*lines.at(id);p.line_bounds.push_back(line.archived_bounds);
            for(const auto& member:line.members){const auto& model=*models.at(member.model);const auto& b=model.scalars;
                if(p.model_bounds.emplace(member.model,VectorBounds{b[4],b[5],b[6],b[7]}).second)charge(storage,64+sizeof(std::pair<const int,VectorBounds>),max_bytes);
                charge(spans,model.subcurves.size(),100000);plane_spans+=model.subcurves.size();charge(storage,model.subcurves.size()*sizeof(VectorLineSpan),max_bytes);
                has_profiles=has_profiles||model.crosspoints[0]>=0||model.crosspoints[1]>=0;
                for(auto sub:model.subcurves){const auto count=curves.at(sub)->scales.size();charge(nodes,count,100000);charge(storage,count*sizeof(VectorWidthNode),max_bytes);}}}
        if(has_profiles)charge(storage,plane_spans*sizeof(VectorSpanProfiles),max_bytes);
        p.lines.strokes=vector_archive_detail::project_line_strokes(archive,refs,vector_archive_detail::LineClosurePolicy::NativeStored,&p.line_profiles);
    }
    if(!result.empty()){result.front().reserved_storage=source*16;result.front().reserved_work=work;}
    return result;
}
} // namespace
std::vector<StoredLinePlane> decode_stored_line_planes(const Layer& layer){
    if(layer.opaque||(layer.family!=0x22100000&&layer.family!=0x24400000&&layer.family!=0x20000200)||layer.planes.size()>1024)
        throw std::invalid_argument("Unsupported stored Line owner");
    return project(layer,decode_vector_line_archive(layer),false);
}
std::vector<StoredLinePlane> project_stored_paint_lines(const Layer& layer,const VectorLineArchive& archive){
    if(layer.opaque||(layer.family!=0x21200000&&layer.family!=0x20800000)||layer.planes.size()>1024)
        throw std::invalid_argument("Unsupported stored Paint owner");
    return project(layer,archive,true);
}
}
