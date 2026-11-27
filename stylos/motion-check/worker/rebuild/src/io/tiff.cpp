#include "retas/image_codecs.hpp"
#include <algorithm>
#include <cmath>
#include <map>

namespace retas {
Raster decode_tiff(std::span<const std::uint8_t> bytes){
    if(bytes.size()<8||!((bytes[0]=='I'&&bytes[1]=='I')||(bytes[0]=='M'&&bytes[1]=='M')))throw FormatError(0,"Invalid TIFF header");
    const auto endian=bytes[0]=='I'?Endian::Little:Endian::Big;Reader h(bytes,endian);h.u16();
    if(h.u16()!=42)throw FormatError(2,"Unsupported TIFF version");const auto offset=h.u32();
    if(offset<8||offset>bytes.size())throw FormatError(4,"Invalid TIFF directory offset");
    Reader entries(bytes.subspan(offset),endian,offset);const auto count=entries.u16();entries.require_count(count,12,4096);
    struct Tag {unsigned type;std::uint32_t count;Bytes data;};std::map<unsigned,Tag> tags;
    for(unsigned i=0;i<count;++i){
        const auto id=entries.u16(),type=entries.u16();const auto n=entries.u32();auto field=entries.bytes(4);
        if(!(id==256||id==257||id==258||id==259||id==262||id==273||id==277||id==278||id==279||id==282||id==284))continue;
        const unsigned size=type==3||type==8?2:(type==4||type==9?4:(type==5?8:0));
        if(!size||!n||n>100000)throw FormatError(entries.offset()-12,"Invalid TIFF tag type/count");
        const std::size_t length=std::size_t(size)*n;
        if(length>4){Reader pointer(field,endian);const auto start=pointer.u32();if(start>bytes.size()||length>bytes.size()-start)throw FormatError(entries.offset()-4,"Invalid TIFF tag payload");field.assign(bytes.begin()+start,bytes.begin()+start+length);}
        tags[id]={type,n,std::move(field)};
    }
    entries.u32();
    auto values=[&](unsigned id,std::vector<std::uint32_t> fallback){
        auto it=tags.find(id);if(it==tags.end())return fallback;
        const auto& t=it->second;if(t.type!=3&&t.type!=4)throw FormatError(offset,"Invalid TIFF integer tag");
        Reader r(t.data,endian);std::vector<std::uint32_t> result;result.reserve(t.count);
        for(std::uint32_t i=0;i<t.count;++i)result.push_back(t.type==3||t.type==8?r.u16():r.u32());return result;
    };
    auto scalar=[&](unsigned id,std::uint32_t fallback){const auto v=values(id,{fallback});if(v.size()!=1)throw FormatError(offset,"Invalid TIFF scalar count");return v[0];};
    const auto width=scalar(256,0),height=scalar(257,0),samples=scalar(277,0);
    if(!width||!height||width>65535||height>65535||std::uint64_t((width+255)/256)*((height+255)/256)*256*256*4>512ull*1024*1024)
        throw FormatError(offset,"Invalid TIFF image dimensions");
    if((samples!=3&&samples!=4)||scalar(259,1)!=1||scalar(262,0)!=2||scalar(284,1)!=1)
        throw FormatError(offset,"TIFF requires uncompressed chunky RGB/RGBA");
    const auto bits=values(258,{});
    if(bits.size()!=samples||std::any_of(bits.begin(),bits.end(),[](auto b){return b!=8;}))throw FormatError(offset,"TIFF requires 8-bit channels");
    const auto starts=values(273,{}),lengths=values(279,{});
    if(starts.empty()||starts.size()!=lengths.size()||starts.size()>height)throw FormatError(offset,"Invalid TIFF strip tables");
    if(!tags.contains(278)&&height%starts.size())throw FormatError(offset,"Ambiguous TIFF strip rows");
    const auto rows=scalar(278,std::uint32_t(height/starts.size()));
    if(!rows||(std::uint64_t(height)+rows-1)/rows!=starts.size())throw FormatError(offset,"Invalid TIFF rows per strip");
    double dpi=144;
    if(auto it=tags.find(282);it!=tags.end()){
        if(it->second.type!=5||it->second.count!=1)throw FormatError(offset,"Invalid TIFF resolution");Reader r(it->second.data,endian);const auto numerator=r.u32(),denominator=r.u32();if(numerator&&denominator)dpi=double(numerator)/denominator;
    }
    Raster image(width,height,32,dpi);unsigned y=0;
    for(std::size_t s=0;s<starts.size();++s){
        const auto n=std::min(rows,height-y);const auto length=std::size_t(n)*width*samples;
        if(starts[s]>bytes.size()||lengths[s]>bytes.size()-starts[s]||length>lengths[s])throw FormatError(offset,"Truncated TIFF strip");
        Reader strip(bytes.subspan(starts[s],length),endian,starts[s]);
        for(unsigned j=0;j<n;++j,++y)for(unsigned x=0;x<width;++x){const auto r=strip.u8(),g=strip.u8(),b=strip.u8(),a=samples==4?strip.u8():std::uint8_t(255);image.set_pixel(x,y,Pixel::rgba8(r,g,b,a));}
    }
    return image;
}
Bytes encode_tiff(const Raster& image){
    if((!image.can_edit()&&!(image.depth==24&&image.color_order==4))||image.width<=0||image.height<=0||image.width>65535||image.height>65535)
        throw std::invalid_argument("Unsupported TIFF output dimensions or layout");
    const unsigned samples=image.depth==24?3:4;const std::uint64_t payload=std::uint64_t(image.width)*image.height*samples;
    if(payload+174>512ull*1024*1024||!std::isfinite(image.resolution)||image.resolution<=0||image.resolution>UINT32_MAX/10.0)throw std::length_error("TIFF output exceeds supported limits");
    Writer w;w.u16(0x4d4d);w.u16(42);w.u32(8);w.u16(12);
    auto tag=[&](std::uint16_t id,std::uint16_t type,std::uint32_t count,std::uint32_t value){w.u16(id);w.u16(type);w.u32(count);if(type==3&&count==1){w.u16(std::uint16_t(value));w.u16(0);}else w.u32(value);};
    tag(255,3,1,1);tag(256,3,1,image.width);tag(257,3,1,image.height);tag(258,3,samples,166);tag(259,3,1,1);tag(262,3,1,2);
    tag(273,4,1,166+samples*2);tag(277,3,1,samples);tag(279,4,1,std::uint32_t(payload));tag(282,5,1,158);tag(283,5,1,158);tag(284,3,1,1);w.u32(0);
    w.u32(std::uint32_t(image.resolution*10));w.u32(10);for(unsigned c=0;c<samples;++c)w.u16(8);
    for(int y=0;y<image.height;++y)for(int x=0;x<image.width;++x){const auto p=image.pixel(x,y);w.u8(p.r>>8);w.u8(p.g>>8);w.u8(p.b>>8);if(samples==4)w.u8(p.a>>8);}
    return std::move(w.data);
}
}
