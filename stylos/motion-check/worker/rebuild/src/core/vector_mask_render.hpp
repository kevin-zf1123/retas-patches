#pragma once
#include "vector_stored_line.hpp"
#include "retas/fill.hpp"

namespace retas::vector_render_detail {
struct BitLineMask {
    BinaryMask mask;
    VectorBounds trace_clip,draw_clip;
    StoredLineCacheCosts costs;
};
//55C020/62EFA0's transient source1 mode0 path. The source keeps its saved
//LC/MC gates and CP profiles. Context uses the actual bitmap dimensions
//and DPI, depth1, oversample1, zero origin and a finite positive raw scale
//from TempDPI/bitmapDPI*16. The same-DPI native4x case has scale4.
//clip is the original55C020 trace clip
//(saved source bounds projected toward zero, then inflated1).62EFA0 inflates
//it by2 again for drawing, without changing trace_clip. Source alpha WORD0
//clears bits; every nonzero alpha sets bits, independent of RGB or opacity.
//All work is transient and source-immutable. Costs include previous source
//resources; failure/cancellation throws before any result is published.
BitLineMask render_stored_line_bit_mask(const StoredLinePlane&,
    const VectorRenderContext&,StoredLineCacheCosts previous={},std::stop_token={});
struct BitCircleMaskOptions {
    //Captured globalPrefInfo+98, not a style multiplier. ON enables
    //634/638 with F32(the named profile's raw scale*0.60000002f),
    //retaining original gates/CPs: raw9.6 at1x, raw2.4 at4x.
    bool minimum_width{};
};
//55C020 passes no scalar override to62EFA0. At native4x, captured global
//minimum ON therefore uses F32(4*0.60000002f), raw2.4, never forced raw9.6.
//LC remains forward and clearRect remains OFF, as in the legacy bit entry.
BitLineMask render_stored_line_circle_mask_4x(const StoredLinePlane&,
    const VectorRenderContext&,BitCircleMaskOptions={},
    StoredLineCacheCosts previous={},std::stop_token={});
//53B9B0 grouped Paint Circle overrides634/638 with raw F32(9.6),
//independent of global minimum. Actual4x BitMask order/clear/clip remain.
BitLineMask render_stored_line_circle_eraser_mask_4x(const StoredLinePlane&,
    const VectorRenderContext&,StoredLineCacheCosts previous={},std::stop_token={});
//53FB60/5414F0/62E820's native1x Circle path on a fresh transient offscreen.
//Context is depth1/oversample1/zeroorigin with actual Temp dimensions/DPI
//and finite positive raw scale (16 for a fresh Temp). clip is the
//caller's raw dirty bounds projected with floor
//at1/16 then inflated1. Drawing independently inflates2. StoredLC order is
//reversed, MC/SC order and saved CPs/gates are retained. clearRect1 clears
//only existing handles: a fresh unwritten block remains absent; later
//blocks clear/free any prior span/cursor spill before drawing. The source
//is one actual visible TempLine projection; hidden Raster children are not
//passed. previous and reserved projection resources stay cumulative.
BitLineMask render_stored_line_circle_mask_1x(const StoredLinePlane&,
    const VectorRenderContext&,BitCircleMaskOptions={},
    StoredLineCacheCosts previous={},std::stop_token={});
}
