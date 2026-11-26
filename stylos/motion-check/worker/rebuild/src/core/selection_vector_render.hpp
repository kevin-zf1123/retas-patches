#pragma once
#include "vector_stored_line.hpp"
#include <set>
#include "retas/vector_ruler_render.hpp"
#include "retas/vector_shape_render.hpp"
namespace retas::vector_render_detail {
struct ClipboardVectorRender {Raster pixels;StoredLineCacheCosts costs;std::set<TileCoord> resolved_blocks,touched_blocks;};
StoredLineCacheCosts ruler_output_costs(std::span<const VectorRulerPlaneGeometry>,const VectorRenderContext&,StoredLineCacheCosts,std::stop_token stop={});
StoredLineCacheCosts shape_output_costs(std::span<const VectorShapePlaneGeometry>,const VectorRenderContext&,StoredLineCacheCosts,std::stop_token stop={});
//5ED450: one actual source child, fresh destination-sized4x temporary.
//Normal display and fractional owner-output profiles remain separate.
ClipboardVectorRender render_clipboard_lines(std::span<const StoredLinePlane>,const VectorRenderContext&,StoredLineCacheCosts,bool byte_output=false,bool fractional4=false,std::stop_token stop={});
//676DB0: one matching LC, forward caller order, original projected bbox clip.
void draw_selection_line_bits(const StoredLinePlane&,std::size_t,const VectorRenderContext&,
    BinaryMask&,bool minimum_width,StoredLineCacheCosts&,std::stop_token);
Raster render_direct_line_bits(std::span<const StoredLinePlane>,const VectorRenderContext&,const Raster&,StoredLineCacheCosts&);
ClipboardVectorRender render_clipboard_rulers(std::span<const VectorRulerPlaneGeometry>,const VectorRenderContext&,StoredLineCacheCosts,bool byte_output=false,bool fractional4=false,std::stop_token stop={});
ClipboardVectorRender render_clipboard_shapes(std::span<const VectorShapePlaneGeometry>,const VectorRenderContext&,StoredLineCacheCosts,bool byte_output=false,bool face_chain=false,bool fractional4=false,std::stop_token stop={});
}
