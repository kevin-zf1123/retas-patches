#pragma once

#include "geometry.hpp"
#include "raster.hpp"
#include <stop_token>

namespace retas {
// ImageResolution's same-depth raster operation (mode0 for1-bit; mode1 otherwise).
// The source rectangle
// is clipped independently to the source's logical bounds, and the clipped
// extent is mapped to the entire output. It is not a padded-canvas resize.
// Admits normal depths1/2/8/16 (order0),24 (order4),32/64 (order3).
// The16-bit profile is gray8 plus alpha8, not a16-bit coverage channel.
// Mode0 uses block-local integer majority; mode1 uses native area tables or
// exact Half/Quarter floor shortcuts. No compositing or palette remapping.
// The returned raster owns its backing; source backing is never modified.
// Caller must serialize calls with external mutation of source.tiles and its
// backing aliases. Admission caps are 512 MiB for source physical storage,
// complete output reservation and tables together, and 128 Mi pixel visits.
Raster resolution_raster(const Raster& source, Rect source_rectangle,
                         int width, int height, double dpi,
                         std::stop_token stop = {});
}
