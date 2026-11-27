#pragma once
#include "retas/resolution_raster.hpp"

namespace retas::resolution_detail {
// Shared by all planes of one document operation, rather than restarting
// resource admission for each plane. Counts are conservative reservations.
struct Budget {
    std::uint64_t bytes{},work{};
    std::stop_token stop;
    void poll() const;
    void allocate(std::uint64_t);
    void visit(std::uint64_t);
};
Raster resample(const Raster&,Rect,int,int,double,Budget&,bool change_resolution2=false,int target_depth=0,bool mode_zero=false);
}
