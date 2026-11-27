#pragma once
#include "retas/floating.hpp"
namespace retas::selection_detail {
bool vector_move_owner(const Layer&) noexcept;
bool materialize_vector_target(Layer&,std::size_t,const Layer&,std::array<int,2>,double,std::uint64_t&,std::uint64_t&);
void validate_vector_move(const Layer&,double dpi,std::uint64_t& bytes,std::uint64_t& work);
Layer capture_vector_move(Layer& remainder,const Layer& source,const std::vector<std::size_t>& planes,
    const RegionSelection&,double dpi,std::uint64_t& bytes,std::uint64_t& work,bool& captured,Rect& payload_bounds,
    VectorBounds& history_damage,bool copy_only=false);
void materialize_vector_move(Layer& destination,const Layer& payload,const std::vector<std::size_t>& planes,
    std::array<int,2> delta,double dpi,std::uint64_t& bytes,std::uint64_t& work);
}
