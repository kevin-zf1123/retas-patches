#pragma once
#include "retas/vector_line_selection.hpp"
#include "vector_probe.hpp"

namespace retas::vector_line_selection_detail {
// Borrow an already captured state3/4 bitmap for the duration of a command.
// Native5C76E0 uses the actual bitmap DPI and floors the translated origin.
vector_probe_detail::Scope borrow_mask_scope(const RegionSelection&,double document_dpi,
    std::array<int,2> target_origin,vector_probe_detail::Budget);
// Bind once per private candidate. Scope borrows the public packet's deep mask;
// keep the packet alive throughout query/mutation. Both ledgers remain unchanged
// on failure.5C77E0 rectangle uses16*(selectionOrigin-targetOrigin);5C76E0
// masked offset instead usesfloor(deltaOrigin*forward), forward=(doc/maskDPI)*16.
vector_probe_detail::Scope reserve_line_selection_scope(const FrozenVectorLineSelection&,
    std::array<int,2> target_origin,std::uint64_t& storage,std::uint64_t& work,
    std::stop_token stop={});
}
