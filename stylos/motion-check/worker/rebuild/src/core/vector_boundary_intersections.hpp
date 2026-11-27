#pragma once
#include "vector_graph_edit.hpp"

namespace retas::vector_boundary_detail {
struct ModelLocationPair {
    int first_sub{-1},second_sub{-1};
    float first_parameter{},second_parameter{};
    bool operator==(const ModelLocationPair&) const = default;
};
//5BBE10 ->5BB1F0(mode0): paired raw LOCATION arrays in physical SC order.
// Same-MC comparisons start at the next SC and append the reciprocal pair.
// Equal-geometry and endpoint-nearest augmentation share the existing query
// core. No Vertex resolution, welding, allocation, splitting or publication.
std::vector<ModelLocationPair> collect_model_locations(
    vector_graph_detail::Editor&,int first_model,int second_model);
struct OrderedRegionPair {
    int first{-1},second{-1};
    std::vector<int> vertices;
    bool operator==(const OrderedRegionPair&) const = default;
};
// The three work trees consumed by5C2410. Pair order is significant: the
// first Region supplies material before the second Region. Vertex arrays in
// pairs/visited_vertices are pointer-ordered sets; global_vertices retains
// the physical repeated event visits used by5BC470/5B7C00.
struct RepairState {
    bool events_found{};
    std::vector<OrderedRegionPair> region_pairs;
    std::map<int,int> redirected_models;
    std::vector<int> visited_vertices,global_vertices;
    VectorBounds context_dirty;
};
//5D2A00 on an already copied, unified destination topology. The same Editor
// and sparse-pool cursors remain live through the subsequent Region commit.
// Input arrays retain occurrence order and duplicates. Line/Face maps contain
// physical owner-plane indices, not native pointer-order surrogates.
// Raw MC occurrences read the current slot cell: a split-released cell retains
// empty SC/CP heads until reuse, which then exposes the new live MC. Retired
// cells stay private to this phase and are never serialized into the archive.
// The caller owns a private Stage and publishes it only after the complete
// Region pipeline succeeds. Cross-page native pointer comparisons, and multiple
// Line-plane identities at one repaired Vertex, reject without publication.
RepairState run(vector_graph_detail::Editor&,
    const std::vector<int>& copied_models,const std::vector<int>& affected_models,
    std::vector<int>& affected_regions,const std::map<int,std::size_t>& line_planes,
    const std::map<int,std::size_t>& face_planes);
//5D38E0's single intersection phase: shared_tree=true uses one aliased
//event map for a self scan and splits only the source occurrence array.
//Otherwise the two maps remain distinct and both arrays are split in order.
//Both paths collect source-map Vertex occurrences AFTER the split pass, then
//rank redirects and repair those local Line endpoints. No Region pairs or
//fan fixpoint are created here; scratch Line topology needs no Paint extension.
RepairState intersect_pass(vector_graph_detail::Editor&,
    const std::vector<int>& source_models,const std::vector<int>& peer_models,
    bool shared_tree,const std::map<int,std::size_t>& line_planes,
    const std::map<int,std::size_t>& face_planes);
}
