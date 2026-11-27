#include "retas/image_codecs.hpp"
#include <algorithm>

namespace retas {
Raster decode_sgi(std::span<const std::uint8_t> bytes){
    Reader h(bytes);if(h.u16()!=0x1da)throw FormatError(0,"Invalid SGI signature");
    const auto storage=h.u8(),bpc=h.u8();const auto dimension=h.u16(),width=h.u16(),height=h.u16(),channels=h.u16();
    h.take(92);const auto colormap=h.u32();h.take(404);
    if(storage>1||bpc!=1||(dimension!=3&&dimension!=0x300)||(channels!=3&&channels!=4)||colormap)
        throw FormatError(2,"Unsupported SGI channel/storage layout");
    if(!width||!height||std::uint64_t((width+255)/256)*((height+255)/256)*256*256*4>512ull*1024*1024)
        throw FormatError(6,"SGI image exceeds allocation limit");
    const auto rows=std::size_t(height)*channels;const auto data_start=512+(storage?rows*8:0);
    std::vector<std::uint32_t> starts(rows),lengths(rows);
    if(storage){for(auto& v:starts)v=h.u32();for(auto& v:lengths)v=h.u32();}
    else if(std::size_t(width)*rows>h.remaining())throw FormatError(512,"Truncated SGI planes");
    Raster result(width,height);
    for(unsigned c=0;c<channels;++c)for(unsigned y=0;y<height;++y){
        const auto index=std::size_t(c)*height+y;Bytes row;row.reserve(width);
        if(storage){
            const auto start=starts[index],length=lengths[index];
            if(start<data_start||start>bytes.size()||!length||length>bytes.size()-start)throw FormatError(512+index*4,"Invalid SGI row bounds");
            Reader r(bytes.subspan(start,length),Endian::Big,start);
            while(row.size()<width){
                const auto packet=r.u8();const unsigned count=packet&127;
                if(!count||count>width-row.size())throw FormatError(r.offset()-1,"Invalid SGI RLE count");
                if(packet&128){const auto values=r.take(count);row.insert(row.end(),values.begin(),values.end());}
                else row.insert(row.end(),count,r.u8());
            }
        }else{const auto values=bytes.subspan(512+index*width,width);row.assign(values.begin(),values.end());}
        for(unsigned x=0;x<width;++x){
            auto p=result.pixel(x,height-1-y);const auto value=std::uint16_t(row[x]*257);
            if(c==0)p.r=value;else if(c==1)p.g=value;else if(c==2)p.b=value;else p.a=value;
            if(channels==3)p.a=65535;result.set_pixel(x,height-1-y,p);
        }
    }
    return result;
}
Bytes encode_sgi(const Raster& image,bool compressed,int channels){
    if(!channels)channels=image.depth==24?3:4;
    if((!image.can_edit()&&!(image.depth==24&&image.color_order==4))||(channels!=3&&channels!=4)||
       image.width<=0||image.height<=0||image.width>65535||image.height>65535)
        throw std::invalid_argument("Unsupported SGI output layout");
    const auto rows=std::size_t(image.height)*channels;
    if(rows*(image.width*2ull+8)+512>512ull*1024*1024)throw std::length_error("SGI output exceeds size limit");
    Writer h;h.u16(0x1da);h.u8(compressed);h.u8(1);h.u16(3);h.u16(std::uint16_t(image.width));h.u16(std::uint16_t(image.height));h.u16(std::uint16_t(channels));h.u32(0);h.u32(255);h.data.resize(512);
    Writer starts,lengths,payload;
    for(int c=0;c<channels;++c)for(int y=image.height-1;y>=0;--y){
        Bytes row;row.reserve(image.width);
        for(int x=0;x<image.width;++x){const auto p=image.pixel(x,y);row.push_back(std::uint8_t((c==0?p.r:c==1?p.g:c==2?p.b:p.a)>>8));}
        if(!compressed){payload.raw(row);continue;}
        starts.u32(std::uint32_t(512+rows*8+payload.data.size()));const auto begin=payload.data.size();
        std::size_t x=0;
        while(x<row.size()){
            std::size_t count=1;while(count<127&&x+count<row.size()&&row[x+count]==row[x])++count;
            if(count>1){payload.u8(std::uint8_t(count));payload.u8(row[x]);x+=count;}
            else{
                const auto first=x++;
                while(x<row.size()&&x-first<127){if(x+1<row.size()&&row[x]==row[x+1])break;++x;}
                payload.u8(std::uint8_t(128|(x-first)));payload.raw(std::span(row).subspan(first,x-first));
            }
        }
        lengths.u32(std::uint32_t(payload.data.size()-begin));
    }
    if(compressed){h.raw(starts.data);h.raw(lengths.data);}h.raw(payload.data);return std::move(h.data);
}
}
