#include "retas/image_codecs.hpp"

namespace retas {
Raster decode_pic(std::span<const std::uint8_t> bytes){
    Reader r(bytes);if(r.u32()!=0x5380f634)throw FormatError(0,"Invalid SOFTIMAGE PIC magic");if(r.f32()!=3.0f)throw FormatError(4,"Unsupported PIC version");r.take(80);
    if(r.u32()!=0x50494354)throw FormatError(88,"Invalid PIC image identifier");
    const auto width=r.u16(),height=r.u16();r.f32();const auto channels=r.u16();r.u16();
    const auto rgb=r.bytes(4);const bool alpha=rgb[0]==1;
    if(rgb[0]>1||rgb[1]!=8||rgb[2]!=2||rgb[3]!=0xe0||channels!=3)throw FormatError(104,"Unsupported PIC RGB channel descriptor");
    if(alpha&&r.bytes(4)!=Bytes{0,8,2,0x10})throw FormatError(108,"Unsupported PIC alpha descriptor");
    if(!width||!height||std::uint64_t((width+255)/256)*((height+255)/256)*256*256*4>512ull*1024*1024)throw FormatError(92,"PIC image exceeds allocation limit");
    Raster image(width,height);
    for(unsigned y=0;y<height;++y)for(unsigned stream=0;stream<(alpha?2u:1u);++stream){
        unsigned x=0;
        while(x<width){
            const auto control=r.u8();const bool repeated=control>127;
            const unsigned count=control==128?r.u16():(repeated?control-127:control+1);
            if(!count||count>width-x)throw FormatError(r.offset(),"PIC packet crosses row bounds");
            const unsigned stride=stream?1:3;auto packet=r.take(stride*(repeated?1:count));
            for(unsigned i=0;i<count;++i,++x){const auto p=packet.subspan((repeated?0:i)*stride,stride);
                if(!stream)image.set_pixel(x,y,Pixel::rgba8(p[0],p[1],p[2]));
                else{auto value=image.pixel(x,y);value.a=std::uint16_t(p[0]*257);image.set_pixel(x,y,value);}
            }
        }
    }
    return image;
}
Bytes encode_pic(const Raster& image){
    if((!image.can_edit()&&!(image.depth==24&&image.color_order==4))||image.width<=0||image.height<=0||image.width>65535||image.height>65535)
        throw std::invalid_argument("Unsupported PIC output dimensions or layout");
    const bool alpha=image.depth!=24;
    if(std::uint64_t(image.width)*image.height*(alpha?6:4)+112>512ull*1024*1024)throw std::length_error("PIC output exceeds size limit");
    Writer w;w.u32(0x5380f634);w.f32(3);w.data.resize(88);w.u32(0x50494354);w.u16(std::uint16_t(image.width));w.u16(std::uint16_t(image.height));w.f32(1);w.u16(3);w.u16(0);
    w.raw(Bytes{std::uint8_t(alpha),8,2,0xe0});if(alpha)w.raw(Bytes{0,8,2,0x10});
    for(int y=0;y<image.height;++y)for(int stream=0;stream<(alpha?2:1);++stream){
        auto sample=[&](int x){const auto p=image.pixel(x,y);return stream?std::uint32_t(p.a>>8):std::uint32_t((p.r>>8)|((p.g>>8)<<8)|((p.b>>8)<<16));};
        auto write=[&](std::uint32_t p){w.u8(p&255);if(!stream){w.u8((p>>8)&255);w.u8((p>>16)&255);}};
        int x=0;
        while(x<image.width){
            int count=1;const auto first=sample(x);while(x+count<image.width&&count<65535&&sample(x+count)==first)++count;
            if(count>1){if(count<=128)w.u8(std::uint8_t(count+127));else{w.u8(128);w.u16(std::uint16_t(count));}write(first);x+=count;}
            else{
                const int start=x++;
                while(x<image.width&&x-start<128){if(x+1<image.width&&sample(x)==sample(x+1))break;++x;}
                w.u8(std::uint8_t(x-start-1));for(int i=start;i<x;++i)write(sample(i));
            }
        }
    }
    return std::move(w.data);
}
}
