#include "retas/batch_image_codec.hpp"
#include "retas/pict.hpp"
#include "batch_image_codec_qt.hpp"
#include <set>
#include <algorithm>
#include <cstdlib>
#ifdef RETAS_HAS_NATIVE_IMAGE_CODECS
#include "batch_image_native.h"
#endif
namespace retas {
namespace {
std::string extension(std::string_view name){if(name.starts_with('.'))name.remove_prefix(1);std::string value(name);for(auto& c:value)if(c>='A'&&c<='Z')c+=32;return value;}
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Batch image cancelled");}
#ifdef RETAS_HAS_NATIVE_IMAGE_CODECS
struct NativeBuffer{retas_image_buffer value{};~NativeBuffer(){std::free(value.data);
#ifdef RETAS_RENDER_ALLOCATOR
    retas_release_codec(value.budget_bytes);
#endif
}};
Raster native_read(std::string_view ext,std::span<const std::uint8_t> bytes,std::stop_token stop){NativeBuffer out;const bool png=ext=="png";if(!(png?retas_read_png(bytes.data(),bytes.size(),&out.value):retas_read_jpeg(bytes.data(),bytes.size(),&out.value))){if(out.value.budget_rejected)throw std::bad_alloc();throw std::runtime_error(out.value.error[0]?out.value.error:"Image decode failed");}Raster result(out.value.width,out.value.height,32,out.value.dpi);for(int y=0;y<result.height;++y){poll(stop);for(int x=0;x<result.width;++x){const auto* p=out.value.data+(std::size_t(y)*result.width+x)*(png?4:3);result.set_pixel(x,y,Pixel::rgba8(p[0],p[1],p[2],png?p[3]:255));}}return result;}
Bytes native_jpeg(const Raster& image,int quality,std::stop_token stop){if(quality<0||quality>100||std::uint64_t(image.width)*image.height>64ull*1024*1024)throw std::invalid_argument("Invalid JPEG quality/extent");Bytes rgb(std::size_t(image.width)*image.height*3);for(int y=0;y<image.height;++y){poll(stop);for(int x=0;x<image.width;++x){const auto p=image.pixel(x,y);const auto n=(std::size_t(y)*image.width+x)*3;rgb[n]=p.r>>8;rgb[n+1]=p.g>>8;rgb[n+2]=p.b>>8;}}NativeBuffer out;if(!retas_write_jpeg(rgb.data(),image.width,image.height,quality,&out.value))throw std::runtime_error(out.value.error[0]?out.value.error:"JPEG encode failed");poll(stop);return {out.value.data,out.value.data+out.value.size};}
#endif
Raster read_image(std::string_view ext,std::span<const std::uint8_t> bytes,std::stop_token stop){
    poll(stop);if(ext=="bmp"||ext=="dib")return decode_bmp(bytes);if(ext=="tga")return decode_tga(bytes);if(ext=="tif"||ext=="tiff")return decode_tiff(bytes);if(ext=="sgi")return decode_sgi(bytes);if(ext=="pic")return decode_pic(bytes);if(ext=="pict"||ext=="pct")return decode_pict(bytes);if(ext=="skf")return decode_skf(bytes);if(ext=="lif")return decode_lif(bytes);
#ifdef RETAS_HAS_NATIVE_IMAGE_CODECS
    if(ext=="png"||ext=="jpg"||ext=="jpeg")return native_read(ext,bytes,stop);
#elif defined(RETAS_HAS_QT_IMAGE_CODECS)
    if(ext=="png"||ext=="jpg"||ext=="jpeg")return batch_image_detail::qt_decode(ext=="png"?"png":"jpeg",bytes,stop);
#endif
    throw std::invalid_argument("No Batch image decoder for extension");
}
Bytes write_image(std::string_view ext,const Raster& image,const BatchImageWriteOptions& options,std::stop_token stop){
    poll(stop);if(ext=="png"){PngWriteContext context;context.cancelled=[stop]{return stop.stop_requested();};return encode_png(image,{options.png_filter,options.png_16bit},context);}if(ext=="bmp"||ext=="dib")return encode_bmp(image,options.bmp_force24);if(ext=="tga")return encode_tga(image,options.tga);if(ext=="tif"||ext=="tiff")return encode_tiff(image);if(ext=="sgi")return encode_sgi(image,options.sgi_compressed);if(ext=="pic")return encode_pic(image);if(ext=="pict"||ext=="pct")return encode_pict(image);if(ext=="raw")return encode_raw(image);if(ext=="yuv")return encode_yuv(image,options.yuv);
#ifdef RETAS_HAS_NATIVE_IMAGE_CODECS
    if(ext=="jpg"||ext=="jpeg")return native_jpeg(image,options.jpeg_quality,stop);
#endif
    throw std::invalid_argument("No Batch image encoder for extension");
}
BatchDecodedImage decode(std::string_view ext,std::span<const std::uint8_t> bytes,std::stop_token stop,const SvdImportSettings& svd){
    if(bytes.size()>512ull*1024*1024)throw std::length_error("Batch image input exceeds512MiB");poll(stop);BatchDecodedImage result;
    if(ext=="cel"||ext=="dga")result.data=decode_cel(bytes);
    else{auto image=ext=="svd"?decode_svd(bytes,svd):read_image(ext,bytes,stop);result.context.resources=decode_retas_resource_archive(bytes);if(result.context.resources){
            constexpr std::uint64_t limit=512ull*1024*1024;
            std::uint64_t backing{};
            const auto charge=[&](std::uint64_t bytes){if(bytes>limit-backing)throw std::length_error("Batch RETAS restored rasters exceed512MiB");backing+=bytes;};
            charge(std::uint64_t(image.tiles.size())*image.tile_bytes());
            const auto full=std::uint64_t((image.width+255)/256)*((image.height+255)/256)*65536*4;
            bool mask{},grad_a{},grad_b{};
            for(const auto& resource:result.context.resources->resources){
                if(resource.id==retas_resource_preview_small||resource.id==retas_resource_preview_large||resource.id==retas_resource_grad_auxiliary_a||resource.id==retas_resource_grad_auxiliary_b){
                    if(resource.payload.size()<18)throw std::invalid_argument("Truncated Batch RETAS TGA resource");
                    const auto& p=resource.payload;const auto w=unsigned(p[12])|unsigned(p[13])<<8,h=unsigned(p[14])|unsigned(p[15])<<8;
                    charge(std::uint64_t((w+255)/256)*((h+255)/256)*65536*4);
                    grad_a|=resource.id==retas_resource_grad_auxiliary_a;grad_b|=resource.id==retas_resource_grad_auxiliary_b;
                }else if(resource.id==retas_resource_mainline_mask){mask=true;charge(resource.payload.size());}
                else if(resource.id==retas_resource_document_data){charge(resource.payload.size());charge(full);}
            }
            // Restoration creates either one RGBA copy or the two ff01
            // split surfaces. Include their worst-case physical tile backing
            // before the resource decoder or split can allocate it.
            charge(full*(mask&&!(grad_a&&grad_b)?2:1));
            auto restored=restore_retas_raster_document(image,*result.context.resources);result.data=std::move(restored.document);result.data.unrestored_external_content=restored.resources.has_unrestored_semantics();}
        else{result.data=make_document(image.width,image.height,image.depth,image.resolution);result.data.active_plane()->raster=std::move(image);result.data.active_plane()->pixels_changed=true;}
        if(ext=="tga"&&bytes.size()>=18){result.context.write.tga.compressed=bytes[2]==10||bytes[2]==11;result.context.write.tga.depth=bytes[16]==32?32:24;}
        if((ext=="bmp"||ext=="dib")&&bytes.size()>=30)result.context.write.bmp_force24=bytes[28]!=32;
    }
    poll(stop);return result;
}
Bytes encode(std::string_view ext,const DocumentData& document,const BatchImageContext& context,std::stop_token stop){
    poll(stop);if(ext=="cel"||ext=="dga"){auto data=document;data.dga=ext=="dga";return encode_cel(data,SavePolicy::LegacyCanonical,false);}
    if(document.has_unrendered_content()||document.unrestored_external_content)throw std::invalid_argument("Batch image has unrestored content");auto image=render_retas_image_base(document,ext=="png"&&context.write.png_16bit?64:32);poll(stop);auto bytes=write_image(ext,image,context.write,stop);auto prepared=prepare_retas_image_resources(document,context.resources?&*context.resources:nullptr);if(!prepared.can_write_compatible())throw std::invalid_argument("Batch image has unsupported RETAS resources");poll(stop);return rewrite_retas_resource_archive(bytes,prepared.updates);
}
}
const BatchImageCodec* BatchImageCodecs::find(std::string_view ext)const{const auto name=extension(ext);for(const auto& codec:formats)if(std::ranges::find(codec.extensions,name)!=codec.extensions.end())return &codec;return nullptr;}
std::size_t BatchImageCodecs::storage()const{if(formats.size()>128)throw std::length_error("Batch codec count exceeds128");std::size_t size=formats.size()*sizeof(BatchImageCodec);std::set<std::string> names;for(const auto& f:formats){if(f.extensions.empty()||f.extensions.size()>32)throw std::invalid_argument("Invalid Batch codec extension count");for(const auto& name:f.extensions){if(name.empty()||name.size()>16||std::ranges::any_of(name,[](unsigned char c){return !((c>='a'&&c<='z')||(c>='0'&&c<='9'));})||!names.insert(name).second)throw std::invalid_argument("Invalid or duplicate Batch codec extension");size+=name.size()+sizeof(std::string);}}return size;}
BatchImageCodecs default_batch_image_codecs(){return default_batch_image_codecs(SvdImportSettings{});}
BatchImageCodecs default_batch_image_codecs(const SvdImportSettings& svd){
    BatchImageCodecs result;const auto add=[&](std::initializer_list<std::string> extensions,bool reader,bool writer){BatchImageCodec codec;codec.extensions=extensions;if(reader)codec.decode=[svd](auto ext,auto bytes,auto stop){return decode(ext,bytes,stop,svd);};if(writer)codec.encode=encode;result.formats.push_back(std::move(codec));};
    add({"cel","dga"},true,true);add({"bmp","dib"},true,true);add({"tif","tiff"},true,true);add({"tga"},true,true);add({"sgi"},true,true);add({"pic"},true,true);add({"pict","pct"},true,true);add({"lif"},true,false);add({"skf"},true,false);add({"svd"},true,false);add({"raw"},false,true);add({"yuv"},false,true);
#ifdef RETAS_HAS_NATIVE_IMAGE_CODECS
    add({"png"},true,true);add({"jpeg","jpg"},true,true);
#elif defined(RETAS_HAS_QT_IMAGE_CODECS)
    add({"png"},batch_image_detail::qt_readable("png"),true);add({"jpeg","jpg"},batch_image_detail::qt_readable("jpeg"),false);
#else
    add({"png"},false,true);add({"jpeg","jpg"},false,false);
#endif
    return result;
}
}
