#pragma once
#include "vector_boundary_integrate.hpp"

namespace retas::vector_eraser_detail {
struct PointSelectionResult;
PointSelectionResult extract_border_point_selection(vector_boundary_detail::Integrator&,std::size_t,int,const vector_probe_detail::Scope&);
//5D85B0 create1: preserve selected and retained marked-MC runs; first run
//appends and later runs insert before the original LC. Collect source Regions
//for the caller's single final materializer after selected LC soft retirement.
PointSelectionResult extract_border_marked_trim(vector_boundary_detail::Integrator&,std::size_t,int);
struct SplitReleaseResult {
    std::vector<int> processed_lines,new_lines;
    VectorBounds immediate_dirty{},context_dirty{};
};
struct PairedPointResult {
    std::vector<int> source_lines,destination_lines;
    VectorBounds immediate_dirty{},context_dirty{},replacement_dirty{};
};
//5D74F0 with a non-null destination Vert-Line owner. Outside runs remain
// in source; selected runs clone into destination (arg_C complements this
// predicate). Both real owners append fresh styled LCs and directed MCs.
// The source materializer runs against source+20, then the original LC is
// deeply released. Distinct archives, real2121/2122 children, same DPI and
// shared storage/work ledgers are required. Caller discards both candidates
// after an error; this leaf encodes neither archive nor public transaction.
PairedPointResult replace_point_pair(vector_graph_detail::Editor& source,
    std::size_t source_plane,int source_line,
    vector_graph_detail::Editor& destination,std::size_t destination_plane,
    const vector_probe_detail::Scope&,bool complement=false,bool trim=false);
void retain_point_selection(vector_graph_detail::Editor&,std::size_t,int,const vector_probe_detail::Scope&);
struct PointSelectionResult {
    std::vector<int> source_lines,selected_lines,pending_regions;
    VectorBounds immediate_dirty{},context_dirty{};
};
//5D7E90 create1 on an ordinary temporary Vert owner: selected split MCs
// transfer to styled LCs in the same owner, retaining member selectors and
// geometry identities. No Region is manufactured or repaired. The selected
// LC occurrence array is consumed by the subsequent Line→Paint converter;
// its later option0 soft LC retirement remains the caller's responsibility.
PointSelectionResult extract_point_selection(vector_graph_detail::Editor&,
    std::size_t plane,int source_line,const vector_probe_detail::Scope&);
// Native5DA9F0 low seeds0x01/0x12 on one actual active Line owner.
// Border callers retain live side topology until pending Region repair.
// Vert-Line callers clone retained directed runs before retiring the source.
// The caller owns a private candidate and must discard it after an error.
SplitReleaseResult erase_point_trim(vector_graph_detail::Editor&,
    vector_boundary_detail::Integrator*,std::size_t plane,
    const std::vector<int>& original_lines,const vector_probe_detail::Scope&,
    int mode);
}
