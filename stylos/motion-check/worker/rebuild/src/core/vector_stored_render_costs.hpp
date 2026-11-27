#pragma once
#include "vector_stored_line.hpp"

namespace retas {struct VectorFacePlaneView;}

namespace retas::vector_render_detail {
// Normal stored RGBA1x/AA2 admission, including original bounds, saved CPs
// and the source projection's reserved storage/work. Uses the same prepare
// path as can_render_stored_line_planes without producing an output raster.
// Returns cumulative costs including previous resources; admission or
// cancellation failure throws before the caller publishes its candidate.
StoredLineCacheCosts stored_line_planes_costs(std::span<const StoredLinePlane>,
    const VectorRenderContext&,StoredLineCacheCosts previous={},std::stop_token={});
// Actual ordinary Paint AA2 admission: sparse Lines, complete Face programs
// and their union of physical execution blocks. Faces remain caller-owned
// for the duration of this call; hidden siblings still participate.
StoredLineCacheCosts stored_paint_planes_costs(std::span<const StoredPaintPlane>,
    const VectorRenderContext&,StoredLineCacheCosts previous={},std::stop_token={});
// Face-only Paint's actual owner admission, including whole-owner gradient
// query bounds and deep texture snapshots. All Face pointers are borrowed
// synchronously; preceding source/derived resources remain in the ledger.
StoredLineCacheCosts stored_face_planes_costs(std::span<const VectorFacePlaneView>,
    const VectorRenderContext&,StoredLineCacheCosts previous={},std::stop_token={});
}
