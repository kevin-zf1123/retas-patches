#include "retas/tga.hpp"
#include <algorithm>

namespace retas {
Raster decode_tga(std::span<const std::uint8_t> bytes,TgaPacketPolicy policy){
    const auto signature=ascii("TRUEVISION-XFILE.");
    if(bytes.size()>=26&&bytes.back()==0&&std::equal(signature.begin(),signature.end(),bytes.end()-18)){
        const auto footer=bytes.size()-26;Reader tail(bytes.subspan(footer,8),Endian::Little);std::size_t end=footer;
        for(int i=0;i<2;++i){const auto offset=tail.u32();if(offset){if(offset<18||offset>footer)throw FormatError(footer,"Invalid TGA footer offset");end=std::min(end,std::size_t(offset));}}
        bytes=bytes.first(end);
    }
    Reader r(bytes,Endian::Little);
    const auto id_length=r.u8(),color_map=r.u8(),type=r.u8();
    r.take(9);const auto width=r.u16(),height=r.u16();const auto depth=r.u8(),descriptor=r.u8();
    if(color_map||!(type==2||type==3||type==10||type==11)||!(depth==8||depth==24||depth==32)||(descriptor&0xc0))
        throw FormatError(0,"Unsupported TGA color map, pixel layout, or interleaving");
    if(!width||!height||std::uint64_t((width+255)/256)*((height+255)/256)*256*256*4>512ull*1024*1024)
        throw FormatError(12,"TGA dimensions exceed supported allocation limits");
    r.take(id_length);
    Raster image(width,height,32);
    auto pixel=[&]{
        const auto b=r.u8();if(depth==8)return Pixel::rgba8(b,b,b);
        const auto g=r.u8(),red=r.u8();const auto a=depth==32?r.u8():std::uint8_t(255);
        return Pixel::rgba8(red,g,b,a);
    };
    const std::size_t total=std::size_t(width)*height;
    auto put=[&](std::size_t index,Pixel p){
        const int sx=int(index%width),sy=int(index/width);
        image.set_pixel(descriptor&0x10?width-1-sx:sx,descriptor&0x20?sy:height-1-sy,p);
    };
    if(type==2||type==3){
        if(total>r.remaining()/(depth/8))throw FormatError(r.offset(),"Truncated TGA pixels");
        for(std::size_t i=0;i<total;++i)put(i,pixel());
    }else{
        std::size_t i=0;
        while(i<total){
            const auto packet=r.u8();const std::size_t count=(packet&127)+1;
            const auto used=policy==TgaPacketPolicy::LegacyRows?std::min(count,std::size_t(width)-i%width):count;
            if(used>total-i)throw FormatError(r.offset()-1,"TGA packet exceeds image dimensions");
            if(packet&128){const auto p=pixel();for(std::size_t n=0;n<used;++n)put(i+n,p);}
            else{
                // Legacy packets discard excess row pixels, but consume all bytes.
                if(count>r.remaining()/(depth/8))throw FormatError(r.offset(),"Truncated TGA raw packet");
                for(std::size_t n=0;n<count;++n){const auto p=pixel();if(n<used)put(i+n,p);}
            }
            i+=used;
        }
    }
    return image;
}
Bytes encode_tga(const Raster& image,TgaSettings settings){
    if((!image.can_edit()&&!(image.depth==24&&image.color_order==4))||image.width<=0||image.height<=0||image.width>65535||image.height>65535||
       !(settings.depth==24||settings.depth==32))throw std::invalid_argument("Unsupported TGA output dimensions or depth");
    if(std::uint64_t(image.width)*image.height*(settings.depth/8+1)>512ull*1024*1024)
        throw std::length_error("TGA output exceeds supported size limit");
    Writer w(Endian::Little);w.u8(0);w.u8(0);w.u8(settings.compressed?10:2);
    for(int i=0;i<9;++i)w.u8(0);
    w.u16(std::uint16_t(image.width));w.u16(std::uint16_t(image.height));w.u8(std::uint8_t(settings.depth));w.u8(0);
    auto packed=[&](int x,int y){auto p=image.pixel(x,y);return std::uint32_t((p.b>>8)|((p.g>>8)<<8)|((p.r>>8)<<16))|
        (settings.depth==32?std::uint32_t(p.a>>8)<<24:0);};
    const unsigned stride=settings.depth/8;
    for(int y=image.height-1;y>=0;--y){
        int x=0;
        while(x<image.width){
            const auto first=packed(x,y);
            if(!settings.compressed){w.integer(first,stride);++x;continue;}
            int count=1;
            while(count<128&&x+count<image.width&&packed(x+count,y)==first)++count;
            if(count>1){w.u8(std::uint8_t(128| (count-1)));w.integer(first,stride);x+=count;}
            else{
                const int start=x++;
                while(x<image.width&&x-start<128){
                    if(x+1<image.width&&packed(x,y)==packed(x+1,y))break;
                    ++x;
                }
                w.u8(std::uint8_t(x-start-1));for(int j=start;j<x;++j)w.integer(packed(j,y),stride);
            }
        }
    }
    w.u32(0);w.u32(0);w.raw(ascii("TRUEVISION-XFILE."));w.u8(0);
    return std::move(w.data);
}
}
