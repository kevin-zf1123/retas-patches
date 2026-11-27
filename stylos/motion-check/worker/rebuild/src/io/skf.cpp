#include "retas/image_codecs.hpp"
#include <algorithm>
#include <cmath>
#include <zlib.h>

namespace retas {
SkfImage decode_skf_data(std::span<const std::uint8_t> bytes){
    Reader file(bytes);if(file.u32()!=0x57534b46)throw FormatError(0,"Invalid SKF signature");
    const auto version=file.u16(),sections=file.u16();if((version>>8)>0x10||!sections)throw FormatError(4,"Invalid SKF version/section count");
    file.require_count(sections,6,4096);
    for(unsigned section=0;section<sections;++section){const auto id=file.u16();auto r=file.section();if(id!=1)continue;
        const auto sx=r.f64(),sy=r.f64();const auto unit=r.i32();const auto dpi=r.f64(),base_x=r.f64(),base_y=r.f64(),percent=r.f64();r.i32();
        std::array<double,4> margins;for(auto& margin:margins)margin=r.f64();
        for(auto value:{sx,sy,dpi,base_x,base_y,percent,margins[0],margins[1],margins[2],margins[3]})if(!std::isfinite(value))throw FormatError(r.offset(),"Non-finite SKF geometry");
        if(dpi<=0||percent<=0)throw FormatError(r.offset(),"Invalid SKF resolution or scale");
        const double conversion=unit?0.3937007874015748:1.0,scale=100/percent;
        const auto w=(margins[0]*conversion+margins[2]*conversion+sx*(base_x*conversion)*scale)*dpi;
        const auto h=(margins[1]*conversion+margins[3]*conversion+sy*(base_y*conversion)*scale)*dpi;
        if(!std::isfinite(w)||!std::isfinite(h)||w<1||h<1||w>65535||h>65535)throw FormatError(r.offset(),"Invalid SKF raster dimensions");
        SkfImage image;image.width=int(w);image.height=int(h);image.resolution=dpi;
        const auto pixels=std::uint64_t(image.width)*image.height;
        if(pixels>512ull*1024*1024/96)throw FormatError(r.offset(),"SKF oversampled passes exceed allocation limit");
        r.take(6*8);std::array<int,6> values;for(auto& value:values)value=r.i32();
        r.require_count(values[4],4,100000);for(int n=0;n<values[4];++n)r.section();
        const auto pairs=r.i32();r.require_count(pairs,16,100000);r.take(std::size_t(pairs)*16);r.i32();
        for(auto& pass:image.passes){
            const auto size=r.u32();const auto compressed=r.take(size);pass.resize(std::size_t(pixels)*48);
            uLongf output=static_cast<uLongf>(pass.size());uLong input=size;
            if(uncompress2(pass.data(),&output,compressed.data(),&input)!=Z_OK||output!=pass.size()||input!=size)throw FormatError(r.offset()-size,"Invalid SKF zlib pass");
        }
        r.take(8);if(version>=3)for(auto& opacity:image.opacity)opacity=r.i32();
        if(version>=4)for(auto& enabled:image.enabled)enabled=r.i32();
        for(auto opacity:image.opacity)if(opacity<0||opacity>100)throw FormatError(r.offset(),"Invalid SKF pass opacity");
        return image;
    }
    throw FormatError(file.offset(),"SKF has no image section");
}
void render_skf(const SkfImage& sketch,Raster& destination){
    if(sketch.width<=0||sketch.height<=0||std::uint64_t(sketch.width)*sketch.height>512ull*1024*1024/96||sketch.width!=destination.width||sketch.height!=destination.height||destination.depth!=32||!destination.can_edit())
        throw std::invalid_argument("SKF requires a matching initialized 32-bit destination");
    const auto bytes=std::uint64_t(sketch.width)*sketch.height*48;
    for(unsigned i=0;i<2;++i)if(sketch.passes[i].size()!=bytes||sketch.opacity[i]<0||sketch.opacity[i]>100)throw std::invalid_argument("Invalid SKF render pass");
    const auto stride=std::size_t(sketch.width)*12;
    for(int pass=1;pass>=0;--pass){if(!sketch.enabled[pass]||!sketch.opacity[pass])continue;
        const unsigned opacity=unsigned(sketch.opacity[pass])*255/100;
        for(int y=0;y<sketch.height;++y)for(int x=0;x<sketch.width;++x){
            std::array<unsigned,3> sum{};unsigned count=0;
            for(int row=0;row<4;++row){const auto* p=sketch.passes[pass].data()+(std::size_t(y)*4+row)*stride+std::size_t(x)*12;
                for(int col=0;col<4;++col,p+=3){if(p[0]==255&&p[1]==255&&p[2]==255)continue;for(unsigned c=0;c<3;++c)sum[c]+=p[c];++count;}
            }
            if(!count)continue;
            const unsigned coverage=((count*255/16)*opacity)>>8;auto pixel=destination.pixel(x,y);
            const unsigned inverse=255-(pixel.a>>8),weight=inverse*coverage/255;
            auto channel=[&](unsigned d,unsigned total){return unsigned(std::max(0,int(d>>8)-int(weight*(255-total/count)/255)));};
            destination.set_pixel(x,y,Pixel::rgba8(channel(pixel.r,sum[0]),channel(pixel.g,sum[1]),channel(pixel.b,sum[2]),255-inverse*(255-coverage)/255));
        }
    }
}
Raster decode_skf(std::span<const std::uint8_t> bytes){
    const auto sketch=decode_skf_data(bytes);
    // Ordinary PaintMan plugin import overrides the reported 24-bit depth with
    // a 32-bit scratch raster whose fresh tiles are transparent white.
    Raster output(sketch.width,sketch.height,32,sketch.resolution);
    render_skf(sketch,output);return output;
}
}
