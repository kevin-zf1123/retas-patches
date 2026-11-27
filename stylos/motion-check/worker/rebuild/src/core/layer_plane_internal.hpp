#pragma once
#include "retas/layer_plane.hpp"
#include "resolution_raster_internal.hpp"
namespace retas::stack_detail {
Raster convert_raster(const Raster&,int depth);
//Optional source_request preserves63EF00's rounded extent for32/64 blending;
//low-depth explicit requests require the separate native three-copy staging.
void merge_raster(Raster&,const Raster&,int dx,int dy,int destination_mode,int source_mode=1,bool full_request=false,resolution_detail::Budget* budget=nullptr,std::optional<Rect> source_request={});
void transfer_raster(Plane&,const CommonElement& destination_owner,const Plane&,const CommonElement& source_owner,bool conversion);
Rect merged_owner_bounds(const CommonElement& destination,const Layer& source);
int plane_default_mode(std::uint32_t family) noexcept;
void merge_vector_planes(Layer&,std::size_t target,std::size_t source);
void append_vector_layer(Layer&,const Layer&);
}
