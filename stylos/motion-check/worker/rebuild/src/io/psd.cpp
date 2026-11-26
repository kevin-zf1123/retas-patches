#include "retas/image_codecs.hpp"
#include <algorithm>

namespace retas {
namespace {
struct Channel {int id;std::uint32_t size;std::span<const std::uint8_t> data;};
struct Layer {PsdLayerInfo info;std::vector<Channel> channels;unsigned opacity;bool mask{};};
struct Psd {int width,height;unsigned channels;double dpi{144};std::vector<Layer> layers;std::span<const std::uint8_t> composite;};
void dimensions(std::int64_t width,std::int64_t height){
    if(width<0||height<0||width>65535||height>65535||std::uint64_t((width+255)/256)*((height+255)/256)*256*256*4>512ull*1024*1024)
        throw FormatError(0,"PSD dimensions exceed supported limits");
}
Psd parse(std::span<const std::uint8_t> bytes){
    Reader r(bytes);if(r.u32()!=0x38425053||r.u16()!=1)throw FormatError(0,"Invalid PSD signature/version");r.take(6);
    Psd psd;psd.channels=r.u16();const auto h=r.u32(),w=r.u32();dimensions(w,h);
    if(!w||!h||psd.channels<3||psd.channels>56||r.u16()!=8||r.u16()!=3)throw FormatError(12,"PSD requires 8-bit RGB channels");
    psd.width=int(w);psd.height=int(h);r.section();auto resources=r.section();
    while(resources.remaining()){
        if(resources.u32()!=0x3842494d)throw FormatError(resources.offset()-4,"Invalid PSD resource signature");const auto id=resources.u16();
        const auto name=resources.u8();resources.take(name);if((name+1)%2)resources.take(1);
        const auto size=resources.u32();auto data=resources.take(size);if(size%2)resources.take(1);
        if(id==1005){Reader resolution(data);const auto fixed=resolution.u32();if(fixed)psd.dpi=double(fixed)/65536;}
    }
    auto mask=r.section();
    if(mask.remaining()){
        auto layers=mask.section();
        if(layers.remaining()){
            const int signed_count=std::bit_cast<std::int16_t>(layers.u16());const auto count=std::abs(signed_count);
            layers.require_count(count,34,300);
            for(int i=0;i<count;++i){
                const auto top=layers.i32(),left=layers.i32(),bottom=layers.i32(),right=layers.i32();
                const auto width=std::int64_t(right)-left,height=std::int64_t(bottom)-top;dimensions(width,height);
                Layer layer{{{},left,top,int(width),int(height)},{},255};const auto channels=layers.u16();layers.require_count(channels,6,56);
                for(unsigned c=0;c<channels;++c){const int id=std::bit_cast<std::int16_t>(layers.u16());const auto size=layers.u32();layer.channels.push_back({id,size,{}});}
                if(layers.u32()!=0x3842494d)throw FormatError(layers.offset()-4,"Invalid PSD blend signature");layers.u32();layer.opacity=layers.u8();layers.take(3);
                auto extra=layers.section();layer.mask=extra.section().remaining()!=0;extra.section();
                const auto name_size=extra.u8();layer.info.name=extra.bytes(name_size);extra.take((4-(name_size+1)%4)%4);
                // Masks/effects are separate channels/metadata; selected-layer import
                // follows the original raw channel path, not a PSD compositor.
                psd.layers.push_back(std::move(layer));
            }
            for(auto& layer:psd.layers)for(auto& channel:layer.channels)channel.data=layers.take(channel.size);
        }
    }
    psd.composite=r.take(r.remaining());return psd;
}
Bytes channel_row(Reader& source,unsigned width){
    Bytes row;row.reserve(width);
    while(row.size()<width){const auto control=std::bit_cast<std::int8_t>(source.u8());const unsigned count=control>=0?control+1:1-control;
        if(count>width-row.size())throw FormatError(source.offset()-1,"PSD PackBits run exceeds row");
        if(control>=0){const auto data=source.take(count);row.insert(row.end(),data.begin(),data.end());}
        else row.insert(row.end(),count,source.u8());
    }
    if(source.remaining())throw FormatError(source.offset(),"Unexpected PSD row tail");return row;
}
void put_channel(Raster& image,unsigned channel,int y,std::span<const std::uint8_t> row){
    for(int x=0;x<image.width;++x){auto p=image.pixel(x,y);const auto value=std::uint16_t(row[x]*257);
        switch(channel){case 0:p.r=value;break;case 1:p.g=value;break;case 2:p.b=value;break;case 3:p.a=value;break;default:continue;}
        image.set_pixel(x,y,p);
    }
}
void decode_channels(Reader& r,Raster& image,unsigned channels,int single_channel=-1){
    const auto compression=r.u16();if(compression>1)throw FormatError(r.offset()-2,"PSD ZIP compression is unsupported by the original reader");
    const auto count=std::size_t(channels)*image.height;std::vector<std::uint16_t> sizes;
    if(compression){r.require_count(static_cast<std::int32_t>(count),2,56*65535);sizes.reserve(count);for(std::size_t i=0;i<count;++i)sizes.push_back(r.u16());}
    for(unsigned c=0;c<channels;++c)for(int y=0;y<image.height;++y){const unsigned target=single_channel<0?c:unsigned(single_channel);
        if(compression){const auto size=sizes[std::size_t(c)*image.height+y];Reader row(r.take(size));const auto data=channel_row(row,image.width);put_channel(image,target,y,data);}
        else put_channel(image,target,y,r.take(image.width));
    }
}
}
std::vector<PsdLayerInfo> psd_layers(std::span<const std::uint8_t> bytes){auto parsed=parse(bytes);std::vector<PsdLayerInfo> result;for(auto& layer:parsed.layers)result.push_back(std::move(layer.info));return result;}
Raster decode_psd(std::span<const std::uint8_t> bytes,int selected_layer){
    const auto psd=parse(bytes);Raster output(psd.width,psd.height,32,psd.dpi);
    if(selected_layer<0||selected_layer>=int(psd.layers.size())){
        Reader data(psd.composite);decode_channels(data,output,psd.channels);
        if(psd.channels==3)for(int y=0;y<output.height;++y)for(int x=0;x<output.width;++x){auto p=output.pixel(x,y);if(p.r!=65535||p.g!=65535||p.b!=65535){p.a=65535;output.set_pixel(x,y,p);}}
    }else{
        const auto& layer=psd.layers[selected_layer];if(!layer.info.width||!layer.info.height)return output;
        Raster local(layer.info.width,layer.info.height);const bool alpha=layer.channels.size()-unsigned(layer.mask)==4;unsigned colors=0;
        for(const auto& channel:layer.channels){if(channel.id<-1||channel.id>2)continue;const auto target=channel.id==-1?3:channel.id;
            if(target!=3)colors|=1u<<target;Reader data(channel.data);decode_channels(data,local,1,target);
        }
        if(colors!=7)throw FormatError(0,"Selected PSD layer does not contain RGB channels");
        for(int y=0;y<local.height;++y)for(int x=0;x<local.width;++x){
            const auto dx=std::int64_t(layer.info.x)+x,dy=std::int64_t(layer.info.y)+y;if(dx<0||dy<0||dx>=output.width||dy>=output.height)continue;
            auto p=local.pixel(x,y);if(!alpha)p.a=65535;
            else if(layer.opacity>0&&layer.opacity<255)p.a=std::uint16_t(((unsigned(p.a>>8)*layer.opacity)/255)*257);
            output.set_pixel(int(dx),int(dy),p);
        }
    }
    return output;
}
}
