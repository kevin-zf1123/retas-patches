#pragma once
#include "vector_boundary_integrate.hpp"
namespace retas::vector_boundary_detail {
// BFIL's5B22C0 packet: extra=branch=gather=0. All coordinates/radii are raw
// signed32 units. The Editor and Integrator belong to the caller's private
// candidate and retain allocator cursors throughout the complete pipeline.
// Errors may have edited that candidate; only the caller publishes its Layer
// after the complete command succeeds. Bounds/query leaves retain their
// existing raw-coordinate/work limits; cross-page Region pointer ordering
// rejects because native heap address order is not persisted in CEL.
// Ordinary driver accepts nonzero signed radii. Negative radii cannot pass
// the native nonnegative-distance comparison, but retain the seed live set.
// Raw0 is a modern direct-call precondition failure (native callers skip it).
// The raster connector driver still requires positive radii.
// JINT uses extra=true for ordinary connector MC flag0x41; special source
// replacement copies its original flags. Branch/gather modes remain excluded.
struct GapWedge {int region{-1};bool two_sided{};float previous{},current{};};
struct GapTarget {int model{-1},subcurve{-1};float parameter{};bool special{};};
struct GapResult {
    bool processed{}; //5B22C0: a join branch completed, independent of cuts.
    bool storage_changed{}; //False ordinary joins may still have cut targets.
    std::vector<int> regions;
    VectorBounds context_dirty;
};
GapWedge source_gap_wedge(Integrator&,int vertex);
std::optional<GapTarget> find_gap_target(Integrator&,int vertex,const GapWedge&,
    int radius,const vector_probe_detail::Scope& = {});
//5B0330 /5B0640. Ordinary returns false AFTER target cut and dirty union
// when source/cut coordinates coincide. Special has no native BOOL gate.
GapResult join_gap(Integrator&,int source_vertex,const GapTarget&,bool special);
GapResult join_gap(Integrator&,int source_vertex,const GapTarget&,bool special,bool joint_line_extra);
GapResult close_gaps(Integrator&,int seed_region,int radius,
    const vector_probe_detail::Scope& = {});
GapResult close_gaps(Integrator&,int seed_region,int radius,
    const vector_probe_detail::Scope&,bool joint_line_extra);
GapResult close_raster_gaps(Integrator&,int seed_region,int radius,
    const vector_probe_detail::Scope&);
}
