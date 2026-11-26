#pragma once
#include "retas/document.hpp"
namespace retas::selection_detail {
RegionSelection combine_selection_layer(const Layer&,const RegionSelection&,bool subtract,int,int,double,std::uint64_t&,std::uint64_t&,std::stop_token,bool* propagate=nullptr);
RegionSelection from_selection_layer(const Layer&,const RegionSelection&,int page_width,int page_height,double page_dpi,
    std::uint64_t& bytes,std::uint64_t& work,std::stop_token);
}
