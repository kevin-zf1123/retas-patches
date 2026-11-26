#pragma once
#include "retas/vector_face_archive.hpp"
#include "vector_probe.hpp"
namespace retas::vector_fill_detail {
//5A1CF0/59D4E0/59E950: original-span query with saved gates. An empty or
//inverted embedded outer gate selects the exterior, independently of the
//outer member-count gate used by the subsequent direct Face writer.
bool region_contains(const VectorPaintArchive&,int region,VectorFaceVertex,
    vector_probe_detail::Budget);
bool region_contains(const VectorPaintArchive&,int region,VectorFaceVertex,vector_probe_detail::Budget,bool exclude_origins);
// Plane ownership validation without rendering/material activation or planar
// certification. Geometry is consumed only by the actual Region query.
void validate_paint_owners(const VectorPaintArchive&,const Layer&,
    vector_probe_detail::Budget);
}
namespace retas::vector_fill_detail {
// Read-only5BEB40 and5BDC20 shared by native Region repair.
float boundary_heading(const VectorLineArchive&,int subcurve,int vertex,vector_probe_detail::Budget);
int boundary_predecessor(const VectorLineArchive&,int vertex,int subcurve,vector_probe_detail::Budget);
bool temporary_orientation(const VectorLineArchive&,const std::vector<VectorLineMember>&,vector_probe_detail::Budget);
struct MaterializedRegion {int region{-1};bool changed{};VectorBounds context_dirty;};
//5C14D0: ordered material-equivalent unoutlined-boundary dissolution. This
//owns all temporary marker lanes, native sparse pools, Face/Loop lifetime and
//post-walk endpoint normalization; caller dirty excludes context-only repair.
MaterializedRegion materialize_region(VectorPaintArchive&,Layer&,int seed,
    std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& references,
    std::uint64_t& storage,vector_probe_detail::Budget);
MaterializedRegion materialize_region(VectorPaintArchive&,Layer&,int seed,std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& references,std::uint64_t& storage,vector_probe_detail::Budget,std::vector<int>* related);
}
