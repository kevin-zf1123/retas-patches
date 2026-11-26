#pragma once
#include "vector_stored_line.hpp"

namespace retas::vector_render_detail {
// Print's private normal2 LC/Face cache. Complete hidden graph admission and
// the existing material/topology certificate precede visibility filtering.
// Caller reserves wire expansion in the shared ledger; malformed inputs,
// limits and cancellation throw. A null runtime owner cache returns nullopt
// after admission. Every logical cache block retains fresh-load transparent
// white backing, including padding. Actual32/64 context depth controls both
// cache production and pre-decompression accounting. No source, extent or
// history is modified.
std::optional<Raster> paint_cache_for_print(const Layer&,const VectorRenderContext&,
    StoredLineCacheCosts&,std::stop_token);
//62F4A0: target-sized normal1/AA2 redraw, origin includes owner XY.
//Complete hidden admission and deferred materials precede visibility; the
//native unguarded null carrier is refused safely. Caller transfers at XY0.
Raster paint_reference_redraw(const Layer&,const VectorRenderContext&,
    StoredLineCacheCosts&,std::stop_token);
// Ordinary62F4A0 request uses target32/64 and normal1/AA2/weighted4.
// Every requested physical block is present white/A0 before root transfer.
Raster paint_direct_redraw(const Layer&,const VectorRenderContext&,
    StoredLineCacheCosts&,std::stop_token);
}
