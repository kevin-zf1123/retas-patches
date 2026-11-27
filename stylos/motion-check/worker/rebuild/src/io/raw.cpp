#include "retas/image_codecs.hpp"

namespace retas {
Bytes encode_raw(const Raster& image,unsigned host_abi){
    if(!image.can_edit()&&!(image.depth==24&&image.color_order==4))throw std::invalid_argument("Unsupported RAW pixel layout");
    const unsigned depth=image.depth==64&&host_abi>=184?64:(image.depth==32?32:24);
    if(image.width<=0||image.height<=0)throw std::invalid_argument("Invalid RAW dimensions");
    const std::size_t stride=(std::size_t(image.width)*(depth/8)+3)&~std::size_t(3);
    if(stride>512ull*1024*1024/std::size_t(image.height))throw std::length_error("RAW output exceeds size limit");
    Writer output(Endian::Little);
    for(int y=0;y<image.height;++y){
        const auto start=output.data.size();
        for(int x=0;x<image.width;++x){const auto p=image.pixel(x,y);
            if(depth==64){output.u16(p.r);output.u16(p.g);output.u16(p.b);output.u16(p.a);}
            else{output.u8(p.r>>8);output.u8(p.g>>8);output.u8(p.b>>8);if(depth==32)output.u8(p.a>>8);}
        }
        output.data.resize(start+stride,0);
    }
    return std::move(output.data);
}
}
