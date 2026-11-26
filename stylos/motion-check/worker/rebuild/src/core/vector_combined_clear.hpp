#pragma once
#include "vector_tool_selection.hpp"

namespace retas::vector_clear_detail {
struct CombinedClearResult {
    bool performed{};
    std::uint64_t erased_faces{},erased_lines{};
    VectorBounds changed{},context{};
};
//5D8B10/5D7160 partitions Pure LCs before the payload copy. Both selected
//and retained runs keep their original directed MCs in the live Border.
//Returns context+78 from original and replacement LC caches, independently
//of the subsequent deletion worker's BOOL result.
VectorBounds partition_capture_lines(vector_boundary_detail::Integrator&,
    const std::vector<std::size_t>&,const std::vector<int>& regions,bool marked,VectorBounds converter_dirty);
//Deletion half of5DB770. Uses the already converted selection and the same
//live context; no second208 conversion and exactly one final materializer.
CombinedClearResult delete_captured_pure_faces(vector_boundary_detail::Integrator&,
    const std::vector<std::size_t>&,std::vector<int> regions,bool full,bool spatial);
//66E730(flags300): a single private Border context spans208 conversion,
//5DA0B0 Face removal,5DA7E0 marked/full or5DA2F0 spatial Pure removal,
//and one final5C1E10. The caller owns capture, history and publication.
//native is required only for nonFull actual selection; an unavailable packet
//is the native NULL getter, distinct from an available empty state4 cache.
CombinedClearResult clear_combined_pure_faces(VectorPaintArchive&,Layer&,
    const std::vector<std::size_t>& selected_planes,SelectionLayerState,
    const vector_selection_detail::FrozenNativeSelection* native,
    std::uint32_t application_flags,std::uint64_t& wire,std::uint64_t& nodes,
    std::uint64_t& references,std::uint64_t& storage,std::uint64_t& work,
    std::stop_token);
}
