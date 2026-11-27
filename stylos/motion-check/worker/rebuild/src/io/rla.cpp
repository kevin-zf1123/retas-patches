#include "retas/image_codecs.hpp"
#include <algorithm>
#include <array>

namespace retas {
RlaImage decode_rla(std::span<const std::uint8_t> bytes,unsigned selection){
    Reader file(bytes);const auto header=file.take(740);
    auto word=[&](unsigned index){Reader r(header.subspan(index*2,2));return r.u16();};
    const int left=std::bit_cast<std::int16_t>(word(4)),right=std::bit_cast<std::int16_t>(word(5));
    const int bottom=std::bit_cast<std::int16_t>(word(6)),top=std::bit_cast<std::int16_t>(word(7));
    const int width=right-left+1,height=top-bottom+1;
    if(width<=0||height<=0||word(329)==16||word(331)==16||
       std::uint64_t((width+255)/256)*((height+255)/256)*256*256*4>512ull*1024*1024)throw FormatError(8,"Unsupported RLA image geometry or depth");
    const auto descriptor=header.subspan(400,128);unsigned flags=0;bool scan=false;
    constexpr std::string_view letters="ZEOUNRCB";constexpr std::array<unsigned,8> components{4,1,2,8,4,4,1,3};
    for(auto c:descriptor){if(!c)break;if(c=='('){scan=true;continue;}if(c==')')break;
        if(scan){const auto bit=letters.find(char(c));if(bit!=std::string_view::npos)flags|=1u<<bit;}}
    if(flags&&!(flags&4))throw FormatError(400,"Legacy RLA optional channels require Object ID");
    if(selection&&selection!=10000&&!(flags&4))throw FormatError(400,"RLA has no Object ID channel");
    if(selection==10000&&!(flags&1))throw FormatError(400,"RLA has no Z depth channel");
    file.require_count(height,4,65536);std::vector<std::uint32_t> offsets;offsets.reserve(height);
    for(int y=0;y<height;++y){const auto offset=file.u32();if(offset<740+std::uint64_t(height)*4||offset>=bytes.size())throw FormatError(file.offset()-4,"Invalid RLA scanline offset");offsets.push_back(offset);}
    auto sorted=offsets;std::sort(sorted.begin(),sorted.end());
    if(std::adjacent_find(sorted.begin(),sorted.end())!=sorted.end())throw FormatError(740,"Duplicate RLA row offset");
    RlaImage output{Raster(width,height,32,72),{},0,bool(flags&1)};if(selection==10000)output.z_depth.resize(std::size_t(width)*height);
    auto read_channel=[&](Reader& row){const auto size=row.u16();if(size>32767)throw FormatError(row.offset()-2,"RLA channel byte count exceeds signed 16-bit limit");Reader packed(row.take(size));Bytes result;result.reserve(width);
        while(packed.remaining()){
            const auto control=std::bit_cast<std::int8_t>(packed.u8());const unsigned count=control<0?-int(control):int(control)+1;
            if(count>std::size_t(width)-result.size())throw FormatError(row.offset()-size,"RLA channel packet exceeds width");
            if(control<0){const auto literal=packed.take(count);result.insert(result.end(),literal.begin(),literal.end());}
            else result.insert(result.end(),count,packed.u8());
        }
        if(result.size()!=std::size_t(width))throw FormatError(row.offset()-size,"Incomplete RLA channel");return result;
    };
    for(int y=0;y<height;++y){const auto start=offsets[height-1-y];const auto next=std::upper_bound(sorted.begin(),sorted.end(),start);
        const auto end=next==sorted.end()?bytes.size():*next;Reader row(bytes.subspan(start,end-start));
        const auto red=read_channel(row),green=read_channel(row),blue=read_channel(row);Bytes alpha(width,255);if(std::bit_cast<std::int16_t>(word(11))>0)alpha=read_channel(row);
        std::array<std::vector<Bytes>,8> auxiliary;
        for(unsigned channel=0;channel<8;++channel)if(flags&(1u<<channel))for(unsigned c=0;c<components[channel];++c)auxiliary[channel].push_back(read_channel(row));
        for(int x=0;x<width;++x){auto p=Pixel::rgba8(red[x],green[x],blue[x],alpha[x]);unsigned object=0;
            if(flags&4){object=(unsigned(auxiliary[2][0][x])<<8)|auxiliary[2][1][x];output.max_object_id=std::max(output.max_object_id,object);}
            if(selection==10000){std::uint32_t bits=0;for(unsigned c=0;c<4;++c)bits=(bits<<8)|auxiliary[0][c][x];output.z_depth[std::size_t(y)*width+x]=std::bit_cast<float>(bits);}
            else if(selection){if(object!=selection)p=Pixel{};else if(flags&64)p.a=std::uint16_t(auxiliary[6][0][x]*257);}
            output.raster.set_pixel(x,y,p);
        }
    }
    return output;
}
}
