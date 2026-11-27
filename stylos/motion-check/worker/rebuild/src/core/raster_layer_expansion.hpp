#pragma once
#include "retas/layer_move.hpp"
#include <stop_token>

namespace retas::layer_move_detail {
// Internal bounded form for Transform preparation. Counters are used bytes
// and visits in the caller's aggregate512MiB/128Mi budget.
bool expand_raster_layer(Layer&,Rect,std::uint64_t& bytes,std::uint64_t& work,std::stop_token);
// Native61FD20/61FD70 before a page geometry crop, independent of LYMV gates.
void move_geometry_layer(Layer&,int dx,int dy,int page_width,int page_height,
    std::uint64_t& bytes,std::uint64_t& work,std::stop_token);
}
