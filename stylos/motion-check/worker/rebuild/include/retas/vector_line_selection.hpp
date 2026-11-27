#pragma once
#include "selection.hpp"
#include <stop_token>

namespace retas {
namespace vector_line_selection_detail { struct CarrierAccess; }
// Immutable actual raster indication for543230/5C77E0 LINE/CURV clipping.
// State4 uses its raster backing here, independently of Paint's vector cache.
// Copies share owned mask bits and allocated-zero block state.
class FrozenVectorLineSelection {
    struct State;std::shared_ptr<const State> state_;
    explicit FrozenVectorLineSelection(std::shared_ptr<const State>);
    friend struct vector_line_selection_detail::CarrierAccess;
    friend FrozenVectorLineSelection prepare_vector_line_selection(
        const RegionSelection&,double,double,std::stop_token);
public:
    FrozenVectorLineSelection(const FrozenVectorLineSelection&)=default;
    FrozenVectorLineSelection& operator=(const FrozenVectorLineSelection&)=default;
    const RegionSelection& source_snapshot() const noexcept;
    // Original input mask identity plus captured metadata and document DPI.
    // Session must additionally check Document's selection generation.
    bool matches_source(const RegionSelection&,double selection_document_dpi) const noexcept;
    bool bypass() const noexcept;
    double selection_document_dpi() const noexcept;
    double actual_mask_dpi() const noexcept;
};
// Explicit physical carrier factory. States0/1 bypass; state2 retains the
// un-clamped rectangle; state3/4 require actual mask storage and offscreen DPI.
// A default Session may prepare states0..3 using actual_mask_dpi==document DPI
// for its actual1x carrier. State4 and other densities require this explicit
// packet. No vector-cache reconstruction or contour sampling occurs.
// Preparation freezes real mask/extent and native forward/inverse mapping.
// It changes no Document, selection state or history. Invalid/resource/cancel
// errors throw before return; bounded storage/work costs join the consuming
// tool's ledgers before scope use. Input mask is immutable during this call.
FrozenVectorLineSelection prepare_vector_line_selection(const RegionSelection&,
    double selection_document_dpi,double actual_mask_dpi,std::stop_token stop={});
}
