#include "retas/image_codecs.hpp"
#include <algorithm>
#include <zlib.h>

namespace retas {
Raster decode_lif(std::span<const std::uint8_t> bytes){
    Reader h(bytes,Endian::Little);const auto magic=h.u32(),version=h.u32();
    if((magic!=0x4c494646&&magic!=0x4c494620)||(version>>16)>1)throw FormatError(0,"Unsupported LIF header");
    h.take(8);const auto compression=h.u32();h.take(8);
    Reader d(h.take(84),Endian::Little,28);d.take(24);
    const auto width=d.i32(),height=d.i32(),depth=d.i32(),size=d.i32();d.take(24);const auto skip=d.i32();
    const auto offset=std::int64_t(112)+skip;
    if(width<=0||height<=0||width>65535||height>65535||(depth!=24&&depth!=32)||
       std::uint64_t((width+255)/256)*((height+255)/256)*256*256*4>512ull*1024*1024)
        throw FormatError(52,"Invalid LIF dimensions or depth");
    if(skip<0||size<=0||std::size_t(offset)>bytes.size()||std::size_t(size)>bytes.size()-offset)
        throw FormatError(92,"Invalid LIF payload bounds");
    Reader payload(bytes.subspan(offset,size),Endian::Little,offset);Raster image(width,height);
    const auto bpp=unsigned(depth/8);const auto stride=(std::size_t(width)*bpp+3)&~std::size_t(3);
    if(version==0x10000||compression==1){
        const auto compressed_size=payload.u32();auto compressed=payload.take(compressed_size);
        Bytes decoded(stride*height);uLongf decoded_size=static_cast<uLongf>(decoded.size());uLong used=compressed_size;
        if(uncompress2(decoded.data(),&decoded_size,compressed.data(),&used)!=Z_OK||decoded_size!=decoded.size()||used!=compressed_size)
            throw FormatError(offset,"Invalid LIF zlib stream");
        for(int y=0;y<height;++y)for(int x=0;x<width;++x){const auto* p=decoded.data()+y*stride+x*bpp;image.set_pixel(x,y,Pixel::rgba8(p[0],p[1],p[2],bpp==4?p[3]:255));}
    }else{
        if(width>32767||height>32767)throw FormatError(52,"Legacy LIF dimensions exceed signed 16-bit range");
        // The original helper reverses channel bytes and flips bottom-up rows.
        // Place rows directly: never reproduce its uninitialised padding reads.
        for(int y=height-1;y>=0;--y){unsigned x=0;
            while(x<unsigned(width)){
                const auto control=payload.u8();const unsigned count=(control&127)+1,used=std::min(count,unsigned(width)-x);
                const bool repeat=control&128;auto packet=payload.take(bpp*(repeat?1:count));
                for(unsigned i=0;i<used;++i,++x){const auto p=packet.subspan((repeat?0:i)*bpp,bpp);
                    image.set_pixel(x,y,Pixel::rgba8(p[bpp-1],p[bpp-2],p[bpp-3],bpp==4?p[0]:255));}
            }
        }
    }
    return image;
}
}
