#pragma once
#include "vector_boundary_source_copy.hpp"
#include "retas/selection.hpp"

namespace retas::vector_selection_detail {
// Immutable native208 Paint0-only indication. RegionSelection has neither
// physical offscreen DPI nor the state4 vector cache; those are explicit here.
// Preparing before tool geometry is a modern ownership choice: the native
// on-demand getter's shared WorkLayer reset prelude remains unproved.
class FrozenNativeSelection {
    struct Data;std::shared_ptr<const Data> data_;
    explicit FrozenNativeSelection(std::shared_ptr<const Data>);
    friend FrozenNativeSelection prepare(const RegionSelection&,const CommonElement&,
        double,std::uint64_t&,vector_probe_detail::Budget);
    friend FrozenNativeSelection freeze_cache(const VectorPaintArchive&,const Layer&,
        std::array<int,2>,std::uint64_t&,vector_probe_detail::Budget);
public:
    bool bypass()const;
    // False means616750 conversion produced no Region; native getter is NULL.
    // An explicitly frozen state4 cache stays available even with no Faces.
    bool available()const;
    const VectorPaintArchive& archive()const;
    const Layer& owner()const;
    std::array<int,2> origin()const;
    const std::vector<int>& selected_regions()const;
};
// States0/1 bypass. State2 is the un-clamped raw16 rectangle. State3 uses
// actual mask DPI and stored clip/active bits,32.f/200.f sampling. State4
// must enter through freeze_cache; no mask re-trace substitutes for it.
FrozenNativeSelection prepare(const RegionSelection&,const CommonElement& document,
    double actual_mask_dpi,std::uint64_t& storage,vector_probe_detail::Budget);
FrozenNativeSelection freeze_cache(const VectorPaintArchive&,const Layer& cached208,
    std::array<int,2> cached_origin,std::uint64_t& storage,vector_probe_detail::Budget);

struct GradationTransferResult { VectorBounds converter_dirty;std::vector<int> selected_regions; };
//5DB770 Point uses the same5D4440 all-copy with an empty PlaneMap,
//rerun0 and an explicit Region collector from its8/50 mask conversion.
GradationTransferResult transfer_regions(vector_boundary_detail::Integrator&,
    const VectorPaintArchive&,const Layer&,std::vector<int> selected_regions);
//5F3430->5D4440: ONE selection copy, EMPTY PlaneMap, NONNULL selected
//collector and rerun0. Unlike Shape/Polyline this does not copy to a scratch.
GradationTransferResult gradation_transfer(vector_boundary_detail::Integrator&,
    const FrozenNativeSelection&,std::array<int,2> target_origin);

struct TransferResult {
    bool storage_changed{};
    VectorBounds converter_dirty,first_copy_dirty,prepass_dirty;
    std::vector<int> selected_regions,prepass_regions;
};
//646740/6345B0 all-copy writeback, including mapped Pure siblings.
VectorBounds transfer_all(vector_boundary_detail::Integrator&,const VectorPaintArchive&,
    const Layer&,const std::map<std::size_t,std::size_t>&);
// Actual55C6F0/561040 fill-only route: all-selection geometry into private
// generated scratch with EMPTY PlaneMap/nonNULL selection; selected Region
// copy to target with5D4770 rerun1. PlaneMap admits only generated Paint
// children, matching the native sourcePlanes Paint0 packet. Line tool routes
// are not admitted here. The LINE/CURV553970 selected route instead enters
//543230/5DB5E0/5DB0D0(flags101);5D7160 is a separate Paint/helper route.
// Generated/cache inputs remain immutable; target must be a private candidate
// whose caller handles atomic publication and converter-dirty history gate.
TransferResult transfer(vector_boundary_detail::Integrator& target,
    const VectorPaintArchive& generated,const Layer& generated_owner,
    const FrozenNativeSelection&,std::array<int,2> generated_origin,
    const std::map<std::size_t,std::size_t>& paint_planes);
}
