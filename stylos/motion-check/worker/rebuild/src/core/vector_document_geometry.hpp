#pragma once
#include "retas/document.hpp"
#include "retas/vector_geometry.hpp"

namespace retas::vector_geometry_detail {
bool owner(std::uint32_t family);
// Native persistent MC/Line cache rebuild, independent of raster projection.
void rebuild_lines(VectorLineArchive&,const std::vector<int>& lines,std::uint64_t& work,std::stop_token,bool refresh_models=true);
void rebuild_models(VectorLineArchive&,const std::vector<int>& models,std::uint64_t& work,std::stop_token);
// Closed Line/Temp/Vert-Line and Paint/Select envelopes. Shape/Ruler extensions
// need their own pool transforms. Geometry admission is independent of drawing.
void validate(const Layer& layer,std::uint64_t& allocation,std::uint64_t& work,std::stop_token stop);
// Shift only primitive/material/extension caches; owner and child metadata
// remain with the caller. Offsets are raw sixteenth-pixel coordinates.
void translate_cache(Layer&,int,int,std::uint64_t&,std::uint64_t&,std::stop_token);
void crop(Layer& layer,Rect target,std::uint64_t& allocation,std::uint64_t& work,std::stop_token stop);
void resolution(Layer& layer,int old_width,int old_height,int width,int height,double dpi,
                std::uint64_t& allocation,std::uint64_t& work,std::stop_token stop);
// Native16x pivot/trigonometric path. kind28/29 mirror,30/31 quarter-turn.
void page_transform(Layer&,std::int32_t kind,std::uint64_t& allocation,std::uint64_t& work,std::stop_token);
}
