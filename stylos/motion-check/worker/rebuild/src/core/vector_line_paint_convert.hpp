#pragma once
#include "vector_boundary_source_copy.hpp"

namespace retas::vector_line_paint_detail {
struct ConversionSettings {
    // Actual source child identity -> destination pure Line child identity.
    std::map<std::size_t,std::size_t> planes;
    std::array<std::int32_t,2> translation{};
    std::optional<vector_copy_detail::Matrix> matrix;
};
struct ConversionResult {
    // The native temporary pointer array retains occurrences, including
    // released slots and later replacements at the same identity.
    std::vector<int> line_occurrences;
    //Raw5C3DD0 emission array;5C2410/5D15F0 may subsequently retire slots.
    //Read live ownership from the completed Integrator, not this array.
    std::vector<int> new_regions;
    vector_boundary_detail::BoundaryCandidates affected;
    VectorBounds context_dirty;
};
//5D4F10: pre-query frozen input LC boxes, directed occurrence clone5D1190,
//5D38E0/5C3DD0, optional target5D2A00/5C2410(rerun1), then5D15F0.
//This is a Line-to-Paint conversion entry, not Lighttable source preparation.
//An Integrator/Editor and all allocation cursors stay live across the whole
//pipeline. The caller publishes only its successfully completed candidate.
ConversionResult convert_lines(vector_boundary_detail::Integrator&,
    const VectorLineArchive&,const Layer& source_owner,
    const std::vector<int>& source_lines,const ConversionSettings&);
//5D52B0: empty source LC is a no-op; rerun controls target materialization.
ConversionResult convert_line(vector_boundary_detail::Integrator&,
    const VectorLineArchive&,const Layer& source_owner,int source_line,
    const ConversionSettings&,bool rerun_materializer);
}
