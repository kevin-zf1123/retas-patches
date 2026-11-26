#pragma once
#include "vector_probe.hpp"
#include "retas/vector_face_archive.hpp"
namespace retas::vector_graph_detail {struct Editor;}
namespace retas::vector_color_detail {
//5C8B80/5C8660 original-SC raster transitions, followed by5D6DB0.
// Collection keeps physical SC order and native float32 accumulation. The
// caller keeps one live Editor across materialization and owner publication.
struct LineSplitEvent {int code{},sub{-1};float t{};std::array<VectorPoint,2> hints;};
struct MaterializedLineSpans {
    std::vector<std::pair<int,bool>> parts;
    // Source first/start and source last/end no-change endpoint hits only.
    bool start{},end{};
};
std::vector<LineSplitEvent> collect_line_split_events(const VectorLineArchive&,
    int model,bool initial_side,const vector_probe_detail::Scope&,
    vector_probe_detail::Budget,bool collect_hints=true);
// Edits the caller's candidate in place. An error may have allocated or cut
// records, so callers must discard the candidate rather than publish it.
MaterializedLineSpans materialize_line_split_events(vector_graph_detail::Editor&,
    int model,const std::vector<LineSplitEvent>&,bool initial_side);
//5EE550 materializes selected LC runs before the final color write. Pools are
// sparse; owner traversal, SC identities and saved residual caches survive.
struct CutResult {std::vector<int> selected;VectorBounds dirty;bool topology_changed{};};
CutResult cut_lines(VectorLineArchive&,VectorPaintArchive*,Layer&,std::size_t plane,
    const std::vector<int>& original_lines,const vector_probe_detail::Scope&,
    std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& references,
    std::uint64_t& storage,std::uint64_t& work,std::stop_token);
}
