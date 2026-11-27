#pragma once
#include "vector_boundary_intersections.hpp"
#include <memory>
namespace retas::vector_boundary_detail {
//5A1D50/59D710 uses floor only for the saved Loop box gate. Ray roots use
//double query+.01 and comparisons use double curve positions at storedF32 t.
bool region_contains_double(const VectorPaintArchive&,int region,VectorPoint,vector_probe_detail::Budget);
// One destination owner and one Editor live across copy/intersection/commit.
// Geometry slots0..5 belong to Editor; this object owns paint slots6..11 and
// intrusive paint-plane lists. The caller publishes its private Stage only
// after the complete pipeline succeeds.
class Integrator {
    struct State;std::unique_ptr<State> state_;
public:
    explicit Integrator(vector_graph_detail::Editor&);
    ~Integrator();
    Integrator(const Integrator&)=delete;Integrator& operator=(const Integrator&)=delete;
    const std::map<int,std::size_t>& face_planes() const;
    std::map<int,std::size_t> line_planes();
    VectorBounds cache_loop(bool embedded,int loop_or_region);
    bool retire_empty_hole(int loop);
    vector_graph_detail::Editor& geometry();
    VectorPaintArchive& paint();
    int create_region();
    int create_hole(int region);
    void populate_loop(bool embedded,int loop_or_region,const std::vector<VectorLineMember>&);
    int create_face(std::size_t plane,int region,VectorArchiveFace);
    // Pools9/10/11: GradNode/Gradation/Texture. Only10/11 append Paint owners.
    int create_material(unsigned pool,std::size_t plane,Bytes);
    //598F70 replacement: preserve nonmaterial flags; last old consumer deep
    //release precedes new backlink. Allocator/list caches remain synchronized.
    void replace_face_gradation(int face,int gradation);
    void replace_face_texture(int face,int texture);
    //53F910/5EDA20: first-backlink detach, last-consumer ordered deep release;
    //caller supplies incoming alpha. Only exact solid1 with alpha belowFFFF
    //preserves old alpha when requested. Native performed/history are caller gates.
    bool replace_face_solid(int face,std::uint64_t color,bool preserve_alpha=false);
    //5A2260: remove actual Paint and Region memberships, detach material and
    //release Face through this context's synchronized allocator/list caches.
    void erase_face(int face);
    //Shape deep release owns the complete Region, holes and materials;
    //its geometry is separately released by the extension owner.
    void erase_extension_region(int region);
    // Whole/FaceMark zero targets still allocate then release the orphan in
    // forward stop order, retaining native live allocator H residue.
    void erase_orphan_gradation(std::size_t plane,int gradation);
    void publish();
    // Detaches any remaining LC member through5B59D0 using actual Line owners.
    // Old side Loops are retired by the caller; this deep release never
    // rebuilds their member chains or changes surviving side ownership.
    void erase_model_after_repair(int model);
    //5BF690/5BEEC0: complete missing side Loops; append a fresh exterior even
    //when no side is missing, followed by positive traced Regions.
    VectorBounds complete_missing_sides(const std::vector<int>& models,std::vector<int>& regions);
    //5C2410. Physical input/output occurrence order is retained; null selected
    //and null output pointers differ from present empty arrays. Consumes the
    //three repair work trees and clears temporary MC marker lanes.
    //Present frozen_color selects5BE350 full64 ClickColor material policy;
    //zero remains enabled. Geometry/dirty/materialization gates are unchanged.
    VectorBounds commit_regions(RepairState&,const std::vector<int>& primary,
        const std::vector<int>& affected,bool rerun_materializer,
        std::vector<int>* selected=nullptr,std::vector<int>* output=nullptr);
    VectorBounds commit_regions(RepairState&,const std::vector<int>& primary,
        const std::vector<int>& affected,bool rerun_materializer,
        std::vector<int>* selected,std::vector<int>* output,
        std::optional<std::uint64_t> frozen_color);
    //5C1E10 native pending pointer set, including related-Region suppression.
    // Entries originate from live caller arrays: subsequently retired slots
    // are empty no-ops; a currently live reused slot is materialized normally.
    // Returns only converter-context dirty; caller color damage is separate.
    // A nonnull collector appends the native ordered unique returned Regions;
    // related Regions suppress both pending and already-collected entries.
    // Existing collector prefixes are preserved. Clear opts into canonical
    // page-ID/slot order when CEL has no cross-page native address evidence.
    VectorBounds materialize_regions(const std::vector<int>& pending,
        std::vector<int>* collector=nullptr,bool canonical_page_order=false);
    //5D15F0 merges only the first two zero-outer-member Regions, then returns.
    bool merge_exteriors_once(VectorBounds& converter_dirty);
    //5BF9F0: follow connector side Loops directly, without intersection repair.
    //A rejected small positive contour leaves the caller's target cut intact.
    bool convert_raster_connector(int seed_region,int model,int raw_gap,
        std::vector<int>& live_regions,VectorBounds& converter_dirty);
    //Live intrusive successor; retired slots retain their last link.
    int region_successor(int region) const;
};
}
