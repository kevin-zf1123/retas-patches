#include "retas/image_codecs.hpp"
#include <cmath>

namespace retas {
Raster decode_bmp(std::span<const std::uint8_t> bytes){
    Reader r(bytes,Endian::Little);if(r.u16()!=0x4d42)throw FormatError(0,"Invalid BMP signature");r.u32();r.take(4);const auto offset=r.u32(),header=r.u32();
    int width{},height{};unsigned planes{},depth{},colors{};double dpi=144;
    if(header==12){width=r.u16();height=r.u16();planes=r.u16();depth=r.u16();}
    else if(header==40){
        width=r.i32();height=r.i32();planes=r.u16();depth=r.u16();if(r.u32()!=0)throw FormatError(30,"BMP requires uncompressed BI_RGB pixels");r.u32();
        const auto ppm=r.i32();r.i32();colors=r.u32();r.u32();if(ppm>0)dpi=std::round(double(ppm)*2.54/100*10)/10;
    }else throw FormatError(14,"Unsupported BMP information header");
    if(width<=0||height<=0||width>65535||height>65535||planes!=1||!(depth==1||depth==4||depth==8||depth==24||depth==32)||
       std::uint64_t((width+255)/256)*((height+255)/256)*256*256*4>512ull*1024*1024)throw FormatError(18,"Invalid BMP dimensions or depth");
    if(!colors&&depth<=8)colors=1u<<depth;if(colors>256||(depth<=8&&colors>(1u<<depth)))throw FormatError(46,"Invalid BMP palette size");
    std::vector<Pixel> palette;palette.reserve(colors);
    for(unsigned i=0;i<colors;++i){const auto b=r.u8(),g=r.u8(),red=r.u8();if(header!=12)r.u8();palette.push_back(Pixel::rgba8(red,g,b));}
    if(offset<r.offset()||offset>bytes.size())throw FormatError(10,"Invalid BMP raster offset");
    const auto stride=(std::size_t(width)*depth+31)/32*4;
    if(std::size_t(height)>(bytes.size()-offset)/stride)throw FormatError(offset,"Truncated BMP raster");
    Reader data(bytes.subspan(offset));Raster image(width,height,32,dpi);
    for(int y=height-1;y>=0;--y){const auto row=data.take(stride);for(int x=0;x<width;++x){Pixel p;
        if(depth<=8){const auto bit=unsigned(x)*depth;const auto index=(row[bit/8]>>(8-depth-bit%8))&((1u<<depth)-1);
            if(index>=palette.size())throw FormatError(offset,"BMP pixel references missing palette entry");p=palette[index];}
        else{const auto* sample=row.data()+std::size_t(x)*(depth/8);p=Pixel::rgba8(sample[2],sample[1],sample[0],depth==32?sample[3]:255);}
        image.set_pixel(x,y,p);
    }}return image;
}
Bytes encode_bmp(const Raster& image,bool force24){
    if((!image.can_edit()&&!(image.depth==24&&image.color_order==4))||image.width<=0||image.height<=0||image.width>65535||image.height>65535)
        throw std::invalid_argument("Unsupported BMP output dimensions or layout");
    const unsigned depth=!force24&&image.depth!=24?32:24;const auto stride=(std::size_t(image.width)*depth+31)/32*4;
    if(stride*image.height+54>512ull*1024*1024||!std::isfinite(image.resolution)||image.resolution<=0||image.resolution>INT32_MAX*2.54/100)
        throw std::length_error("BMP output exceeds supported limits");
    Writer w(Endian::Little);w.u16(0x4d42);w.u32(std::uint32_t(stride*image.height+54));w.u32(0);w.u32(54);w.u32(40);
    w.i32(image.width);w.i32(image.height);w.u16(1);w.u16(std::uint16_t(depth));w.u32(0);w.u32(0);
    const auto ppm=std::int32_t(image.resolution*100/2.54);w.i32(ppm);w.i32(ppm);w.u32(0);w.u32(0);
    for(int y=image.height-1;y>=0;--y){const auto start=w.data.size();for(int x=0;x<image.width;++x){const auto p=image.pixel(x,y);w.u8(p.b>>8);w.u8(p.g>>8);w.u8(p.r>>8);if(depth==32)w.u8(p.a>>8);}w.data.resize(start+stride,0);}
    return std::move(w.data);
}
}
