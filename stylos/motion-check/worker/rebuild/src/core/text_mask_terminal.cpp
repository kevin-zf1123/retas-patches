#include "text_font_backend_internal.hpp"
#include "retas/numeric.hpp"
#include "retas/document.hpp"
#include "retas/text_tool.hpp"
#include "retas/raster_stroke_pixels.hpp"
#include "selection_raster.hpp"
#include "vector_line_output.hpp"
#include <algorithm>
#include <cmath>

namespace retas::text_font_detail {
namespace {
using Budget=resolution_detail::Budget;
void validate(const Raster& image,Budget& budget){
    if(image.width<=0||image.height<=0||image.width>100000||image.height>100000
       ||std::uint64_t(image.width)*image.height>268435456||!std::isfinite(image.resolution)||image.resolution<=0
       ||!raster_stroke_surface_supported(image))throw std::invalid_argument("Invalid Text mask backing");
    const auto count=std::uint64_t((image.width+255)/256)*((image.height+255)/256);
    if(image.tiles.size()>count)throw std::invalid_argument("Invalid Text mask block count");
    for(const auto& [tile,bytes]:image.tiles){budget.visit(1);
        if(tile.x<0||tile.y<0||std::int64_t(tile.x)*256>=image.width||std::int64_t(tile.y)*256>=image.height||!bytes||bytes->size()!=image.tile_bytes())
            throw std::invalid_argument("Invalid Text mask block");
        budget.allocate(bytes->size()+128);
    }
}
void ensure(Raster& image,TileCoord tile,Budget& budget){
    const auto found=image.tiles.find(tile);
    if(found!=image.tiles.end()&&found->second.use_count()==1)return;
    budget.allocate(image.tile_bytes()+128);budget.visit(65536);
    selection_detail::store_raw_pixel(image,tile.x*256,tile.y*256,selection_detail::raw_pixel(image,tile.x*256,tile.y*256));
}
void paint(Raster& output,const ExportMasks& masks,int density,bool behind,Budget& budget){
    if((output.depth!=32&&output.depth!=64)||output.color_order!=3||density<0||density>100
       ||(behind&&output.depth!=32))throw std::invalid_argument("Invalid Text mask destination");
    validate(output,budget);
    const auto validate_mask=[&](const Mask& mask){
        if(mask.raster.depth!=1||mask.raster.color_order!=0)throw std::invalid_argument("Text requires canonical binary masks");
        validate(mask.raster,budget);
    };
    validate_mask(masks.glyph);if(masks.leader)validate_mask(*masks.leader);
    ensure(output,{0,0},budget); // AutoLock(target,0,0,1), even with no intersection
    const unsigned alpha=unsigned(*legacy_rt_mul_div(output.depth==64?65535:255,density,100));
    const auto draw=[&](const Mask& mask,std::uint32_t color){const auto& bitmap=mask.raster;
        const auto left=std::clamp<std::int64_t>(mask.origin[0],0,output.width),top=std::clamp<std::int64_t>(mask.origin[1],0,output.height);
        const auto right=std::clamp<std::int64_t>(std::int64_t(mask.origin[0])+bitmap.width,left,output.width),bottom=std::clamp<std::int64_t>(std::int64_t(mask.origin[1])+bitmap.height,top,output.height);
        if(left>=right||top>=bottom)return;
        budget.visit(std::uint64_t(right-left)*std::uint64_t(bottom-top));
        auto pigment=Pixel::rgba8(color&255,(color>>8)&255,(color>>16)&255,output.depth==32?alpha:255);
        if(output.depth==64)pigment.a=std::uint16_t(alpha);
        for(int ty=int(top/256);ty*256<bottom;++ty)for(int tx=int(left/256);tx*256<right;++tx){budget.poll();ensure(output,{tx,ty},budget);
            for(int y=std::max(int(top),ty*256);y<std::min(int(bottom),(ty+1)*256);++y)for(int x=std::max(int(left),tx*256);x<std::min(int(right),(tx+1)*256);++x){
                if(!bitmap.pixel(x-mask.origin[0],y-mask.origin[1]).a)continue;
                output.set_pixel(x,y,behind?vector_render_detail::output_terminal::behind_white(output.pixel(x,y),pigment,32):pigment);
            }
        }
    };
    draw(masks.glyph,masks.text_color);if(masks.leader)draw(*masks.leader,masks.line_color);
}
}
void paint_export_masks(Raster& output,const ExportMasks& masks,int density,Budget& budget){paint(output,masks,density,true,budget);}
void paint_conversion_masks(Raster& output,const ExportMasks& masks,Budget& budget){paint(output,masks,100,false,budget);}
ExportMasks ordinary_view_masks(const Layer& owner,const Plane& plane,double dpi,Budget& budget){
    const auto* image=image_masks(owner,plane,dpi);
    if(!image||!plane.runtime_text_cache->image_geometry)throw std::invalid_argument("Mixed-DPI Text view requires valid image bitmaps");
    return zero_placement_masks(owner,plane,*image,budget);
}
ExportMasks zero_placement_masks(const Layer& owner,const Plane& plane,const ExportMasks& bitmaps,Budget& budget){
    const auto* image=&bitmaps;
    budget.allocate(plane.specific.size()*2ull+sizeof(ExportMasks));budget.visit(plane.specific.size());
    const auto p=decode_text_parameters(plane.specific);
    const auto copy=[&](const Mask& source,std::array<int,2> origin,std::int64_t width,std::int64_t height){
        budget.allocate(sizeof(Mask)+source.raster.tiles.size()*128);budget.visit(source.raster.tiles.size());
        auto mask=source;mask.origin=origin;mask.raster.width=int(std::min<std::int64_t>(mask.raster.width,width));mask.raster.height=int(std::min<std::int64_t>(mask.raster.height,height));
        std::erase_if(mask.raster.tiles,[&](const auto& tile){return std::int64_t(tile.first.x)*256>=mask.raster.width||std::int64_t(tile.first.y)*256>=mask.raster.height;});return mask;
    };
    if(p.text.empty()||!p.text.front())return {copy(image->glyph,{},1,1),{},image->text_color,image->line_color};
    const auto checked=[](std::int64_t value){if(value<INT32_MIN||value>INT32_MAX)throw std::length_error("Text view coordinate exceeds signed32");return int(value);};
    const auto point=[&](std::array<int,2> local){return std::array<int,2>{checked(std::int64_t(owner.common.x)+local[0]),checked(std::int64_t(owner.common.y)+local[1])};};
    const auto text=point(p.text_anchor),endpoint=p.line_enabled?point(p.line_anchor):text;
    const int w=image->glyph.raster.width,h=image->glyph.raster.height;
    const int left=p.line_enabled&&text[0]<endpoint[0]?checked(std::int64_t(text[0])-w):text[0];
    const int right=checked(std::int64_t(left)+w),bottom=checked(std::int64_t(text[1])+h),join=bottom-1;
    ExportMasks result{copy(image->glyph,{left,text[1]},w,h),{},image->text_color,image->line_color};
    if(image->leader){
        auto x=left,y=text[1],r=right,b=bottom;
        if(text[0]!=endpoint[0]&&join!=endpoint[1]){x=std::min(x,std::min(text[0],endpoint[0]));y=std::min(y,std::min(join,endpoint[1]));r=std::max(r,std::max(text[0],endpoint[0]));b=std::max(b,std::max(join,endpoint[1]));}
        result.leader=copy(*image->leader,{x,y},std::int64_t(r)-x,std::int64_t(b)-y);
    }
    return result;
}
void admit_display_masks(const ExportMasks& masks,Budget& budget){
    budget.allocate(sizeof(ExportMasks));
    const auto admit=[&](const Mask& mask){
        if(mask.raster.depth!=1||mask.raster.color_order!=0)throw std::invalid_argument("Text requires canonical binary masks");
        validate(mask.raster,budget);budget.visit(std::uint64_t(mask.raster.width)*mask.raster.height);
    };
    admit(masks.glyph);if(masks.leader)admit(*masks.leader);
}
namespace {
Pixel pigment(std::uint32_t color,int depth,int density){
    auto p=Pixel::rgba8(color&255,(color>>8)&255,(color>>16)&255);
    const auto alpha=unsigned(*legacy_rt_mul_div(depth==64?65535:255,density,100));
    p.a=std::uint16_t(depth==64?alpha:alpha*257);return p;
}
}
Pixel display_pixel(const ExportMasks& masks,Pixel destination,int x,int y,int depth,int density){
    const auto draw=[&](const Mask& mask,std::uint32_t color){
        const auto sx=std::int64_t(x)-mask.origin[0],sy=std::int64_t(y)-mask.origin[1];
        if(sx>=0&&sy>=0&&sx<mask.raster.width&&sy<mask.raster.height&&mask.raster.pixel(int(sx),int(sy)).a)
            destination=vector_render_detail::output_terminal::behind_white(destination,pigment(color,depth,density),depth);
    };
    draw(masks.glyph,masks.text_color);if(masks.leader)draw(*masks.leader,masks.line_color);return destination;
}
void paint_display_masks(Raster& output,const ExportMasks& masks,int density,std::array<int,2> owner,std::stop_token stop){
    const auto draw=[&](const Mask& mask,std::uint32_t color){
        const auto ink=pigment(color,output.depth,density);if(!ink.a)return;
        const auto ox=std::int64_t(owner[0])+mask.origin[0],oy=std::int64_t(owner[1])+mask.origin[1];
        for(const auto& [tile,bytes]:mask.raster.tiles){
            const auto left=std::max<std::int64_t>(0,ox+std::int64_t(tile.x)*256),top=std::max<std::int64_t>(0,oy+std::int64_t(tile.y)*256);
            const auto right=std::min({std::int64_t(output.width),ox+mask.raster.width,ox+std::int64_t(tile.x+1)*256});
            const auto bottom=std::min({std::int64_t(output.height),oy+mask.raster.height,oy+std::int64_t(tile.y+1)*256});
            for(auto y=top;y<bottom;++y){if(stop.stop_requested())throw std::runtime_error("Text display cancelled");
                for(auto x=left;x<right;++x)if(mask.raster.pixel(int(x-ox),int(y-oy)).a){
                    const auto before=output.pixel(int(x),int(y)),after=vector_render_detail::output_terminal::behind_white(before,ink,output.depth);
                    if(after!=before)output.set_pixel(int(x),int(y),after);
                }
            }
        }
    };
    draw(masks.glyph,masks.text_color);if(masks.leader)draw(*masks.leader,masks.line_color);
}
}
