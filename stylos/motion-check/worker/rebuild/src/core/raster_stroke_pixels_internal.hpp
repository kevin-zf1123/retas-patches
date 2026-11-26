#pragma once
#include "retas/raster_stroke_pixels.hpp"
namespace retas {
RasterStrokePixelResult clear_raster_stroke_pixels(Raster&,Point,const RasterStrokeDab&,
    const std::optional<RasterSelection>&,std::stop_token);
RasterStrokePixelResult transfer_raster_stroke_pixels(Raster&,const Raster&,Pixel,RasterStrokeTerminal,Rect,std::stop_token);
RasterStrokePixelResult transfer_eraser_mask(Raster&,const BinaryMask&,const std::optional<RasterSelection>&,std::stop_token,double source_dpi=0);
RasterStrokePixelResult transfer_eraser_coverage(Raster&,const Raster&,Rect,std::stop_token);
RasterStrokePixelResult transfer_pencil_line(Raster&,const Raster&,Pixel,Rect,std::stop_token);
}
