#pragma once
#include "vector_boundary_integrate.hpp"
namespace retas::vector_grouped_face_detail {
struct DeleteResult {
    std::uint64_t erased_faces{};
    bool storage_changed{};
    VectorBounds immediate_dirty; //5DA0B0 direct saved embedded outer bounds.
};
//5DA0B0 DELETE ONLY. Inputs are actual owner Region occurrence order and
//unique actual selected Paint child indices. ALL selected compares against
//every owner Paint child, including hidden/unmarked children. Face marks are
//never a deletion filter: each selected-plane Face of every input Region is
//visited in its physical Region list order. Caller must materialize the
//ORIGINAL ordered Region IDs afterwards, then publish its private context.
DeleteResult delete_region_faces(vector_boundary_detail::Integrator&,
    const std::vector<int>& ordered_regions,
    const std::vector<std::size_t>& selected_paint_planes);
}
