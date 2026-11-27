#pragma once
#include "retas/document.hpp"
namespace retas::vector_render_detail {struct StoredLineCacheCosts;}
namespace retas::selection_detail {
// Native vector-owner+100 transient raster helper. Construction is bounded
// by the existing owner renderer certificates and never rewrites wire data.
Raster owner_helper(const Layer&,double dpi,vector_render_detail::StoredLineCacheCosts* costs=nullptr);
Raster motion_owner_helper(const Layer&,int width,int height,double dpi,vector_render_detail::StoredLineCacheCosts&);
DocumentData query_data(const DocumentData&);
}
