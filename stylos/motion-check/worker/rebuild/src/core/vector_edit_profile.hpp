#pragma once
#include "retas/vector_geometry.hpp"
#include <map>
namespace retas::vector_edit_detail {
void incremental_subcurve(VectorLineArchive&,Layer&,int subcurve,
    std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& references,
    std::uint64_t& storage,std::uint64_t& work,std::stop_token);
//5B7760 retained endpoint reconstruction and59E460 incremental saved gates.
// This edits only the selected CP, flags and the selected SC's cache union.
void partition_endpoint(VectorLineArchive&,Layer&,int model,bool at_start,bool cache_at_start,std::uint32_t half_width,
    std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& references,std::uint64_t& storage,std::uint64_t& work,std::stop_token);
//5B7D70: visit logical Line endpoints in saved member order (with repeats),
// rebuild same-plane angle fans, then adjust retained first/last descriptors.
// Neighbor Lines receive incremental cache unions; the caller performs the
// selected Line's subsequent59B840(1) full cache rebuild.
void rebuild_line_endpoints(VectorLineArchive&,Layer&,int line,
    const std::map<int,std::size_t>& line_planes,std::uint64_t& wire,
    std::uint64_t& nodes,std::uint64_t& references,std::uint64_t& storage,
    std::uint64_t& work,std::stop_token);
// One native5B6E90 Vertex visit; preserves the supplied Line-to-plane map.
// This does not visit other endpoints or apply retained endpoint adjustment.
void repair_vertex_endpoints(VectorLineArchive&,Layer&,int vertex,std::size_t plane,
    const std::map<int,std::size_t>& line_planes,std::uint64_t& wire,
    std::uint64_t& nodes,std::uint64_t& references,std::uint64_t& storage,
    std::uint64_t& work,std::stop_token);
//5B49E0 explicit cut hints and5B4E30 retained-only adjustment. Neither
// rewrites peer endpoint fans or MC/LC cache fields.
void cut_endpoint(VectorLineArchive&,Layer&,int model,bool at_start,
    std::array<VectorPoint,2> hints,std::uint64_t& wire,std::uint64_t& nodes,
    std::uint64_t& references,std::uint64_t& storage,std::uint64_t& work,std::stop_token);
void retained_endpoint(VectorLineArchive&,Layer&,int model,bool at_start,
    std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& references,
    std::uint64_t& storage,std::uint64_t& work,std::stop_token);
}
