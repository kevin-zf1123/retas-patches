#include "selection_clipboard_target.hpp"
#include "selection_vector_render.hpp"
#include "vector_clipboard_context.hpp"
#include "selection_raster.hpp"
#include "vector_document_geometry.hpp"
#include "retas/vector_face_archive.hpp"
#include "retas/vector_extension_archive.hpp"
#include "retas/vector_color_metadata.hpp"
#include "vector_material_edit.hpp"
#include "../io/vector_face_projection.hpp"
#include <cmath>
#include <limits>
namespace retas::selection_detail {
namespace {
void charge(std::uint64_t& remaining,std::uint64_t n){if(n>remaining)throw std::length_error("Clipboard target aggregate resources exceeded");remaining-=n;}
std::uint64_t tint(std::uint64_t color,int depth,bool face=false){
    if(depth<8)return (face||color>>48)?0xffff000000000000ull:0;
    if(depth==8){const unsigned value=face?unsigned(color>>24)&255u:255u;return std::uint64_t(value*257)*0x1000100010001ull;}
    return color;
}
void face_colors(VectorShapePlaneGeometry& p,int depth){if(depth>8)return;for(auto& ptr:p.faces){auto f=std::make_shared<VectorShapeFaceGeometry>(*ptr);const auto original=f->geometry;f->geometry.color=(original.material_flags&15)==1?tint(original.color,depth,depth==8):0xffff000000000000ull|(depth==8?0xffffffffffffull:0);f->geometry.material_flags=1;f->geometry.gradation.reset();f->geometry.texture.reset();f->texture_recipe.reset();ptr=std::move(f);}}
Raster typed_tmp(const vector_render_detail::ClipboardVectorRender& rendered,int depth,std::uint64_t& bytes,std::uint64_t& work,std::stop_token stop){
    const auto& source=rendered.pixels;const int w=source.width,h=source.height;charge(work,std::uint64_t(w)*h);Raster out(w,h,depth,source.resolution);
    const auto blocks=std::uint64_t((w+255)/256)*((h+255)/256);charge(bytes,blocks*out.tile_bytes());
    if(depth==16||depth==24){
        //64A620 binds the real low-depth row pitch but reads/writes DWORDs
        //at x*4. Preserve the observable in-backing byte layout. Native
        //out-of-backing accesses are rejected before any candidate publication.
        for(int ty=0;ty<(h+255)/256;++ty)for(int tx=0;tx<(w+255)/256;++tx){const int width=std::min(256,w-tx*256),height=std::min(256,h-ty*256);const auto pitch=std::size_t(256*depth/8);if(rendered.resolved_blocks.contains({tx,ty})&&std::size_t(height-1)*pitch+std::size_t(width)*4>out.tile_bytes())throw std::invalid_argument("Native low-depth DWORD vector target would exceed tile backing");}
        for(int ty=0;ty<(h+255)/256;++ty)for(int tx=0;tx<(w+255)/256;++tx){auto tile=std::make_shared<Bytes>(out.tile_bytes(),depth==24?255:0);if(rendered.resolved_blocks.contains({tx,ty}))for(int y=0;y<std::min(256,h-ty*256);++y)for(int x=0;x<std::min(256,w-tx*256);++x){const auto p=source.pixel(tx*256+x,ty*256+y);const std::uint32_t value=p.a?std::uint32_t(p.b>>8)|std::uint32_t(p.g>>8)<<8|std::uint32_t(p.r>>8)<<16|std::uint32_t(p.a>>8)<<24:0;const auto at=std::size_t(y)*(256*depth/8)+x*4;for(unsigned i=0;i<4;++i)(*tile)[at+i]=std::uint8_t(value>>(8*i));}out.tiles[{tx,ty}]=std::move(tile);}return out;
    }
    for(int ty=0;ty<(h+255)/256;++ty)for(int tx=0;tx<(w+255)/256;++tx)if(depth>=8||rendered.touched_blocks.contains({tx,ty})){auto tile=std::make_shared<Bytes>(out.tile_bytes(),0);
        if(depth==32||depth==64){const std::uint64_t clear=depth==32?0x00ffffffull:0x0000ffffffffffffull;const unsigned stride=depth/8;
            for(std::size_t at=0;at<tile->size();at+=stride)for(unsigned i=0;i<stride;++i)(*tile)[at+i]=std::uint8_t(clear>>(8*i));}
        out.tiles[{tx,ty}]=std::move(tile);}
    for(int y=0;y<h;++y)for(int x=0;x<w;++x){if(x==0&&stop.stop_requested())throw std::runtime_error("Vector materialization cancelled");if(depth<8&&!out.tiles.contains({x/256,y/256}))continue;const auto p=source.pixel(x,y);if(depth==32||depth==64){if(rendered.resolved_blocks.contains({x/256,y/256}))store_raw_pixel(out,x,y,p.a?(depth==32?std::uint64_t(p.b>>8)|std::uint64_t(p.g>>8)<<8|std::uint64_t(p.r>>8)<<16|std::uint64_t(p.a>>8)<<24:std::uint64_t(p.b)|std::uint64_t(p.g)<<16|std::uint64_t(p.r)<<32|std::uint64_t(p.a)<<48):0);}else store_raw_pixel(out,x,y,depth==1?std::uint64_t(p.a!=0):depth==2?std::uint64_t(p.a?3:0):std::uint64_t(p.a>>8));}
    return out;
}
}
Raster clipboard_vector_raster(const Layer& input,std::size_t child,const Layer& target,const Plane& plane,std::array<int,2> delta,std::uint64_t& bytes,std::uint64_t& work,bool direct_line_target,bool fractional4,std::stop_token stop){
    if(stop.stop_requested())throw std::runtime_error("Vector materialization cancelled");
    if(!plane.raster)throw std::invalid_argument("Vector clipboard raster target has no native offscreen");
    const auto& r=*plane.raster;const auto ratio=r.resolution/input.common.resolution;
    if(!std::isfinite(ratio)||ratio<=0)throw std::invalid_argument("Clipboard affine DPI ratio overflow");
    const Layer& source=input;std::uint64_t storage=512ull*1024*1024-bytes,operations=128ull*1024*1024-work;
    VectorRenderContext context{r.width,r.height,r.depth==64?64:32,r.resolution,input.planes.at(child).common.resolution/r.resolution,
        {double(target.common.x)-(double(input.common.x)+delta[0])*ratio,double(target.common.y)-(double(input.common.y)+delta[1])*ratio},std::nullopt,r.depth<8?1u:4u,true};
    using numeric_detail::binary64;const auto inverse_ratio=binary64(1.0/ratio);
    const auto scale=binary64(ratio*0.0625);
    const auto tx=binary64(binary64(double(input.common.x)+delta[0])-binary64(double(target.common.x)*inverse_ratio));
    const auto ty=binary64(binary64(double(input.common.y)+delta[1])-binary64(double(target.common.y)*inverse_ratio));
    context.clipboard_matrix={scale,0,binary64(tx*ratio),0,scale,binary64(ty*ratio)};
    const auto determinant=binary64(scale*scale),inverse_determinant=binary64(1.0/determinant),inverse_scale=binary64(scale*inverse_determinant);
    context.clipboard_inverse={inverse_scale,0,binary64(-binary64(context.clipboard_matrix[2]*scale)*inverse_determinant),0,inverse_scale,binary64(-binary64(scale*context.clipboard_matrix[5])*inverse_determinant)};
    const vector_render_detail::StoredLineCacheCosts previous{storage,operations};vector_render_detail::ClipboardVectorRender result;
    if(source.family==0x40004000){auto g=decode_vector_ruler_geometries(source);auto p=std::move(g.at(child));if(r.depth<=8){p.color=r.depth<8?0xffff000000000000ull:tint(p.color,r.depth);for(auto& line:p.lines.strokes)line.color=tint(line.color,r.depth);}result=vector_render_detail::render_clipboard_rulers({&p,1},context,previous,r.depth==8,fractional4,stop);}
    else if(source.family==0x40002000){auto g=decode_vector_shape_geometries(source);auto p=std::move(g.at(child));for(auto& line:p.lines.strokes)line.color=tint(line.color,r.depth);face_colors(p,r.depth);result=vector_render_detail::render_clipboard_shapes({&p,1},context,previous,r.depth==8,false,fractional4,stop);}
    else if(source.family==0x21200000||source.family==0x20800000){auto a=decode_vector_paint_archive(source);
        if(source.planes[child].family==0x21110001||source.planes[child].family==0x21120001){auto g=vector_render_detail::project_stored_paint_lines(source,a.base);auto p=std::move(g.at(child));for(auto& line:p.lines.strokes)line.color=tint(line.color,r.depth);result=vector_render_detail::render_clipboard_lines({&p,1},context,previous,r.depth==8,fractional4,stop);}
        else{const vector_material_detail::PaintLists lists(source.planes[child]);auto g=vector_archive_detail::project_shape_faces(a,lists.ids[0],storage);VectorShapePlaneGeometry p;for(auto& face:g){const int id=int(p.faces.size());const auto gate=face.face.region_bounds;p.faces.push_back(std::make_shared<const VectorShapeFaceGeometry>(std::move(face.face)));p.commands.push_back({-1,id,gate});}face_colors(p,r.depth);result=vector_render_detail::render_clipboard_shapes({&p,1},context,previous,r.depth==8,true,fractional4,stop);}

    }else{auto g=vector_render_detail::decode_stored_line_planes(source);auto p=std::move(g.at(child));for(auto& line:p.lines.strokes)line.color=tint(line.color,r.depth);
        if(direct_line_target&&r.depth==1&&(plane.family&0x00f00000u)==0x00200000u){context.converter_fine_step=true;auto costs=previous;auto bits=vector_render_detail::render_direct_line_bits({&p,1},context,r,costs);bytes=512ull*1024*1024-costs.storage;work=128ull*1024*1024-costs.work;return bits;}
        result=vector_render_detail::render_clipboard_lines({&p,1},context,previous,r.depth==8,fractional4,stop);}
    if(result.costs.storage>512ull*1024*1024||result.costs.work>128ull*1024*1024)throw std::length_error("Vector clipboard render exceeds aggregate resources");bytes=512ull*1024*1024-result.costs.storage;work=128ull*1024*1024-result.costs.work;return typed_tmp(result,r.depth,bytes,work,stop);
}
Raster clipboard_line_raster(const Layer& source,const Plane& line,const Layer& target,const Plane& plane,std::array<int,2> delta,std::uint64_t& bytes,std::uint64_t& work){
    if(!line.raster||line.raster->depth!=1||line.common.resolution!=source.common.resolution*8)throw std::invalid_argument("Native112 clipboard requires actual8x one-bit source");const auto& src=*line.raster;const auto& dst=*plane.raster;const auto dx=std::int64_t(source.common.x)+delta[0]-target.common.x,dy=std::int64_t(source.common.y)+delta[1]-target.common.y;
    charge(bytes,std::uint64_t((dst.width+255)/256)*((dst.height+255)/256)*65536*4);charge(work,std::uint64_t(dst.width)*dst.height*64);Raster tmp(dst.width,dst.height,32,target.common.resolution);
    for(int y=0;y<dst.height;++y)for(int x=0;x<dst.width;++x){unsigned bits{};for(int j=0;j<8;++j)for(int i=0;i<8;++i)bits+=((std::int64_t(x)-dx)*8+i>=0&&(std::int64_t(y)-dy)*8+j>=0&&(std::int64_t(x)-dx)*8+i<src.width&&(std::int64_t(y)-dy)*8+j<src.height)?raw_pixel(src,int((std::int64_t(x)-dx)*8+i),int((std::int64_t(y)-dy)*8+j))!=0:0;const unsigned alpha=(255*bits)>>6;if(alpha<127)continue;
        const auto color=line.common.flags[4]?line.common.color:0xffff000000000000ull;const std::uint32_t value=dst.depth==32||dst.depth==64?(std::uint32_t(color>>40)&255u)|(std::uint32_t(color>>24)&255u)<<8|(std::uint32_t(color>>8)&255u)<<16|(std::uint32_t(color>>56)&255u)<<24:std::uint32_t(1)<<24;store_raw_pixel(tmp,x,y,value);}
    return tmp;
}
}
