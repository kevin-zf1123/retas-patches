#pragma once
#include "retas/floating.hpp"
namespace retas::selection_detail {
//Native target+60 leaf rendering/line-color conversion into a private TMP.
Raster clipboard_vector_raster(const Layer&,std::size_t,const Layer&,const Plane&,
    std::array<int,2>,std::uint64_t&,std::uint64_t&,bool direct_line_target=false,bool fractional4=false,std::stop_token stop={});
Raster clipboard_line_raster(const Layer&,const Plane&,const Layer&,const Plane&,
    std::array<int,2>,std::uint64_t&,std::uint64_t&);
}
