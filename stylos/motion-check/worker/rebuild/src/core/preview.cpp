#include "vector_direct_context.hpp"
#include "retas/preview.hpp"
#include "retas/stylos_pixel.hpp"
#include "stylos_gdi_text.hpp"
#include "retas/raster_stroke_pixels.hpp"
#include "retas/resolution_raster.hpp"
#include "retas/clipboard.hpp"
#include "retas/numeric.hpp"
#include "retas/fill.hpp"
#include "retas/text_font_backend.hpp"
#include "retas/text_tool.hpp"
#include "vector_line_output.hpp"
#include "text_font_backend_internal.hpp"
#include "vector_stored_line.hpp"
#include "selection_source.hpp"
#include "selection_vector.hpp"
#include "selection_extension.hpp"
#include "selection_vector_render.hpp"
#include "paint_owner_render.hpp"
#include "modal_document_guard.hpp"
#include "floating_preview_probe.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <stdexcept>

namespace retas {
PreviewPixel preview_behind(PreviewPixel foreground,PreviewPixel source) noexcept{
    const unsigned remaining=255-foreground.a;
    const unsigned weight=unsigned(source.a)*remaining/255;
    const auto channel=[&](unsigned previous,unsigned incoming){
        return std::uint8_t(std::max(0,int(previous)-int((255-incoming)*weight/255)));
    };
    return {channel(foreground.r,source.r),channel(foreground.g,source.g),channel(foreground.b,source.b),
        std::uint8_t(255-(255-unsigned(source.a))*remaining/255)};
}
PreviewPixel preview_cache_behind(PreviewPixel foreground,PreviewPixel cache) noexcept{
    if(!cache.a||foreground.a==255)return foreground;
    if(cache.a==255&&!foreground.a)return cache;
    const unsigned remaining=255-foreground.a;
    const auto channel=[&](unsigned previous,unsigned incoming){
        return std::uint8_t(std::max(0,int(previous)-int((255-incoming)*remaining/255)));
    };
    return {channel(foreground.r,cache.r),channel(foreground.g,cache.g),channel(foreground.b,cache.b),
        std::uint8_t(255-(255-unsigned(cache.a))*remaining/255)};
}
PreviewPixel preview_reference_behind(PreviewPixel foreground,PreviewPixel sample,PreviewReferenceOptions options){
    if(options.effective_opacity_percent<0||options.effective_opacity_percent>100)
        throw std::invalid_argument("灯箱预览浓度超出支持范围。");
    if(options.mode!=PreviewReferenceMode::Color&&options.mode!=PreviewReferenceMode::Monotone&&options.mode!=PreviewReferenceMode::Halftone)
        throw std::invalid_argument("灯箱预览颜色模式无效。");
    if(!sample.a||foreground.a==255||!options.effective_opacity_percent)return foreground;
    const unsigned density=unsigned(*legacy_rt_mul_div(256,options.effective_opacity_percent,100));
    const auto raw_alpha=sample.a;
    if(!options.color_mapping_applied&&options.mode==PreviewReferenceMode::Monotone){
        const int darkness=255-(unsigned(sample.r)+4*unsigned(sample.g)+2*unsigned(sample.b))/7;
        const auto channel=[&](std::uint16_t color){
            // The original signed SAR rounds negative tint products down.
            const int product=darkness*(int(color>>8)-255);
            const int scaled=product/256-(product<0&&product%256!=0);
            return std::uint8_t(255+scaled);
        };
        sample.r=channel(options.monotone_color.r);sample.g=channel(options.monotone_color.g);sample.b=channel(options.monotone_color.b);
    }else if(!options.color_mapping_applied&&options.mode==PreviewReferenceMode::Halftone){
        sample.r=std::uint8_t((sample.r>>1)+(options.monotone_color.r>>9));
        sample.g=std::uint8_t((sample.g>>1)+(options.monotone_color.g>>9));
        sample.b=std::uint8_t((sample.b>>1)+(options.monotone_color.b>>9));
    }
    if(options.see_through){
        sample.a=std::uint8_t(unsigned(raw_alpha)*density/256);
        if(sample.a==255&&!foreground.a)return sample;
        return preview_behind(foreground,sample);
    }
    unsigned weight=(unsigned(raw_alpha)+1)*density/256;
    // The mapped-color branch of sub_610910 squares this weight. Its
    // ordinary Color branch does not; both replace a nonopaque destination.
    if(options.mode!=PreviewReferenceMode::Color)weight=weight*weight/256;
    const auto channel=[&](unsigned color){return std::uint8_t(255-(255-color)*weight/256);};
    return {channel(sample.r),channel(sample.g),channel(sample.b),255};
}
Pixel preview_display(PreviewPixel cache,Pixel background) noexcept{
    if(!cache.a)return Pixel::rgba8(background.r>>8,background.g>>8,background.b>>8);
    const auto channel=[&](unsigned previous,unsigned incoming){
        return unsigned(std::max(0,int(previous)-int((255-(incoming>>8))*(255-unsigned(cache.a))/255)));
    };
    return Pixel::rgba8(channel(cache.r,background.r),channel(cache.g,background.g),channel(cache.b,background.b));
}
Pixel preview_color_check(PreviewPixel cache,bool preserve_primary,bool paint_mono_present) noexcept{
    if(!cache.a)return Pixel::rgba8(255,255,255);
    const bool primary=(cache.r==255&&!cache.g&&!cache.b)||(!cache.r&&cache.g==255&&!cache.b)||(!cache.r&&!cache.g&&cache.b==255);
    if(preserve_primary&&primary){
        if(paint_mono_present||cache.a==255)return Pixel::rgba8(cache.r,cache.g,cache.b);
        // sub_462100 multiplies by the stored reciprocal, then by the
        // primary channel, before __ftol2. At alpha33 this yields32, so an
        // integer multiply/divide would lose the original quantization.
        const auto alpha=numeric_detail::binary64(double(cache.a)*0.00392156862745098);
        const auto channel=[&](unsigned value){return unsigned(numeric_detail::binary64(alpha*value));};
        return Pixel::rgba8(channel(cache.r),channel(cache.g),channel(cache.b));
    }
    const auto gray=paint_mono_present?0:255-cache.a;
    return Pixel::rgba8(gray,gray,gray);
}
PreviewImage::PreviewImage(Rect bounds):bounds_(bounds){
    if(bounds.width<0||bounds.height<0||std::uint64_t(bounds.width)*bounds.height>64ull*1024*1024
        ||std::int64_t(bounds.x)+bounds.width>std::numeric_limits<int>::max()
        ||std::int64_t(bounds.y)+bounds.height>std::numeric_limits<int>::max())throw std::invalid_argument("预览范围超出支持上限。");
    pixels_.resize(std::size_t(bounds.width)*bounds.height);
}
PreviewPixel PreviewImage::pixel(int x,int y) const noexcept{
    const auto local_x=std::int64_t(x)-bounds_.x,local_y=std::int64_t(y)-bounds_.y;
    if(local_x<0||local_y<0||local_x>=bounds_.width||local_y>=bounds_.height)return {};
    const auto index=std::size_t(local_y)*bounds_.width+std::size_t(local_x);
    return index<pixels_.size()?pixels_[index]:PreviewPixel{};
}
void PreviewImage::set_pixel(int x,int y,PreviewPixel value) noexcept{
    const auto local_x=std::int64_t(x)-bounds_.x,local_y=std::int64_t(y)-bounds_.y;
    if(local_x<0||local_y<0||local_x>=bounds_.width||local_y>=bounds_.height)return;
    const auto index=std::size_t(local_y)*bounds_.width+std::size_t(local_x);
    if(index<pixels_.size())pixels_[index]=value;
}
namespace {
struct Area {
    std::int64_t left{},top{},right{},bottom{};
    bool empty() const noexcept{return left>=right||top>=bottom;}
};
Area area(Rect r){return {r.x,r.y,std::int64_t(r.x)+std::max(0,r.width),std::int64_t(r.y)+std::max(0,r.height)};}
Area intersect(Area a,Area b){return {std::max(a.left,b.left),std::max(a.top,b.top),std::min(a.right,b.right),std::min(a.bottom,b.bottom)};}
Rect rectangle(Area a){if(a.empty())return {};return {int(a.left),int(a.top),int(a.right-a.left),int(a.bottom-a.top)};}
PreviewPixel packed_color(std::uint64_t color){return {std::uint8_t(color>>8),std::uint8_t(color>>24),std::uint8_t(color>>40),std::uint8_t(color>>56)};}
bool color_check_excludes(std::uint32_t family){
    switch(family){
    case 0x10000400: case 0x10010000: case 0x10040000: case 0x18004000:
    case 0x40000800: case 0x40020000: case 0x48040800:return true;
    default:return false;
    }
}
struct Source {
    enum class Mode {Normal,WhiteKey,Mask,Opaque};
    std::shared_ptr<const Raster> raster;
    std::int64_t x{},y{};
    int scale{1},width{},height{};
    unsigned density{255};
    Mode mode{Mode::Normal};
    PreviewPixel fixed;
    bool missing_white{};
    bool native_helper{},stylos_score_pixels{};
    bool direct_copy{};
    std::shared_ptr<const text_font_detail::ExportMasks> text_masks;
    adjust_render_detail::Remap tables;
    bool remap_rgb{true};
};
struct AdapterCosts {
    std::uint64_t work{},bytes{};
    void conversion(int width,int height,int source_width=0,int source_height=0,std::uint64_t backed_tiles=0){
        const auto area=std::uint64_t(width)*height,storage=std::uint64_t((width+255)/256)*((height+255)/256)*256*256*4;
        // Each area-table output interval may cross source cell boundaries.
        // The summed entries per axis are bounded by source+output lengths,
        // including fractional-boundary revisits. The resampler's store and
        // the subsequent RGBA conversion each walk the output once.
        //Equal rounded dimensions select identity, which copies full physical
        //backings even on narrow logical edge tiles.
        const auto visits=!source_width?area:source_width==width&&source_height==height?backed_tiles*65536+area:
            std::uint64_t(source_width+width)*(source_height+height)+2*area;
        if(visits>128ull*1024*1024-work||storage>256ull*1024*1024-bytes)throw UnsupportedPreview("选择预览转换的累计资源超出支持范围。");
        work+=visits;bytes+=storage;
    }
};
void validate_storage(const Raster& raster){
    if(raster.width<=0||raster.height<=0||raster.width>100000||raster.height>100000
        ||std::uint64_t(raster.width)*raster.height>268435456)throw std::invalid_argument("预览光栅尺寸无效。");
    for(const auto& [tile,bytes]:raster.tiles)if(tile.x<0||tile.y<0||std::int64_t(tile.x)*256>=raster.width
        ||std::int64_t(tile.y)*256>=raster.height||!bytes||bytes->size()!=raster.tile_bytes())throw std::invalid_argument("预览光栅图块无效。");
}
struct TextImageContext {
    vector_render_detail::StoredLineCacheCosts& costs;
    std::unique_ptr<TextFontBackend> fonts;
    std::uint64_t pixels{};
};
Source image_text_source(const Layer& owner,const Plane& plane,double dpi,PreviewOptions options,AdapterCosts& costs,TextImageContext& context){
    resolution_detail::Budget budget{context.costs.storage+costs.bytes,context.costs.work+costs.work,options.motion_check?options.motion_stop:options.reference_stop};
    if(budget.bytes>512ull*1024*1024||budget.work>128ull*1024*1024)throw std::length_error("Text image source resource seed exceeds limits");
    budget.poll();
    if(plane.opaque||plane.raster||plane.specific.size()>16ull*1024*1024)throw UnsupportedPreview("Unsupported Text image source payload");
    budget.allocate(plane.specific.size()*64ull+4096+sizeof(Source)+sizeof(Raster)+sizeof(text_font_detail::ExportMasks));budget.visit(plane.specific.size());
    const auto parameters=decode_text_parameters(plane.specific);
    text_font_detail::ExportMasks masks{{Raster(1,1,1,dpi),{}},{},parameters.text_color,parameters.line_color};
    //641CE0 returns before font/owner access when the runtime string is empty.
    if(!parameters.text.empty()&&parameters.text.front()){
#ifdef _WIN32
        if(options.stylos_score_pixels){
            masks=text_font_detail::stylos_gdi_text(parameters,owner,plane,dpi,budget);
        }else
#endif
        {
        if(!(options.motion_check?options.motion_qt_text_fonts:options.reference_qt_text_fonts))throw UnsupportedPreview("Text image source requires explicit Qt fonts");
        if((owner.family!=0x40000800u&&owner.family!=0x48040800u)||owner.opaque
           ||!std::isfinite(owner.common.resolution)||owner.common.resolution<=0||owner.common.opacity<0||owner.common.opacity>100)
            throw UnsupportedPreview("Unsupported Text image source owner");
        if(!context.fonts)context.fonts=std::make_unique<TextFontBackend>();
        masks=text_font_detail::export_masks(*context.fonts,owner,plane,dpi,budget,context.pixels);
        }
    }
    text_font_detail::admit_display_masks(masks,budget);
    Source result;result.raster=std::make_shared<const Raster>(1,1,32,dpi);result.text_masks=std::make_shared<const text_font_detail::ExportMasks>(std::move(masks));
    result.density=unsigned(std::clamp(owner.common.opacity,0,100));result.native_helper=true;
    costs.bytes=budget.bytes-context.costs.storage;costs.work=budget.work-context.costs.work;
    return result; // export_masks origins already include owner XY and child DPI
}
Source source(const Layer& layer,const Plane& plane,double dpi,PreviewOptions options,bool mask=false,AdapterCosts* costs=nullptr,bool native_dispatch=false,TextImageContext* text_image=nullptr){
    AdapterCosts fallback;if(!costs)costs=&fallback;
    if(plane.family==0x40000004u&&(options.motion_check||(options.reference_source&&options.reference_qt_text_fonts))){
        if(mask||!text_image)throw UnsupportedPreview("Text image source requires its independent consumer");
        return image_text_source(layer,plane,dpi,options,*costs,*text_image);
    }
    std::shared_ptr<const text_font_detail::ExportMasks> view;
    if(plane.runtime_text_cache&&plane.runtime_text_cache->image_geometry){
        if(mask||options.reference_source)throw UnsupportedPreview("Mixed-DPI Text cache cannot supply a raster alpha mask or default reference source");
        resolution_detail::Budget budget{costs->bytes,costs->work,{}};
        view=std::make_shared<const text_font_detail::ExportMasks>(text_font_detail::ordinary_view_masks(layer,plane,dpi,budget));costs->bytes=budget.bytes;costs->work=budget.work;
    }
    if(const auto* masks=view?view.get():text_font_detail::display_masks(layer,plane,dpi)){
        if(mask)throw UnsupportedPreview("Text cache cannot supply a raster alpha mask");
        if(options.reference_source&&layer.common.opacity!=100)throw UnsupportedPreview("Partial Text reference source requires its independent native consumer");
        const auto& cache=*plane.runtime_text_cache;
        validate_storage(cache.raster);
        resolution_detail::Budget budget{costs->bytes,costs->work,{}};text_font_detail::admit_display_masks(*masks,budget);
        for(const auto& [tile,bytes]:cache.raster.tiles)budget.allocate(bytes->size()+128);
        costs->bytes=budget.bytes;costs->work=budget.work;
        Source result;result.raster=std::make_shared<const Raster>(cache.raster);result.text_masks=view?std::move(view):cache.masks;
        result.x=cache.image_geometry?0:layer.common.x;result.y=cache.image_geometry?0:layer.common.y;result.width=cache.raster.width;result.height=cache.raster.height;
        result.density=unsigned(layer.common.opacity);result.native_helper=true;return result;
    }
    if(const auto* text=text_raster_cache(plane)){
        if(!text_raster_cache(layer,plane,dpi))throw UnsupportedPreview("Text cache owner or DPI requires a supported font profile");
        if(text->resolution!=dpi)throw UnsupportedPreview("文字缓存需要按当前文档DPI重建。");
        auto converted=plane;converted.raster=*text;converted.common.width=text->width;converted.common.height=text->height;converted.common.resolution=text->resolution;converted.runtime_text_cache.reset();converted.family=0x10000001;
        Layer positioned;positioned.family=layer.family;positioned.common=layer.common;positioned.opaque=layer.opaque;
        if(plane.runtime_text_cache->static_geometry){
            if(mask)throw UnsupportedPreview("Text cache cannot supply a raster alpha mask");
            //641CE0 reads owner density only. Keep generic raster modes from
            //white-keying glyphs or turning the text bounds into opaque paper.
            converted.common.opacity=100;converted.common.opacity_mode=1;converted.common.flags[4]=0;
            positioned.common.opacity_mode=1;
            //Text geometry uses document/child DPI, never the owner DPI.
            //Only this transient raster adapter needs same-DPI metadata.
            positioned.common.resolution=dpi;
        }
        const auto x=std::int64_t(layer.common.x)+plane.runtime_text_cache->origin[0],y=std::int64_t(layer.common.y)+plane.runtime_text_cache->origin[1];
        if(x<INT32_MIN||x>INT32_MAX||y<INT32_MIN||y>INT32_MAX)throw UnsupportedPreview("Text cache origin exceeds signed32");
        positioned.common.x=int(x);positioned.common.y=int(y);return source(positioned,converted,dpi,options,mask,costs,native_dispatch);
    }
    if(layer.opaque||plane.opaque||!plane.raster)throw UnsupportedPreview("预览包含尚未支持的对象。");
    if(!std::isfinite(dpi)||dpi<=0)throw std::invalid_argument("预览分辨率无效。");
    const auto& raster=*plane.raster;
    if(!std::isfinite(layer.common.resolution)||layer.common.resolution<=0||!std::isfinite(raster.resolution)||raster.resolution<=0
        ||!std::isfinite(plane.common.resolution)||plane.common.resolution<=0)throw std::invalid_argument("预览分辨率无效。");
    if(layer.common.resolution!=dpi||raster.resolution!=plane.common.resolution)throw UnsupportedPreview("预览的图层或平面分辨率尚未支持。");
    validate_storage(raster);
    if(!raster_stroke_surface_supported(raster))throw UnsupportedPreview("预览光栅格式尚未支持。");
    if(options.reference_source&&native_dispatch){
        Source result;result.raster=std::make_shared<const Raster>(raster);result.x=layer.common.x;result.y=layer.common.y;result.native_helper=true;
        int scale{};for(int value:{1,2,4,8})if(plane.common.resolution==numeric_detail::binary64(dpi*value))scale=value;
        if(!scale||(scale!=1&&raster.depth!=1))return result;
        if(layer.common.opacity<0||layer.common.opacity>100)throw std::invalid_argument("参考源浓度超出支持范围。");
        result.scale=scale;result.width=raster.width/scale+(scale==2&&raster.width%2!=0);result.height=raster.height/scale;
        const bool opaque=layer.common.opacity_mode==2&&scale==1;
        result.density=opaque?255:unsigned(*legacy_rt_mul_div(255,layer.common.opacity,100));
        result.fixed=packed_color(plane.common.flags[4]?plane.common.color:0);
        result.missing_white=opaque;
        if(raster.can_edit())result.mode=opaque?Source::Mode::Opaque:plane.common.opacity_mode==5?Source::Mode::WhiteKey:Source::Mode::Normal;
        if(scale==2||scale==4)result.density|=result.fixed.a;
        return result;
    }
    if(native_dispatch&&raster.can_edit()&&plane.common.resolution!=dpi){
        // CRasterPlane::Render only has same-DPI RGBA leaves. Its -1/-2/-3
        // handlers consume one-bit sources; every other selector skips.
        Source skipped;skipped.raster=std::make_shared<const Raster>(raster);return skipped;
    }
    if((raster.depth!=1&&(!raster.can_edit()||plane.common.resolution!=dpi))||
        (raster.depth==1&&plane.common.resolution!=dpi&&plane.common.resolution!=dpi*2&&plane.common.resolution!=dpi*4&&plane.common.resolution!=dpi*8)){
        const auto width=std::trunc(raster.width*dpi/plane.common.resolution),height=std::trunc(raster.height*dpi/plane.common.resolution);
        if(!std::isfinite(width)||!std::isfinite(height)||width<1||height<1||width>100000||height>100000
            ||width*height>16ull*1024*1024)throw UnsupportedPreview("选择目标预览缩放超出支持范围。");
        const int w=int(width),h=int(height);
        costs->conversion(w,h,plane.common.resolution==dpi?0:raster.width,plane.common.resolution==dpi?0:raster.height,raster.tiles.size());
        const auto mapped=plane.common.resolution==dpi?raster:resolution_raster(raster,{0,0,raster.width,raster.height},w,h,dpi);
        Raster rgba(w,h,32,dpi);for(int y=0;y<h;++y)for(int x=0;x<w;++x){const auto pixel=raster_stroke_surface_pixel(mapped,x,y);if(pixel.a)rgba.set_pixel(x,y,pixel);}
        auto projected=plane;projected.raster=std::move(rgba);projected.common.width=w;projected.common.height=h;projected.common.depth=32;projected.common.resolution=dpi;
        return source(layer,projected,dpi,options,mask,costs);
    }
    const bool forced=options.force_density||(!mask&&layer.common.opacity_mode==2&&(raster.depth!=1||plane.common.resolution==dpi));
    if(!forced&&(layer.common.opacity<0||layer.common.opacity>100))throw std::invalid_argument("预览浓度超出支持范围。");
    Source result;result.x=layer.common.x;result.y=layer.common.y;
    result.mode=mask?Source::Mode::Mask:(plane.common.opacity_mode==5?Source::Mode::WhiteKey:Source::Mode::Normal);
    result.density=forced?255:unsigned(*legacy_rt_mul_div(255,layer.common.opacity,100));
    if(raster.depth==1&&!mask){
        result.scale=0;for(int scale:{1,2,4,8})if(plane.common.resolution==numeric_detail::binary64(dpi*scale))result.scale=scale;
        if(!result.scale||raster.color_order!=0)throw UnsupportedPreview("主线预览缩放尚未支持。");
        result.fixed=packed_color(plane.common.flags[4]?plane.common.color:0);
        result.mode=Source::Mode::Normal;
        if(result.scale==1&&layer.common.opacity_mode==2){result.density=255;result.missing_white=true;}
        if(result.scale==2||result.scale==4)result.density|=result.fixed.a;
    }else{
        if(!raster.can_edit()||plane.common.resolution!=dpi)throw UnsupportedPreview("此位深或分辨率的预览尚未支持。");
        if(!mask&&layer.common.opacity_mode==2){result.mode=Source::Mode::Opaque;result.missing_white=true;}
    }
    result.width=raster.width/result.scale+(result.scale==2&&raster.width%2!=0);result.height=raster.height/result.scale;
    result.raster=std::make_shared<const Raster>(raster);
    return result;
}
Source floating_raster_source(const Layer& layer,const Plane& plane,double dpi,AdapterCosts& costs){
    if(plane.runtime_text_cache&&plane.runtime_text_cache->image_geometry)
        throw UnsupportedPreview("Mixed-DPI image Text cache cannot supply a floating helper");
    if(!plane.raster)return source(layer,plane,dpi,{},false,&costs);
    const auto& raster=*plane.raster;
    int scale{};for(int value:{1,2,4,8})if(plane.common.resolution==numeric_detail::binary64(dpi*value))scale=value;
    if(!scale){auto result=source(layer,plane,dpi,{},false,&costs);result.native_helper=true;return result;}
    if(layer.opaque||plane.opaque||layer.common.resolution!=dpi||raster.resolution!=plane.common.resolution
        ||!raster_stroke_surface_supported(raster))throw UnsupportedPreview("浮动原生 helper 光栅元数据无效。");
    validate_storage(raster);
    const bool opaque=layer.common.opacity_mode==2&&scale==1;
    if(!opaque&&(layer.common.opacity<0||layer.common.opacity>100))throw std::invalid_argument("预览浓度超出支持范围。");
    Source result;result.raster=std::make_shared<const Raster>(raster);result.x=layer.common.x;result.y=layer.common.y;
    // A partial final sampling cell still reaches destination MoveCursor.
    result.scale=scale;result.width=(raster.width+scale-1)/scale;result.height=(raster.height+scale-1)/scale;
    result.native_helper=true;result.fixed=packed_color(plane.common.flags[4]?plane.common.color:0);
    result.density=opaque?255:unsigned(*legacy_rt_mul_div(255,layer.common.opacity,100));
    result.mode=opaque?Source::Mode::Opaque:plane.common.opacity_mode==5?Source::Mode::WhiteKey:Source::Mode::Normal;
    result.missing_white=opaque;
    if(scale==2||scale==4)result.density|=result.fixed.a;
    return result;
}
PreviewPixel sample(const Source& source,int x,int y){
    const auto& raster=*source.raster;
    if(raster.depth==1){
        const int sx=x*source.scale,sy=y*source.scale;
        const auto block=raster.tiles.find({sx/256,sy/256});
        if(block==raster.tiles.end())return source.missing_white?PreviewPixel{255,255,255,255}:PreviewPixel{};
        const auto offset=std::size_t(sy%256)*32+sx%256/8;
        const unsigned mask=(1u<<source.scale)-1,shift=8-source.scale-sx%8;
        unsigned count{};for(int row=0;row<source.scale;++row)count+=std::popcount((unsigned((*block->second)[offset+row*32])>>shift)&mask);
        auto value=source.fixed;value.a=std::uint8_t(((count*255/unsigned(source.scale*source.scale))*(source.density+1))>>8);return value;
    }
    const auto color=raster.pixel(x,y);
    if(source.mode==Source::Mode::Opaque)return {std::uint8_t(color.r>>8),std::uint8_t(color.g>>8),std::uint8_t(color.b>>8),255};
    if(source.mode==Source::Mode::WhiteKey&&color.r==65535&&color.g==65535&&color.b==65535)return {};
    const unsigned alpha=raster.depth==32?((unsigned(color.a>>8)*(source.density+1))>>8)
        :unsigned((std::uint64_t(color.a)*(257*source.density+1))>>24);
    if(source.mode==Source::Mode::Mask)return {std::uint8_t(alpha),std::uint8_t(alpha),std::uint8_t(alpha),255};
    return {std::uint8_t(color.r>>8),std::uint8_t(color.g>>8),std::uint8_t(color.b>>8),std::uint8_t(alpha)};
}
void materialize_helper(Raster& raster,TileCoord tile,std::uint64_t& storage){
    if(raster.tiles.contains(tile))return;
    if(storage>256ull*1024*1024||raster.tile_bytes()>256ull*1024*1024-storage)throw std::length_error("浮动 helper 累计存储超出支持范围。");
    storage+=raster.tile_bytes();raster.set_pixel(tile.x*256,tile.y*256,{});
}
PreviewPixel native_helper_pixel(PreviewPixel destination,const Source& source,int x,int y){
    namespace terminal=vector_render_detail::output_terminal;
    const auto& raster=*source.raster;const int sx=x*source.scale,sy=y*source.scale;
    const auto found=raster.tiles.find({sx/256,sy/256});
    const auto before=Pixel::rgba8(destination.r,destination.g,destination.b,destination.a);
    Pixel after=before;
    const auto color=std::uint64_t(source.fixed.r)*257|(std::uint64_t(source.fixed.g)*257<<16)
        |(std::uint64_t(source.fixed.b)*257<<32)|(std::uint64_t(source.fixed.a)*257<<48);
    if(found==raster.tiles.end()){
        if(source.missing_white)after=terminal::opaque_missing(before,source.tables.get());
    }else if(source.scale>1){
        if(raster.depth==1&&x<raster.width/source.scale+(source.scale==2&&raster.width%2)&&y<raster.height/source.scale){
            const unsigned mask=(1u<<source.scale)-1,shift=8-source.scale-sx%8;unsigned count{};
            const auto offset=std::size_t(sy%256)*32+sx%256/8;
            for(int row=0;row<source.scale;++row)count+=std::popcount((unsigned((*found->second)[offset+row*32])>>shift)&mask);
            if(source.stylos_score_pixels){
                auto ink=source.fixed;
                ink.a=std::uint8_t((255*count/unsigned(source.scale*source.scale))*(source.density+1)>>8);
                return stylos260::behind(destination,ink);
            }
            after=terminal::scaled_mask_behind_white(before,count,source.scale,source.density,color);
        }
    }else if(raster.depth==1||raster.depth==8){
        after=terminal::mask_behind_white(before,unsigned(raster.pixel(x,y).a/257),raster.depth,source.density,color);
    }else if(raster.depth==24){
        if(before.a&&before.a!=65535&&source.density){
            const auto offset=(std::size_t(y%256)*256+x%256)*3;
            if(offset+3>=found->second->size())throw UnsupportedPreview("原生 RGB24 helper 访问超出物理图块。");
            after=terminal::rgb24_behind_white(before,raster.pixel(x,y),(*found->second)[offset+3],source.density);
        }
    }else if(raster.depth==32||raster.depth==64){
        const auto value=raster.pixel(x,y);
        const auto* tables=source.remap_rgb?source.tables.get():nullptr;
        if(source.mode==Source::Mode::Opaque)after=terminal::opaque_present(before,value,raster.depth,32,tables);
        else {
            const auto sample=terminal::raster_source(value,raster.depth,32,source.density,source.mode==Source::Mode::WhiteKey,tables,source.direct_copy);
            if(source.stylos_score_pixels&&!source.direct_copy){
                const bool transparent_white=source.mode==Source::Mode::WhiteKey&&value.r==65535&&value.g==65535&&value.b==65535;
                if(!value.a||transparent_white)return destination;
                return stylos260::behind_nontransparent(destination,PreviewPixel{std::uint8_t(sample.r>>8),std::uint8_t(sample.g>>8),std::uint8_t(sample.b>>8),std::uint8_t(sample.a>>8)});
            }
            after=terminal::behind_white(before,sample,32,source.direct_copy);
        }
    }
    return {std::uint8_t(after.r>>8),std::uint8_t(after.g>>8),std::uint8_t(after.b>>8),std::uint8_t(after.a>>8)};
}
void render_source(PreviewImage& image,const Source& source,std::uint64_t& work,Raster* helper=nullptr,std::uint64_t* storage=nullptr,std::array<int,2> helper_origin={},std::stop_token stop={}){
    if(source.text_masks){
        const auto draw=[&](const text_font_detail::Mask& mask,std::uint32_t color){
            const auto ox=source.x+mask.origin[0],oy=source.y+mask.origin[1];
            const auto pigment=Pixel::rgba8(color&255,(color>>8)&255,(color>>16)&255,unsigned(*legacy_rt_mul_div(255,int(source.density),100)));
            for(const auto& [tile,bytes]:mask.raster.tiles){
                const auto clipped=intersect(area(image.bounds()),{ox+std::int64_t(tile.x)*256,oy+std::int64_t(tile.y)*256,std::min(ox+mask.raster.width,ox+std::int64_t(tile.x+1)*256),std::min(oy+mask.raster.height,oy+std::int64_t(tile.y+1)*256)});
                if(clipped.empty())continue;
                if(helper)for(auto y=(clipped.top-helper_origin[1])/256;y<=(clipped.bottom-1-helper_origin[1])/256;++y)for(auto x=(clipped.left-helper_origin[0])/256;x<=(clipped.right-1-helper_origin[0])/256;++x)materialize_helper(*helper,{int(x),int(y)},*storage);
                const auto amount=std::uint64_t(clipped.right-clipped.left)*(clipped.bottom-clipped.top);if(amount>work)throw std::invalid_argument("Text view exceeds cumulative work");work-=amount;
                for(auto y=clipped.top;y<clipped.bottom;++y){if(stop.stop_requested())throw std::runtime_error("Text view cancelled");
                    for(auto x=clipped.left;x<clipped.right;++x)if(mask.raster.pixel(int(x-ox),int(y-oy)).a){
                        const auto p=image.pixel(int(x),int(y));const auto after=vector_render_detail::output_terminal::behind_white(Pixel::rgba8(p.r,p.g,p.b,p.a),pigment,32);
                        image.set_pixel(int(x),int(y),{std::uint8_t(after.r>>8),std::uint8_t(after.g>>8),std::uint8_t(after.b>>8),std::uint8_t(after.a>>8)});
                    }
                }
            }
        };
        draw(source.text_masks->glyph,source.text_masks->text_color);if(source.text_masks->leader)draw(*source.text_masks->leader,source.text_masks->line_color);return;
    }
    const auto request=intersect(area(image.bounds()),{source.x,source.y,source.x+source.width,source.y+source.height});
    if(request.empty())return;
    const int tile_side=256/source.scale;
    const auto block=[&](TileCoord tile){
        if(stop.stop_requested())throw std::runtime_error("Floating helper render cancelled");
        const auto clipped=intersect(request,{source.x+std::int64_t(tile.x)*tile_side,source.y+std::int64_t(tile.y)*tile_side,
            source.x+(std::int64_t(tile.x)+1)*tile_side,source.y+(std::int64_t(tile.y)+1)*tile_side});
        if(clipped.empty())return;
        const auto amount=std::uint64_t(clipped.right-clipped.left)*(clipped.bottom-clipped.top);
        if(amount>work)throw std::invalid_argument("预览计算量超出支持上限。");work-=amount;
        if(helper)for(auto y=(clipped.top-helper_origin[1])/256;y<=(clipped.bottom-1-helper_origin[1])/256;++y)for(auto x=(clipped.left-helper_origin[0])/256;x<=(clipped.right-1-helper_origin[0])/256;++x)
            materialize_helper(*helper,{int(x),int(y)},*storage);
        for(auto y=clipped.top;y<clipped.bottom;++y){
        if(stop.stop_requested())throw std::runtime_error("Floating helper render cancelled");
        for(auto x=clipped.left;x<clipped.right;++x){
            if(source.native_helper){image.set_pixel(int(x),int(y),native_helper_pixel(image.pixel(int(x),int(y)),source,int(x-source.x),int(y-source.y)));continue;}
            const auto value=sample(source,int(x-source.x),int(y-source.y));
            if(value.a)image.set_pixel(int(x),int(y),preview_behind(image.pixel(int(x),int(y)),value));
        }}
    };
    if(source.missing_white){
        for(auto y=(request.top-source.y)/tile_side;y<=(request.bottom-1-source.y)/tile_side;++y)
            for(auto x=(request.left-source.x)/tile_side;x<=(request.right-1-source.x)/tile_side;++x)block({int(x),int(y)});
    }else for(const auto& [tile,bytes]:source.raster->tiles)block(tile);
}
Source helper_source(Raster raster){
    Source result;result.width=raster.width;result.height=raster.height;result.raster=std::make_shared<const Raster>(std::move(raster));return result;
}
Source reference_owner_terminal(Source result,const Layer& owner){
    //63C4A0: owner2 forces opaque100; every other admitted owner mode is
    //normal. The independent fresh cache carrier stays child mode1.
    result.mode=owner.common.opacity_mode==2?Source::Mode::Opaque:Source::Mode::Normal;
    result.density=vector_render_detail::output_terminal::density(owner.common.opacity_mode==2?100:owner.common.opacity,32);
    return result;
}
Source normal_line_source(const Layer& layer,double dpi,vector_render_detail::StoredLineCacheCosts& scene_costs,std::uint64_t storage_limit=512ull*1024*1024,bool plain=false,Rect request={},bool display_dpi=false,bool minimum_width=false){
    if((layer.common.depth!=32&&((!plain&&!display_dpi)||layer.common.depth!=64))||(!plain&&layer.common.opacity!=100)||layer.common.opacity<0||layer.common.opacity>100||layer.common.opacity_mode!=1
        ||(display_dpi&&!plain?(!std::isfinite(layer.common.resolution)||layer.common.resolution<=0):layer.common.resolution!=dpi)||layer.planes.empty())throw UnsupportedPreview("正式矢量主线缓存元数据尚未支持。");
    for(const auto& plane:layer.planes)if(plane.family!=0x21210001&&plane.family!=0x21220001)
        throw UnsupportedPreview("正式矢量主线缓存需要 Vert-Line 平面。");
    std::uint64_t wire=layer.specific.size();for(const auto& p:layer.planes)wire+=p.specific.size();
    if(scene_costs.storage>storage_limit||wire>(storage_limit-scene_costs.storage)/16)
        throw std::length_error("正式矢量主线预览源的累计存储超出支持范围。");
    const auto validations=std::max<std::size_t>(layer.planes.size(),1);
    if(scene_costs.work>128ull*1024*1024||wire>(128ull*1024*1024-scene_costs.work)/validations)
        throw std::length_error("正式矢量主线预览源的累计验证工作量超出支持范围。");
    scene_costs.work+=wire*validations;
    auto planes=vector_render_detail::decode_stored_line_planes(layer);
    VectorRenderContext context{plain?request.width:layer.common.width,plain?request.height:layer.common.height,display_dpi&&!plain?layer.common.depth:32,plain?dpi:layer.common.resolution,16,plain?VectorPoint{-double(layer.common.x),-double(layer.common.y)}:VectorPoint{},std::nullopt,2};
    context.minimum_line_width=minimum_width;
    // Hidden siblings participate in complete source and resource admission.
    scene_costs=plain?vector_render_detail::stored_line_output_costs(planes,context,vector_render_detail::StoredLineOutputSampling::AA2,scene_costs):context.depth==64?vector_render_detail::stored_line_print_cache_costs(planes,context,scene_costs):vector_render_detail::stored_line_cache_costs(planes,context,scene_costs);
    if(scene_costs.storage>storage_limit)throw std::length_error("Vector Line helper exceeds aggregate storage");
    std::uint64_t reserved{};for(auto& p:planes){reserved+=p.reserved_storage;p.reserved_storage=0;}
    for(std::size_t i=planes.size();i-->0;)if(!layer.planes[i].common.flags[1])planes.erase(planes.begin()+i);
    if(!planes.empty())planes.front().reserved_storage=reserved;
    auto result=helper_source(plain?vector_render_detail::render_stored_line_output(planes,context,vector_render_detail::StoredLineOutputSampling::AA2):context.depth==64?vector_render_detail::render_stored_line_print_cache(planes,context):vector_render_detail::render_stored_line_cache_planes(planes,context));
    //651080 NULL-overlay ->62D940 ->cache+70/63E860: nonmatching
    // DPI completes the actual32/64 cache, then the source1-only scaled/default
    // transfer writes nothing. Floating, Motion and reference callers keep
    // their separate explicit admission and terminal policies.
    if(display_dpi&&!plain&&layer.common.resolution!=dpi)result.width=result.height=0;
    if(plain){result.native_helper=true;result.density=vector_render_detail::output_terminal::density(layer.common.opacity,32);}
    result.x=plain?0:layer.common.x;result.y=plain?0:layer.common.y;return result;
}
template<class Decode,class Costs,class Render>
Source vector_reference_source(const Layer& layer,double dpi,vector_render_detail::StoredLineCacheCosts& scene_costs,std::stop_token stop,Decode decode,Costs output_costs,Render render){
    if((layer.common.depth!=32&&layer.common.depth!=64)||!std::isfinite(dpi)||dpi<=0||!std::isfinite(layer.common.resolution)||layer.common.resolution<=0||layer.common.opacity<0||layer.common.opacity>100||layer.common.opacity_mode<1||layer.common.opacity_mode>5)
        throw UnsupportedPreview("Stored vector reference source requires owner32-or64/finite positive DPI/density0..100/modes1..5");
    resolution_detail::Budget budget{scene_costs.storage,scene_costs.work,stop};budget.poll();
    std::uint64_t wire=layer.specific.size();for(const auto& p:layer.planes){budget.poll();wire+=p.specific.size();}
    const auto validations=std::max<std::size_t>(layer.planes.size(),1);
    if(wire>(modal_detail::max_bytes-budget.bytes)/16)throw std::length_error("Stored vector reference source storage exceeds limits");
    if(wire>(modal_detail::max_work-budget.work)/validations)throw std::length_error("Stored vector reference source validation exceeds limits");
    budget.visit(wire*validations);auto planes=decode(layer);budget.poll();
    //62EA60 inherits owner depth for its real cache;64DE10 consumes WORD
    //samples only after resolve. The final LightTable source stays32.
    const VectorRenderContext context{layer.common.width,layer.common.height,layer.common.depth,layer.common.resolution,16,{},std::nullopt,2};
    budget.allocate(sizeof(Source)+sizeof(Raster));const auto preceding_work=budget.work;
    const auto costs=output_costs(planes,context,{budget.bytes,budget.work},stop);
    budget.bytes=costs.storage;budget.work=costs.work;
    Raster cache(context.width,context.height,context.depth,context.resolution);
    if(layer.runtime_vector_cache_present){
        budget.visit(costs.work-preceding_work); //Cost admission and rendering both prepare.
        const int columns=(context.width+255)/256,rows=(context.height+255)/256;
        budget.allocate(std::uint64_t(columns)*rows*128);
        std::uint64_t reserved{};for(auto& p:planes){reserved+=p.reserved_storage;p.reserved_storage=0;}
        for(std::size_t i=planes.size();i-->0;)if(!layer.planes[i].common.flags[1])planes.erase(planes.begin()+i);
        if(!planes.empty())planes.front().reserved_storage=reserved;
        cache=render(planes,context,stop);
        //Normal copy consumes even A0 samples. Preserve fresh-load white
        //backing in every logical block, independently of geometry sparsity.
        for(int y=0;y<rows;++y)for(int x=0;x<columns;++x){budget.poll();if(!cache.tiles.contains({x,y})){
            budget.visit(65536);cache.set_pixel(x*256,y*256,{});
        }}
    }
    budget.poll();scene_costs={budget.bytes,budget.work};auto result=helper_source(std::move(cache));
    result.native_helper=true;result.x=layer.common.x;result.y=layer.common.y;
    //63E860's nonmatching-DPI wrappers only consume source1; other
    //selectors default. Actual32/64 caches retain complete preparation and
    //root counting, but no transfer, including opaque/global-copy cases.
    if(context.resolution!=dpi)result.width=result.height=0;
    return reference_owner_terminal(std::move(result),layer);
}
Source ordinary_vector_source(const Layer& layer,double dpi,vector_render_detail::StoredLineCacheCosts& costs,std::stop_token stop){
    using namespace vector_render_detail;
    const auto& c=layer.common;VectorRenderContext context{c.width,c.height,c.depth,c.resolution,16,{},std::nullopt,2};
    Source result;
    if(layer.family==0x21200000||layer.family==0x20800000){auto raster=paint_cache_for_print(layer,context,costs,stop);if(raster)result=helper_source(std::move(*raster));}
    else if(layer.family==0x40004000)return vector_reference_source(layer,dpi,costs,stop,decode_vector_ruler_geometries,ruler_output_costs,render_vector_ruler_geometries);
    else if(layer.family==0x40002000)return vector_reference_source(layer,dpi,costs,stop,decode_vector_shape_geometries,shape_output_costs,render_vector_shape_geometries);
    else{
        auto planes=decode_stored_line_planes(layer);costs=stored_line_output_costs(planes,context,StoredLineOutputSampling::AA2,costs,stop);
        std::uint64_t reservation{};for(auto& p:planes){reservation+=p.reserved_storage;p.reserved_storage=0;}
        for(std::size_t i=planes.size();i-->0;)if(!layer.planes[i].common.flags[1])planes.erase(planes.begin()+i);
        if(!planes.empty())planes.front().reserved_storage=reservation;
        if(layer.runtime_vector_cache_present)result=helper_source(render_stored_line_output(planes,context,StoredLineOutputSampling::AA2,stop));
    }
    result.x=c.x;result.y=c.y;if(c.resolution!=dpi)result.width=result.height=0;return result;
}
VectorRenderContext direct_reference_context(const Layer& layer,Rect page,double dpi,bool aa2){return vector_render_detail::direct_output_context(layer,page,dpi,32,aa2?2u:1u);}

Source line_reference_source(const Layer& layer,Rect page,double dpi,vector_render_detail::StoredLineCacheCosts& scene_costs,std::stop_token stop,bool aa2){
    using namespace vector_render_detail;
    if((layer.common.depth!=32&&layer.common.depth!=64)||!std::isfinite(dpi)||dpi<=0||!std::isfinite(layer.common.resolution)||layer.common.resolution<=0||layer.common.opacity<0||layer.common.opacity>100||layer.common.opacity_mode<1||layer.common.opacity_mode>5)
        throw UnsupportedPreview("Stored Line reference source requires owner32-or64/finite positive DPI/density0..100/modes1..5");
    resolution_detail::Budget budget{scene_costs.storage,scene_costs.work,stop};budget.poll();
    std::uint64_t wire=layer.specific.size();for(const auto& p:layer.planes){budget.poll();wire+=p.specific.size();}
    const auto validations=std::max<std::size_t>(layer.planes.size(),1);
    if(wire>(modal_detail::max_bytes-budget.bytes)/16)throw std::length_error("Stored Line reference source storage exceeds limits");
    if(wire>(modal_detail::max_work-budget.work)/validations)throw std::length_error("Stored Line reference source validation exceeds limits");
    budget.visit(wire*validations);auto planes=decode_stored_line_planes(layer);budget.poll();
    //62F4A0 redraws the target-sized temporary with owner XY in its matrix,
    //then transfers it at XY0. Actual target32 chooses material/typed depth;
    //owner32/64 and archived bounds do not select the temporary format.
    const auto context=direct_reference_context(layer,page,dpi,aa2);
    const auto sampling=aa2?StoredLineOutputSampling::AA2:StoredLineOutputSampling::Plain1x;
    budget.allocate(sizeof(Source)+sizeof(Raster));
    const auto costs=stored_line_reference_costs(planes,context,sampling,{budget.bytes,budget.work},stop);
    budget.bytes=costs.storage;budget.work=costs.work; //Includes both preparations.
    //The native direct redraw dereferences its carrier unconditionally. Keep
    //that unsafe absent-carrier input outside this bounded reconstruction.
    if(!layer.runtime_vector_cache_present)throw UnsupportedPreview("Stored Line reference redraw requires a runtime cache carrier");
    const int columns=(context.width+255)/256,rows=(context.height+255)/256;
    budget.allocate(std::uint64_t(columns)*rows*128);
    std::uint64_t reserved{};for(auto& p:planes){reserved+=p.reserved_storage;p.reserved_storage=0;}
    for(std::size_t i=planes.size();i-->0;)if(!layer.planes[i].common.flags[1])planes.erase(planes.begin()+i);
    if(!planes.empty())planes.front().reserved_storage=reserved;
    auto cache=render_stored_line_reference(planes,context,sampling,stop);
    for(int y=0;y<rows;++y)for(int x=0;x<columns;++x){budget.poll();if(!cache.tiles.contains({x,y})){
        budget.visit(65536);cache.set_pixel(x*256,y*256,{});
    }}
    budget.poll();scene_costs={budget.bytes,budget.work};auto result=helper_source(std::move(cache));result.native_helper=true;return reference_owner_terminal(std::move(result),layer);
}
Source paint_reference_source(const Layer& layer,Rect page,double dpi,vector_render_detail::StoredLineCacheCosts& scene_costs,std::stop_token stop,bool aa2){
    if((layer.common.depth!=32&&layer.common.depth!=64)||!std::isfinite(dpi)||dpi<=0||!std::isfinite(layer.common.resolution)||layer.common.resolution<=0||layer.common.opacity<0||layer.common.opacity>100||layer.common.opacity_mode<1||layer.common.opacity_mode>5)
        throw UnsupportedPreview("Paint reference source requires owner32-or64/finite positive DPI/density0..100/modes1..5");
    resolution_detail::Budget budget{scene_costs.storage,scene_costs.work,stop};budget.poll();
    std::uint64_t wire=layer.specific.size();for(const auto& p:layer.planes){budget.poll();wire+=p.specific.size();}
    const auto validations=std::max<std::size_t>(layer.planes.size(),1);
    if(wire>(modal_detail::max_bytes-budget.bytes)/16)throw std::length_error("Paint reference source storage exceeds limits");
    if(wire>(modal_detail::max_work-budget.work)/validations)throw std::length_error("Paint reference source validation exceeds limits");
    budget.visit(wire*validations);budget.allocate(sizeof(Source)+sizeof(Raster));scene_costs={budget.bytes,budget.work};
    const auto context=direct_reference_context(layer,page,dpi,aa2);
    auto cache=vector_render_detail::paint_reference_redraw(layer,context,scene_costs,stop);
    auto result=helper_source(std::move(cache));result.native_helper=true;return reference_owner_terminal(std::move(result),layer);
}
void validate_floating_preview(const DocumentData& data,const FloatingSelection& floating){
    if(floating.target)validate_targeted_floating(data,floating);
    else if(floating.same_document_source)validate_local_floating(data,floating);
    else validate_foreign_floating(data,floating);
}
Rect helper_storage(Rect page,const FloatingSelection& floating){
    if(floating.same_document_source)return page;
    // Plain-paste sub_61CE10 unions the page with the rebased source frame
    // before allocating its helper (PM:894473-894483,894667-894679).
    // Local MoveImage1 instead retains its original page-sized helper.
    const auto a=area(page),b=area(floating.capture_frame);
    const Area storage{std::min(a.left,b.left),std::min(a.top,b.top),std::max(a.right,b.right),std::max(a.bottom,b.bottom)};
    if(storage.right-storage.left>std::numeric_limits<int>::max()||storage.bottom-storage.top>std::numeric_limits<int>::max())
        throw std::invalid_argument("浮动预览存储范围超出支持上限。");
    return rectangle(storage);
}
Raster copy_paint_helper(const Raster& source,Rect storage,Rect frame,std::array<int,2> owner,double dpi,AdapterCosts& costs,std::uint64_t& work){
    Raster destination(storage.width,storage.height,32,dpi);validate_storage(source);
    const std::int64_t dx=std::int64_t(owner[0])-storage.x,dy=std::int64_t(owner[1])-storage.y;
    const auto raw=area(frame);
    if(source.depth==32&&dx==0&&dy==0&&raw.left==storage.x&&raw.top==storage.y&&raw.right==std::int64_t(storage.x)+storage.width
        &&raw.bottom==std::int64_t(storage.y)+storage.height&&source.width==storage.width&&source.height==storage.height){
        const auto visits=std::uint64_t(source.tiles.size())*65536;
        if(visits>work)throw std::length_error("PaintMono helper copy exceeds work budget");work-=visits;
        for(const auto& [tile,bytes]:source.tiles){materialize_helper(destination,tile,costs.bytes);destination.tiles.at(tile)=std::make_shared<Bytes>(*bytes);}
        return destination;
    }
    const auto request=intersect(intersect({raw.left-owner[0],raw.top-owner[1],raw.right-owner[0],raw.bottom-owner[1]},
        {0,0,source.width,source.height}),{-dx,-dy,std::int64_t(storage.width)-dx,std::int64_t(storage.height)-dy});
    if(request.empty())return destination;
    const auto amount=std::uint64_t(request.right-request.left)*(request.bottom-request.top);
    if(amount>work)throw std::length_error("PaintMono helper copy exceeds work budget");work-=amount;
    const auto visit=[&](std::int64_t x,std::int64_t y){if(x>=0&&y>=0&&x<destination.width&&y<destination.height)materialize_helper(destination,{int(x)/256,int(y)/256},costs.bytes);};
    const auto region=[&](Area a){
        for(auto y=a.top/256;y<=(a.bottom-1)/256;++y)for(auto x=a.left/256;x<=(a.right-1)/256;++x)materialize_helper(destination,{int(x),int(y)},costs.bytes);
    };
    std::vector<TileCoord> erase;
    for(auto ty=request.top/256;ty<=(request.bottom-1)/256;++ty)for(auto tx=request.left/256;tx<=(request.right-1)/256;++tx){
        const auto piece=intersect(request,{tx*256,ty*256,(tx+1)*256,(ty+1)*256});
        const Area output{piece.left+dx,piece.top+dy,piece.right+dx,piece.bottom+dy};
        const bool present=source.tiles.contains({int(tx),int(ty)});
        if(source.depth==32&&dx%256==0&&dy%256==0&&piece.right-piece.left==256){
            if(!present&&piece.bottom-piece.top==256){erase.push_back({int(output.left)/256,int(output.top)/256});continue;}
            region(output);
        }else if(source.depth==32&&dx%256==0){
            for(auto y=output.top;y<=output.bottom;++y)visit(output.left,y);
        }else if(source.depth==32&&present){
            region(output);for(auto y=output.top;y<output.bottom;++y)visit(output.right,y);
        }else if(source.depth==32){region(output);}
        else{
            region(output);
            if(!present&&dx%256)for(auto y=output.top;y<output.bottom;++y)visit(output.right,y);
        }
        for(auto y=piece.top;y<piece.bottom;++y)for(auto x=piece.left;x<piece.right;++x){
            const auto value=present?raster_stroke_surface_pixel(source,int(x),int(y)):Pixel::rgba8(255,255,255,0);
            destination.set_pixel(int(x+dx),int(y+dy),Pixel::rgba8(value.r>>8,value.g>>8,value.b>>8,value.a>>8));
        }
    }
    for(auto tile:erase)destination.tiles.erase(tile);
    return destination;
}
}
class FloatingPreview {
public:
    FloatingKind kind{};
    std::shared_ptr<const std::vector<Layer>> payload;
    Rect page,frame,storage;
    double dpi{};
    bool plane_mode{};
    bool foreign{};
    // Ordinary helper plane, or the fresh Paint/Line helper planes. Their
    // density, visibility and mode are defaults, not copied owner properties.
    std::vector<std::pair<std::uint32_t,Source>> sources;
};
static std::shared_ptr<const FloatingPreview> prepare_floating_preview_impl(const DocumentData& data,const FloatingSelection& floating,PreviewOptions options,std::stop_token stop={}){
    if(stop.stop_requested())throw std::runtime_error("Floating helper preparation cancelled");
    validate_floating_preview(data,floating);
    if(!floating.layers||floating.layers->empty()
        ||(floating.kind!=FloatingKind::General&&floating.kind!=FloatingKind::PaintMono&&floating.kind!=FloatingKind::TargetedPaintMono))
        throw std::invalid_argument("此来源或绑定的浮动预览尚未支持。");
    auto preview=std::make_shared<FloatingPreview>();preview->kind=floating.kind;preview->payload=floating.layers;
    preview->page={0,0,data.base.width(),data.base.height()};preview->frame=floating.tight_bounds;preview->dpi=data.base.dpi;preview->plane_mode=options.plane_mode;
    preview->foreign=!floating.same_document_source;preview->storage=helper_storage(preview->page,floating);
    const auto work_area=rectangle(intersect(area(preview->storage),area(preview->frame)));
    std::uint64_t work=128ull*1024*1024;
    AdapterCosts adapters;
    vector_render_detail::StoredLineCacheCosts vector_costs;
    if(floating.kind!=FloatingKind::PaintMono){
        //The final flattened→helper copy visits the whole frame even where
        //all sources are absent. Reserve it before source work or allocation.
        const auto final_visits=std::uint64_t(work_area.width)*work_area.height;
        if(final_visits>work)throw std::length_error("Floating helper frame exceeds work budget");work-=final_visits;
        if(final_visits>256ull*1024*1024/sizeof(PreviewPixel))throw std::length_error("Floating helper scratch exceeds storage budget");
        adapters.bytes=final_visits*sizeof(PreviewPixel);
        PreviewImage flattened(work_area);
        Raster raster(preview->storage.width,preview->storage.height,32,preview->dpi);
        const std::array<int,2> helper_origin{preview->storage.x,preview->storage.y};
        const auto& layers=*floating.layers;
        const auto raster_source=[&](const Layer& layer,const Plane& plane){
            adapters.work=128ull*1024*1024-work;
            auto result=floating_raster_source(layer,plane,data.base.dpi,adapters);
            work=128ull*1024*1024-adapters.work;return result;
        };
        // sub_61BAE0 uses a cleared context. Its restricted branch directly
        // renders plane0 of clone0, bypassing the current-layer mask gate.
        if(options.plane_mode){
            if(!layers.front().planes.empty()&&layers.front().planes.front().common.flags[1])
                render_source(flattened,raster_source(layers.front(),layers.front().planes.front()),work,&raster,&adapters.bytes,helper_origin,stop);
        }else for(std::size_t i=layers.size();i-->0;){
            const auto& layer=layers[i];if(!layer.common.flags[1])continue;
            if((layer.family&0xf0000000u)==0x20000000u||selection_detail::extension_move_owner(layer)){
                vector_costs.work=128ull*1024*1024-work;vector_costs.storage=adapters.bytes;Source helper;
                if(layer.common.depth==32&&(layer.family==0x22100000||layer.family==0x24400000)&&std::ranges::all_of(layer.planes,[](const Plane& p){return p.family==0x21210001||p.family==0x21220001;})){
                    helper=normal_line_source(layer,data.base.dpi,vector_costs,256ull*1024*1024);
                    work=128ull*1024*1024-vector_costs.work;
                }else{
                    auto bytes=256ull*1024*1024-vector_costs.storage;
                    if(selection_detail::extension_move_owner(layer))selection_detail::validate_extension_move(layer,data.base.dpi,bytes,work);
                    else selection_detail::validate_vector_move(layer,data.base.dpi,bytes,work);
                    vector_costs.storage=256ull*1024*1024-bytes;
                    helper=helper_source(selection_detail::owner_helper(layer,data.base.dpi));helper.x=layer.common.x;helper.y=layer.common.y;
                }
                adapters.bytes=vector_costs.storage;
                render_source(flattened,helper,work,&raster,&adapters.bytes,helper_origin,stop);continue;
            }
            for(std::size_t p=layer.planes.size();p-->0;)if(layer.planes[p].common.flags[1])
                render_source(flattened,raster_source(layer,layer.planes[p]),work,&raster,&adapters.bytes,helper_origin,stop);
        }
        for(int y=work_area.y;y<work_area.y+work_area.height;++y){
        if(stop.stop_requested())throw std::runtime_error("Floating helper preparation cancelled");
        for(int x=work_area.x;x<work_area.x+work_area.width;++x){
            const auto value=flattened.pixel(x,y);
            if(value.a)raster.set_pixel(x-preview->storage.x,y-preview->storage.y,Pixel::rgba8(value.r,value.g,value.b,value.a));
        }}
        auto helper=helper_source(std::move(raster));helper.x=preview->storage.x;helper.y=preview->storage.y;
        preview->sources.emplace_back(floating.kind==FloatingKind::TargetedPaintMono?0x12100001:0x10000001,std::move(helper));
    }else{
        if(floating.layers->size()!=1||floating.layers->front().family!=0x11000800)throw std::invalid_argument("PaintMono 浮动载荷无效。");
        const auto& layer=floating.layers->front();
        if(layer.opaque||layer.planes.empty()||layer.planes.size()>2)throw std::invalid_argument("PaintMono 浮动载荷无效。");
        bool paint=false,line=false;
        for(const auto& plane:layer.planes){
            if(plane.opaque||(plane.family!=0x12100001&&plane.family!=0x11110002))throw std::invalid_argument("PaintMono 浮动平面类型无效。");
            auto& seen=plane.family==0x12100001?paint:line;if(seen)throw std::invalid_argument("PaintMono 浮动平面类型重复。");seen=true;
        }
        for(const auto family:{0x12100001u,0x11110002u}){
            Raster raster(preview->storage.width,preview->storage.height,32,preview->dpi);
            const auto found=std::find_if(layer.planes.begin(),layer.planes.end(),[&](const Plane& plane){return plane.family==family;});
            if(found!=layer.planes.end()){
                if(!found->raster||!raster_stroke_surface_supported(*found->raster)||found->raster->resolution!=found->common.resolution
                    ||!std::isfinite(found->common.resolution)||found->common.resolution<=0||layer.common.resolution!=data.base.dpi)throw std::invalid_argument("PaintMono 浮动复制类型尚未支持。");
                validate_storage(*found->raster);
                raster=copy_paint_helper(*found->raster,preview->storage,preview->frame,{layer.common.x,layer.common.y},preview->dpi,adapters,work);
            }
            auto helper=helper_source(std::move(raster));helper.x=preview->storage.x;helper.y=preview->storage.y;
            preview->sources.emplace_back(family,std::move(helper));
        }
    }
    if(stop.stop_requested())throw std::runtime_error("Floating helper preparation cancelled");
    return preview;
}
std::shared_ptr<const FloatingPreview> prepare_floating_preview(const DocumentData& data,const FloatingSelection& floating,PreviewOptions options){
    return prepare_floating_preview_impl(data,floating,options);
}
bool floating_preview_detail::transform_has_blocks(const DocumentData& data,const FloatingSelection& floating,
    std::uint64_t& work_remaining,std::uint64_t bytes_available,std::stop_token stop){
    const auto poll=[&]{if(stop.stop_requested())throw std::runtime_error("Transform helper probe cancelled");};poll();
    if(floating.kind!=FloatingKind::General||!floating.same_document_source||floating.target||!floating.layers||floating.layers->empty())
        throw UnsupportedPreview("Transform helper probe requires local General raster content");
    validate_floating_preview(data,floating);
    const Rect page{0,0,data.base.width(),data.base.height()};
    const auto frame=intersect(area(floating.tight_bounds),area(page));
    const auto work_limit=std::min<std::uint64_t>(work_remaining,128ull*1024*1024),storage_limit=std::min<std::uint64_t>(bytes_available,256ull*1024*1024);
    std::uint64_t work{},storage{};
    const auto visit=[&](std::uint64_t amount){poll();if(amount>work_limit-work)throw std::length_error("Transform helper exceeds aggregate work budget");work+=amount;};
    const auto allocate=[&](std::uint64_t amount){poll();if(amount>storage_limit-storage)throw std::length_error("Transform helper exceeds aggregate storage budget");storage+=amount;};
    // Input maps are visited and shallow-copied even when their backing lies
    // outside the frame. Source overlaps add work; helper allocations union.
    allocate(sizeof(FloatingPreview)+2*sizeof(Raster)+sizeof(std::set<TileCoord>)+512);
    if(!frame.empty())visit(std::uint64_t(frame.right-frame.left)*(frame.bottom-frame.top));
    if(!frame.empty())allocate(std::uint64_t(frame.right-frame.left)*(frame.bottom-frame.top)*sizeof(PreviewPixel));
    std::set<TileCoord> helper_tiles;
    for(const auto& layer:*floating.layers){
        poll();
        if(layer.opaque||(layer.family&0xf0000000u)!=0x10000000u||layer.common.resolution!=data.base.dpi||layer.common.opacity!=100||layer.common.opacity_mode!=1)
            throw UnsupportedPreview("Transform helper owner is outside the normal raster profile");
        for(const auto& plane:layer.planes){
            poll();
            if(plane.opaque||!plane.raster||!plane.raster->can_edit()||plane.common.opacity!=100||plane.common.opacity_mode!=1||plane.common.resolution!=data.base.dpi||plane.raster->resolution!=data.base.dpi
                ||plane.raster->width!=plane.common.width||plane.raster->height!=plane.common.height||plane.raster->depth!=plane.common.depth)
                throw UnsupportedPreview("Transform helper plane is outside the normal raster profile");
            const auto& raster=*plane.raster;validate_storage(raster);
            if(!layer.common.flags[1]||!plane.common.flags[1])continue;
            allocate(sizeof(Source)+sizeof(Raster)+raster.tiles.size()*128);visit(raster.tiles.size()*4+1);
            for(const auto& [tile,bytes]:raster.tiles){
                poll();const auto left=std::int64_t(tile.x)*256,top=std::int64_t(tile.y)*256;
                const auto piece=intersect(frame,{layer.common.x+left,layer.common.y+top,
                    layer.common.x+std::min(left+256,std::int64_t(raster.width)),layer.common.y+std::min(top+256,std::int64_t(raster.height))});
                if(piece.empty())continue;
                visit(std::uint64_t(piece.right-piece.left)*(piece.bottom-piece.top));
                for(auto y=piece.top/256;y<=(piece.bottom-1)/256;++y)for(auto x=piece.left/256;x<=(piece.right-1)/256;++x){
                    const TileCoord key{int(x),int(y)};
                    if(!helper_tiles.contains(key)){allocate(256*256*4+3*128);helper_tiles.insert(key);}
                }
            }
        }
    }
    for(const auto tile:helper_tiles){
        visit(std::uint64_t(std::min(256,page.width-tile.x*256))*std::min(256,page.height-tile.y*256)+3);
    }
    if(!frame.empty())visit(std::uint64_t((frame.right-1)/256-frame.left/256+1)*((frame.bottom-1)/256-frame.top/256+1));
    const auto prepared=prepare_floating_preview_impl(data,floating,{},stop);
    bool found{};
    for(const auto& [family,source]:prepared->sources){
        // Optimize is block-local. A one-node shallow view uses the real
        // raster Optimize worker while bounding cancellation to one block.
        for(const auto& [tile,backing]:source.raster->tiles){
            poll();Raster block(source.raster->width,source.raster->height,32,source.raster->resolution);
            block.tiles.emplace(tile,backing);block.optimize_storage();poll();
            if(!block.tiles.empty()&&!intersect(area(prepared->frame),
                {source.x+std::int64_t(tile.x)*256,source.y+std::int64_t(tile.y)*256,
                 source.x+std::min(std::int64_t(tile.x+1)*256,std::int64_t(block.width)),source.y+std::min(std::int64_t(tile.y+1)*256,std::int64_t(block.height))}).empty())found=true;
        }
    }
    poll();work_remaining-=work;return found;
}
RegionSelection prepare_paste_content_selection(const DocumentData& data,const FloatingSelection& floating,const RegionSelection& carrier){
    if(floating.same_document_source||floating.sources||floating.delta!=std::array<int,2>{})
        throw std::invalid_argument("粘贴内容选区需要已规范化的外部载荷。");
    const auto size=selection_carrier_size(carrier,data.base.width(),data.base.height());
    const auto prepared=prepare_floating_preview(data,floating);
    if(prepared->storage.x||prepared->storage.y)throw std::invalid_argument("粘贴 helper 原点尚未规范化。");
    auto mask=std::make_shared<BinaryMask>(size[0],size[1]);
    const auto& first=*prepared->sources.front().second.raster;
    const Raster* second=prepared->kind==FloatingKind::PaintMono?prepared->sources.at(1).second.raster.get():nullptr;
    const auto request=intersect(area(prepared->frame),{0,0,first.width,first.height});
    std::uint64_t work=128ull*1024*1024;
    if(!request.empty())for(auto ty=request.top/256;ty<=(request.bottom-1)/256;++ty)for(auto tx=request.left/256;tx<=(request.right-1)/256;++tx){
        const TileCoord tile{int(tx),int(ty)};const bool a=first.tiles.contains(tile),b=second&&second->tiles.contains(tile);
        if(!a&&!b)continue;
        const auto piece=intersect(request,{tx*256,ty*256,(tx+1)*256,(ty+1)*256});
        // Native MoveCursor at the helper-piece start gates the entire piece.
        // Safe logical writes retain its valid tile, ignoring physical tails.
        if(piece.left>=size[0]||piece.top>=size[1])continue;
        mask->materialize_block(int(piece.left)/256,int(piece.top)/256);
        const auto right=std::min<std::int64_t>(piece.right,size[0]),bottom=std::min<std::int64_t>(piece.bottom,size[1]);
        const auto visits=std::uint64_t(right-piece.left)*(bottom-piece.top);
        if(visits>work)throw std::length_error("粘贴内容 mask 扫描超出支持范围。");work-=visits;
        const auto selected=[&](const Raster& raster,int x,int y){
            const auto p=raster.pixel(x,y);return p.a&&(prepared->kind!=FloatingKind::PaintMono||p.r!=65535||p.g!=65535||p.b!=65535);
        };
        for(auto y=piece.top;y<bottom;++y)for(auto x=piece.left;x<right;++x)
            if((a&&selected(first,int(x),int(y)))||(b&&selected(*second,int(x),int(y))))mask->set(int(x),int(y));
    }
    auto result=cleared_selection({},size);result.vector_flags=carrier.vector_flags;result.carrier_dpi=carrier.carrier_dpi;
    const auto bounds=mask->extent({0,0,size[0],size[1]});
    if(bounds.width>0&&bounds.height>0){result.state=SelectionLayerState::Mask;result.bounds=bounds;result.mask=std::move(mask);}
    return result;
}
struct PreviewScene::Impl {
    Rect page,floating;
    std::vector<Source> sources;
    struct VectorExtent {std::uint64_t identity;Bytes specific;Rect extent;};
    std::vector<VectorExtent> vector_extents;
    std::optional<PreviewPixel> background;
    bool reference_direct{};
    std::uint64_t render_storage{};
    std::uint64_t render_work{128ull*1024*1024};
    std::stop_token stop;
};
PreviewScene::PreviewScene(const DocumentData& data,const FloatingSelection* floating,std::shared_ptr<const FloatingPreview> prepared,PreviewOptions options){
    if(options.reference_vector_aa2&&(!options.reference_source||!options.reference_qt_text_fonts||options.motion_check||options.plane_mode))
        throw UnsupportedPreview("Vector quality is restricted to the explicit Qt reference source");
    if(data.unrestored_external_content)throw UnsupportedPreview("预览的外部图像内容尚未恢复。");
    auto scene=std::make_shared<Impl>();scene->page={0,0,data.base.width(),data.base.height()};
    if(options.motion_check)scene->stop=options.motion_stop;else if(options.reference_source)scene->stop=options.reference_stop;
    if(options.motion_check){options.reference_source=true;options.reference_direct_copy=false;}
    if(options.reference_source){
        const Layer* single{};std::size_t count{};
        for(const auto& layer:data.layers)if(layer.common.flags[1]){
            switch(layer.family){case 0x40000200:case 0x40001000:case 0x40004000:case 0x40008000:case 0x40010000:case 0x40020000:continue;default:break;}
            ++count;single=&layer;
        }
        scene->reference_direct=count==1&&single->planes.size()==1;
        if(options.reference_direct_copy)scene->reference_direct=*options.reference_direct_copy;
    }
    const bool shared_sources=options.motion_check||(options.reference_source&&options.reference_qt_text_fonts);
    vector_render_detail::StoredLineCacheCosts line_costs;if(shared_sources){line_costs={options.resources[0],options.resources[1]};if(line_costs.storage>512ull*1024*1024||line_costs.work>128ull*1024*1024)throw std::length_error("Image source resource seed exceeds limits");}
    const auto remaps=options.plane_mode?adjust_render_detail::OwnerTables{}:adjust_render_detail::prepare(data,line_costs.storage,line_costs.work,scene->stop);
    if(floating&&floating->active()){
        validate_floating_preview(data,*floating);
        if(!prepared)prepared=prepare_floating_preview(data,*floating,options);
        if(prepared->payload!=floating->layers||prepared->kind!=floating->kind||prepared->page!=scene->page||prepared->frame!=floating->tight_bounds
            ||prepared->dpi!=data.base.dpi||prepared->plane_mode!=options.plane_mode||prepared->foreign==floating->same_document_source
            ||prepared->storage!=helper_storage(scene->page,*floating))throw std::invalid_argument("浮动预览缓存与当前载荷不匹配。");
        auto bounds=area(floating->tight_bounds);bounds.left+=floating->delta[0];bounds.right+=floating->delta[0];bounds.top+=floating->delta[1];bounds.bottom+=floating->delta[1];
        scene->floating=rectangle(intersect(bounds,area(scene->page)));
        if(floating->kind==FloatingKind::General){
            auto helper=prepared->sources.front().second;helper.x+=floating->delta[0];helper.y+=floating->delta[1];scene->sources.push_back(std::move(helper));
        }
    }else prepared.reset();
    if(options.background){const auto color=*options.background;scene->background=PreviewPixel{std::uint8_t(color.r>>8),std::uint8_t(color.g>>8),std::uint8_t(color.b>>8),std::uint8_t(color.a>>8)};}
    AdapterCosts adapters;
    TextImageContext text_image{line_costs,{},0};
    for(std::size_t i=data.layers.size();i-->0;){
        const auto& saved=data.layers[i];std::optional<Layer> forced;
        if(options.motion_check){
            if(saved.family==0x10008000||(!options.motion_show_shadow&&saved.family==0x10040000))continue;
            if(options.motion_only_line){if(!(saved.family&0x01000000u))continue;forced=saved;if(!(saved.family&0x20000000u))forced->common.flags[1]=1;for(auto& p:forced->planes)p.common.flags[1]=(p.family&0x00010000u)!=0;}
            else if(saved.family==0x11000800||saved.family==0x11001000){forced=saved;forced->common.flags[1]=1;forced->common.opacity=100;for(auto& p:forced->planes)p.common.flags[1]=1;}
        }
        const auto& layer=forced?*forced:saved;if(!layer.common.flags[1])continue;
        const auto insert_targeted=[&](std::size_t p){if(prepared&&prepared->kind==FloatingKind::TargetedPaintMono&&floating->target==FloatingTarget{i,p}){
            auto helper=prepared->sources.front().second;helper.x+=floating->delta[0];helper.y+=floating->delta[1];scene->sources.push_back(std::move(helper));}};
        if(options.color_check&&color_check_excludes(layer.family))continue;
        if(layer.family==0x40000200||layer.family==0x40001000)continue;
        if(options.stylos_score_pixels&&(layer.family==0x40008000||layer.family==0x40010000||layer.family==0x40020000))continue;
        if(layer.opaque)throw UnsupportedPreview("预览包含尚未支持的图层。");
        const auto mapped=[&](Source value,bool rgb=true){
            if(!options.plane_mode&&!remaps.empty()){value.tables=remaps[i];value.remap_rgb=rgb;value.native_helper=value.native_helper||bool(value.tables);}
            if(options.motion_check)value.native_helper=true;
            value.stylos_score_pixels=options.stylos_score_pixels;
            return value;
        };
        const auto mapped_plane=[&](const Plane& p){
            auto value=source(layer,p,data.base.dpi,options,false,&adapters,true,&text_image);
            if(shared_sources){
                if(adapters.bytes>512ull*1024*1024-line_costs.storage||adapters.work>128ull*1024*1024-line_costs.work)throw std::length_error("Image source adapters exceed aggregate resources");
                line_costs.storage+=adapters.bytes;line_costs.work+=adapters.work;adapters={};
            }
            //Native Text does not read the Adjust RGB tables in its packet.
            if(!value.text_masks&&!(p.runtime_text_cache&&p.runtime_text_cache->static_geometry&&text_raster_cache(layer,p,data.base.dpi)))
                value=mapped(std::move(value),!p.raster||p.raster->can_edit());
            if(options.reference_source){
                value.native_helper=true;
                value.direct_copy=scene->reference_direct&&p.raster&&p.raster->can_edit()&&p.common.resolution==data.base.dpi&&layer.common.opacity_mode!=2;
            }
            return value;
        };
        if(prepared&&prepared->kind==FloatingKind::PaintMono&&layer.family==0x11000800){
            // Insert by family before every original plane, even if that
            // original plane is hidden. The helper owns its own visibility.
            for(std::size_t p=layer.planes.size();p-->0;){
                const auto& plane=layer.planes[p];
                for(const auto& [family,helper]:prepared->sources)if(plane.family==family){
                    auto translated=helper;translated.x=floating->delta[0];translated.y=floating->delta[1];scene->sources.push_back(std::move(translated));break;
                }
                insert_targeted(p);if(plane.common.flags[1])scene->sources.push_back(mapped_plane(plane));
            }
        }else if(options.motion_check&&(layer.family==0x21200000||layer.family==0x20800000||layer.family==0x20000200)){
            auto helper=helper_source(selection_detail::motion_owner_helper(layer,data.base.width(),data.base.height(),data.base.dpi,line_costs));helper.density=vector_render_detail::output_terminal::density(layer.common.opacity,32);scene->sources.push_back(mapped(std::move(helper)));
        }else if(!options.plane_mode&&options.reference_source&&options.reference_qt_text_fonts&&!options.motion_check&&(layer.family==0x22100000u||layer.family==0x24400000u||layer.family==0x20000200u)){
            auto helper=line_reference_source(layer,scene->page,data.base.dpi,line_costs,scene->stop,options.reference_vector_aa2);
            helper.direct_copy=scene->reference_direct;scene->sources.push_back(mapped(std::move(helper)));
        }else if(!options.plane_mode&&options.reference_source&&options.reference_qt_text_fonts&&!options.motion_check&&(layer.family==0x21200000u||layer.family==0x20800000u)){
            auto helper=paint_reference_source(layer,scene->page,data.base.dpi,line_costs,scene->stop,options.reference_vector_aa2);
            helper.direct_copy=scene->reference_direct;scene->sources.push_back(mapped(std::move(helper)));
        }else if(!options.plane_mode&&(layer.family==0x22100000||layer.family==0x24400000)){
            if(std::ranges::all_of(layer.planes,[](const Plane& p){return p.family==0x21210001||p.family==0x21220001;}))scene->sources.push_back(mapped(normal_line_source(layer,data.base.dpi,line_costs,512ull*1024*1024,options.motion_check,scene->page,!options.reference_source&&!options.motion_check&&!prepared,options.minimum_line_width)));
            else{if(shared_sources)throw UnsupportedPreview("Image source ordinary Line/Pure pairing lacks aggregate helper admission");auto helper=helper_source(selection_detail::owner_helper(layer,data.base.dpi));helper.x=layer.common.x;helper.y=layer.common.y;scene->sources.push_back(mapped(std::move(helper)));}
        }else if(!options.plane_mode&&options.reference_source&&options.reference_qt_text_fonts&&!options.motion_check&&(layer.family==0x40004000u||layer.family==0x40002000u)){
            auto helper=layer.family==0x40004000u
                ?vector_reference_source(layer,data.base.dpi,line_costs,scene->stop,decode_vector_ruler_geometries,vector_render_detail::ruler_output_costs,render_vector_ruler_geometries)
                :vector_reference_source(layer,data.base.dpi,line_costs,scene->stop,decode_vector_shape_geometries,vector_render_detail::shape_output_costs,render_vector_shape_geometries);
            helper.direct_copy=scene->reference_direct;scene->sources.push_back(mapped(std::move(helper)));
        }else if(!options.plane_mode&&!options.reference_source&&!options.motion_check&&!prepared&&layer.common.resolution!=data.base.dpi&&(selection_detail::vector_move_owner(layer)||selection_detail::extension_move_owner(layer)||layer.family==0x20000200)){
            scene->sources.push_back(mapped(ordinary_vector_source(layer,data.base.dpi,line_costs,scene->stop)));
        }else if(!options.plane_mode&&(selection_detail::vector_move_owner(layer)||selection_detail::extension_move_owner(layer))){
            if(options.reference_source&&options.reference_qt_text_fonts&&!options.motion_check)throw UnsupportedPreview("Text reference vector helper lacks aggregate admission");
            auto helper=helper_source(selection_detail::owner_helper(layer,data.base.dpi,options.motion_check?&line_costs:nullptr));helper.x=layer.common.x;helper.y=layer.common.y;scene->sources.push_back(mapped(std::move(helper)));
        }else if(options.plane_mode&&(layer.family==0x40000800u||layer.family==0x48040800u)){
            // LAYP still dispatches Text's ordinary 62B450 -> 641CE0 path:
            // all visible children, glyph then leader in their saved colors.
            for(std::size_t p=layer.planes.size();p-->0;)if(layer.planes[p].common.flags[1])scene->sources.push_back(mapped_plane(layer.planes[p]));
        }else if(options.plane_mode){
            if(int(i)==data.current_layer&&layer.current_plane>=0&&layer.current_plane<int(layer.planes.size())
                &&layer.planes[layer.current_plane].common.opacity_mode==4)
                {insert_targeted(std::size_t(layer.current_plane));scene->sources.push_back(source(layer,layer.planes[layer.current_plane],data.base.dpi,options,true,&adapters));}
        }else for(std::size_t p=layer.planes.size();p-->0;){insert_targeted(p);if(layer.planes[p].common.flags[1])scene->sources.push_back(mapped_plane(layer.planes[p]));}
        if((layer.family&0x20000000u)&&!options.plane_mode&&!scene->sources.empty()){
            const auto& raster=*scene->sources.back().raster;Rect extent{};
            for(const auto& [tile,bytes]:raster.tiles){const int left=tile.x*256,top=tile.y*256,right=std::min(raster.width,left+256),bottom=std::min(raster.height,top+256);if(!bytes||right<=left||bottom<=top)continue;
                if(!extent.width||!extent.height)extent={left,top,right-left,bottom-top};else{const auto x=std::min(extent.x,left),y=std::min(extent.y,top);extent={x,y,std::max(extent.x+extent.width,right)-x,std::max(extent.y+extent.height,bottom)-y};}}
            scene->vector_extents.push_back({layer.common.runtime_identity,layer.specific,extent});
        }
        // The main renderer terminates its stack before the background-family
        // check. Helper flattening above deliberately has no such early exit.
        if(!options.reference_source&&(layer.common.opacity_mode==2||layer.common.opacity_mode==4))break;
        if(!scene->background&&layer.common.flags[4]&&(layer.family&0x04000000u))scene->background=packed_color(layer.common.color);
    }
    if(shared_sources||std::ranges::any_of(scene->sources,[](const Source& source){return bool(source.text_masks);})){
        if(adapters.work>128ull*1024*1024-line_costs.work||adapters.bytes>512ull*1024*1024-line_costs.storage)
            throw std::length_error("Image source adapters exceed aggregate resources");
        line_costs.work+=adapters.work;line_costs.storage+=adapters.bytes;
        scene->render_work=128ull*1024*1024-line_costs.work;
    }
    scene->render_storage=line_costs.storage;
    impl_=std::move(scene);
}
Rect PreviewScene::page_bounds() const noexcept{return impl_?impl_->page:Rect{};}
Rect PreviewScene::floating_bounds() const noexcept{return impl_?impl_->floating:Rect{};}
bool PreviewScene::reference_source_direct() const noexcept{return impl_&&impl_->reference_direct;}
void PreviewScene::publish_vector_cache_extents(DocumentData& data) const noexcept{
    if(!impl_)return;for(const auto& cached:impl_->vector_extents)for(auto& layer:data.layers)
        if(layer.common.runtime_identity==cached.identity&&layer.specific==cached.specific&&layer.runtime_vector_cache_present)layer.runtime_vector_cache_extent=cached.extent;
}
PreviewImage PreviewScene::render(Rect region) const{
    auto result=render_layers(region);finish_background(result);return result;
}
PreviewImage PreviewScene::render_layers(Rect region) const{
    std::array<std::uint64_t,2> admission;
    return render_layers(region,admission);
}
PreviewImage PreviewScene::render_layers(Rect region,std::array<std::uint64_t,2>& admission) const{
    if(!impl_)throw std::logic_error("预览场景已失效。");
    if(impl_->stop.stop_requested())throw std::runtime_error("Image source render cancelled");
    PreviewImage result(rectangle(intersect(area(region),area(impl_->page))));
    std::uint64_t work=impl_->render_work;for(const auto& source:impl_->sources)render_source(result,source,work,nullptr,nullptr,{},impl_->stop);
    admission={impl_->render_storage,128ull*1024*1024-work};
    return result;
}
void PreviewScene::finish_background(PreviewImage& result) const noexcept{
    if(impl_&&impl_->background){
        const auto background=*impl_->background;
        // A zero packed Color32 suppresses the native RenderBackground call.
        if(background.r||background.g||background.b||background.a){
            const auto bounds=rectangle(intersect(area(result.bounds()),area(impl_->page)));
            const auto color=Pixel::rgba8(background.r,background.g,background.b);
            for(int y=bounds.y;y<bounds.y+bounds.height;++y)for(int x=bounds.x;x<bounds.x+bounds.width;++x){
                const auto display=preview_display(result.pixel(x,y),color);
                result.set_pixel(x,y,{std::uint8_t(display.r>>8),std::uint8_t(display.g>>8),std::uint8_t(display.b>>8),255});
            }
        }
    }
}
}
