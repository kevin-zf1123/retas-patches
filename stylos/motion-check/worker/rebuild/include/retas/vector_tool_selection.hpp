#pragma once
#include "selection.hpp"
#include <stop_token>

namespace retas {
struct CommonElement;
struct Layer;
namespace vector_selection_detail { struct BindingAccess; }

// Owned, immutable native208 Paint0 selection graph. Copies share the graph;
// the input mask's bits and allocated-zero blocks are independently frozen.
// This packet does not promote Document selection state or change history.
class FrozenVectorToolSelection {
    struct State;
    std::shared_ptr<const State> state_;
    explicit FrozenVectorToolSelection(std::shared_ptr<const State>);
    friend struct vector_selection_detail::BindingAccess;
    friend FrozenVectorToolSelection prepare_vector_tool_selection(
        const RegionSelection&,const CommonElement&,double,std::stop_token);
    friend FrozenVectorToolSelection prepare_gradation_vector_tool_selection(
        const RegionSelection&,const CommonElement&,double,std::stop_token);
    friend FrozenVectorToolSelection freeze_cached_vector_tool_selection(
        const Layer&,std::array<int,2>,const RegionSelection&,std::stop_token);
public:
    FrozenVectorToolSelection(const FrozenVectorToolSelection&)=default;
    FrozenVectorToolSelection& operator=(const FrozenVectorToolSelection&)=default;
    // The mask here is a deep snapshot, so its identity differs from input.
    const RegionSelection& source_snapshot() const noexcept;
    // Checks captured metadata and original mask identity, like session
    // RegionSelection equality. Document selection generation is checked by
    // the session separately. No raster predicate substitutes for state4.
    bool matches_source(const RegionSelection&) const noexcept;
    bool bypass() const noexcept;
    bool available() const noexcept;
};

// States0/1 bypass; state2 prepares the un-clamped raw16 rectangle; state3
// uses actual offscreen DPI and native32/200 contour sampling. A normal1x
// carrier uses document.resolution;4x uses4*document.resolution. State4 must
// use the cache factory below. Empty state2/3 conversion is unavailable.
// Invalid inputs, resource exhaustion and cancellation throw before return;
// the caller's selection/document are never mutated. Factory and downstream
// tool work use bounded512MiB storage/128Mi work ledgers. Tool implementations
// reserve this packet's preparation costs in their shared operation ledger.
FrozenVectorToolSelection prepare_vector_tool_selection(const RegionSelection&,
    const CommonElement& document,double actual_mask_dpi,std::stop_token stop={});

// GRAD616950 differs from Shape/Polyline: state1 prepares the complete
// physical carrier rectangle and retains the ORIGINAL state1 witness. State0
// bypasses;2/3 match ordinary preparation;4 still requires an actual cache.
FrozenVectorToolSelection prepare_gradation_vector_tool_selection(const RegionSelection&,
    const CommonElement& document,double actual_mask_dpi,std::stop_token stop={});

// Requires source.state==VectorIndicated and an actual native208 Paint0-only
// black indication cache, with16x geometry DPI. Freezes its full typed graph,
// including live sparse allocator H. It never rebuilds state4 from mask bits.
// A valid empty cache remains available, matching the nonNULL native getter.
FrozenVectorToolSelection freeze_cached_vector_tool_selection(const Layer& actual208,
    std::array<int,2> cached_origin,const RegionSelection& source,
    std::stop_token stop={});
//Publish an owned state4 cache witness while retaining the separate bitmap
//carrier. The caller can select_region() it as a normal undoable command.
RegionSelection vector_indicated_selection(const Layer& actual208,
    std::array<int,2> cached_origin,RegionSelection source,std::stop_token stop={});
}
