#pragma once
#include "vector_boundary_integrate.hpp"
#include "vector_copy_coordinates.hpp"

namespace retas::vector_boundary_detail {
struct SourceCopySettings {
    // CPlane identities are represented by physical child indices. Missing
    // keys skip Line/Face allocation. Only pure Line and Paint aliases enter
    // this Border-owned copy path.
    std::map<std::size_t,std::size_t> planes;
    std::array<std::int32_t,2> translation{}; // Raw integer units; no resampling.
    // Null selects5D2160/5D2230, including every source Border Region.
    // A present array selects5D2570/5D27D0 and retains occurrence order.
    std::optional<std::vector<int>> regions;
    bool filtered{}; //5D26C0/5D1FA0; requires a present Region array.
    //5D2230's optional external selection does not limit copied Regions.
    std::optional<std::vector<int>> selected;
    std::optional<vector_copy_detail::Matrix> matrix;
};
struct SourceCopyResult {
    std::map<int,int> vertices,models,subcurves,lines,gradations,textures;
    // CPs clone per endpoint; width/stop/standalone Loop nodes clone per
    // native occurrence, so their source identities may map more than once.
    std::multimap<int,int> scales,crosspoints,loops,gradnodes;
    std::vector<int> copied_models,new_regions,selected_regions; // Creation order.
    std::vector<std::pair<int,int>> region_clones,face_clones;
    VectorBounds line_dirty;
};
struct LineSourceCopyResult:SourceCopyResult {
    // First-created maps above remain stable representatives; these lists
    // retain directed-copy occurrences including repeated input LC/MC/SCs.
    // vertex_clones also records carried endpoints reused across LC members.
    std::multimap<int,int> vertex_clones,model_clones,subcurve_clones,line_clones;
    std::vector<int> copied_lines;
    std::map<int,std::size_t> line_planes;
};
// Native copy primitives5D16B0/5D17A0/5D1C10/5D2230/5D27D0. Fresh
// objects reconstruct active fields; source record/envelope suffixes and
// texture metadata are not transplanted. Target opaque tails are retained.
// Owns no pool cursor: Integrator and its Editor remain live for the complete
// copy/intersection/Region pipeline. The caller supplies a private candidate;
// errors may have edited it and must prevent publication. The optional matrix follows SDK point/bounds arithmetic, LC GetScale
// and active material reconstruction. Native runtime is unverified.
SourceCopyResult copy_source(Integrator&,const VectorPaintArchive& source,
    const Layer& source_owner,const SourceCopySettings& = {});
//5A9930 copies only the first Face of a Shape Region and its fresh material.
//Geometry and target Region are already created by the extension clone.
int copy_shape_face(Integrator&,const VectorPaintArchive&,int source_face,
    std::size_t destination_plane,int destination_region,
    std::optional<vector_copy_detail::Matrix> matrix={});
//5D4F10/5D1190 directed LC primitive, before modifier/Region conversion.
// Every ordered LC occurrence is fresh. selector0 reverses the source MC;
// destination members are selector1 and carry one endpoint across that LC.
// Source Line/Vert-Line child identities map to actual destination pure-Line
// children. No source Paint/Region wrapper is manufactured or consulted.
// The caller owns the candidate and
// must discard it after an error; closure welding and Region conversion are
// subsequent native stages. Source graph storage must be distinct.
LineSourceCopyResult copy_line_occurrences(vector_graph_detail::Editor&,
    const VectorLineArchive&,const Layer& source_owner,
    const std::vector<int>& ordered_lines,
    const std::map<std::size_t,std::size_t>& planes,
    std::array<std::int32_t,2> translation={},
    std::optional<vector_copy_detail::Matrix> matrix={});
//5D0D30 single directed MC leaf for real Vert-Line or Border destinations.
// carry==-1 creates the first Vertex; otherwise the live destination Vertex
// is reused. Each SC creates a fresh end Vertex, and carry receives the last.
// Same-archive copies are admitted while the source MC remains live; MC/SC
// active fields are captured before pool allocation invalidates addresses.
// Does not allocate an LC, copy suffixes, publish owners or rebuild LC caches.
int copy_directed_model(vector_graph_detail::Editor&,const VectorLineArchive&,
    int source_model,bool forward,int& carry,
    std::array<std::int32_t,2> translation={},
    std::optional<vector_copy_detail::Matrix> matrix={});
// Wrapper5D4440/5D4770 subsequently iterates the source-pointer-keyed MC map,
// independently of creation order. Native pages contain4096 MC objects;
// cross-page address order is unproved and rejects here.
std::vector<int> source_pointer_ordered_models(const SourceCopyResult&);
struct BoundaryCandidates {std::vector<int> models,regions;};
//6443E0/5A1F20/59DC20 saved-box query. Retains physical occurrence order
// and duplicates. Selector0 members admit Regions but do not append MCs.
// SDK915 CenterPoint: signed32-wrapped edge sum, then truncation toward zero.
VectorFaceVertex boundary_query_center(VectorBounds);
BoundaryCandidates query_candidates(Integrator&,VectorBounds);
}
