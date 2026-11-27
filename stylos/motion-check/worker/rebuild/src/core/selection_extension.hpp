#pragma once
#include "retas/floating.hpp"
namespace retas::selection_detail {
bool extension_move_owner(const Layer&) noexcept;
void validate_extension_move(const Layer&,double,std::uint64_t&,std::uint64_t&);
Layer capture_extension_move(Layer&,const Layer&,const std::vector<std::size_t>&,
    const RegionSelection&,double,std::uint64_t&,std::uint64_t&,bool& captured);
void materialize_extension_move(Layer&,const Layer&,const std::vector<std::size_t>&,
    std::array<int,2>,double,std::uint64_t&,std::uint64_t&);
}
