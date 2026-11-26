#include "retas/model_thumbnail.hpp"
#include "retas/numeric.hpp"
#include "retas/retas_resources.hpp"
#include "retas/tga.hpp"
#include "retas/image_codecs.hpp"
#include "retas/pict.hpp"
#include <algorithm>
#include <cfenv>
#include <cmath>
#include <limits>

namespace retas {
namespace {
constexpr std::size_t memory_limit=512ull*1024*1024;
constexpr std::uint64_t work_limit=128ull*1024*1024;
constexpr std::uint16_t thumbnail_key=0xff07;
struct Ledger {
    CelFolderBudget local;CelFolderBudget& budget;std::stop_token stop;
    explicit Ledger(ModelThumbnailContext c):budget(c.budget?*c.budget:local),stop(c.stop){}
    void poll()const{if(stop.stop_requested())throw std::runtime_error("Model thumbnail cancelled");}
    void visit(std::uint64_t n){poll();if(budget.work>work_limit||n>work_limit-budget.work)throw std::length_error("Model thumbnail exceeds128Mi work");budget.work+=n;}
    void allocate(std::size_t n){poll();if(budget.allocation_bytes>memory_limit||n>memory_limit-budget.allocation_bytes)throw std::length_error("Model thumbnail exceeds512MiB memory");budget.allocation_bytes+=n;}
};
void dimensions(int w,int h){if(w<=0||h<=0||w>100000||h>100000||std::uint64_t(w)*h>268435456)throw std::invalid_argument("Invalid model thumbnail source dimensions");}
bool depth_valid(int d){return d==1||d==2||d==8||d==16||d==24||d==32||d==64;}
void source(const Raster& image,Ledger& ledger){
    dimensions(image.width,image.height);
    if(!depth_valid(image.depth)||!std::isfinite(image.resolution)||image.resolution<=0||
       (image.depth==8&&image.color_order!=0)||(image.depth==24&&image.color_order!=4)||
       ((image.depth==32||image.depth==64)&&image.color_order!=3))throw std::invalid_argument("Unsupported model thumbnail source layout");
    for(const auto& [p,tile]:image.tiles){ledger.visit(1);if(p.x<0||p.y<0||p.x>=(image.width+255)/256||p.y>=(image.height+255)/256||!tile||tile->size()!=image.tile_bytes())throw std::invalid_argument("Invalid model thumbnail source tile");}
}
std::size_t tile_storage(int w,int h,int d){return std::size_t((w+255)/256)*((h+255)/256)*(256ull*256*d/8+128);}
void dense(Raster& image){
    for(int y=0;y<image.height;y+=256)for(int x=0;x<image.width;x+=256)
        image.tiles.emplace(TileCoord{x/256,y/256},std::make_shared<Bytes>(image.tile_bytes(),0));
}
void put(Raster& image,int x,int y,std::span<const std::uint8_t> bytes){
    auto& tile=*image.tiles.at({x/256,y/256});const auto at=(std::size_t(y%256)*256+x%256)*(image.depth/8);
    std::copy(bytes.begin(),bytes.end(),tile.begin()+std::ptrdiff_t(at));
}
ModelThumbnail sample(const Raster& image,ModelThumbnailSize size,Ledger& ledger){
    ledger.poll();source(image,ledger);
    if(size.width<0||size.height<0||size.width>128||size.height>96)throw std::invalid_argument("Model thumbnail target exceeds128x96");
    ModelThumbnail result{size.width,size.height,32,{}};
    if(!size.width||!size.height)return result;
    const auto bucket=std::uint64_t((image.width+size.width-1)/size.width)*((image.height+size.height-1)/size.height);
    if(bucket>std::numeric_limits<std::uint32_t>::max()/255u)throw std::length_error("Model thumbnail channel sum would overflow");
    ledger.visit(std::uint64_t(std::max(image.width,size.width))*std::max(image.height,size.height));
    ledger.allocate(tile_storage(size.width,size.height,32));result.image.emplace(size.width,size.height,32,144);dense(*result.image);
    for(int y=0;y<size.height;++y){ledger.poll();const auto y0=y*image.height/size.height,y1=std::min(image.height,std::max(y0+1,(y+1)*image.height/size.height));
        for(int x=0;x<size.width;++x){const auto x0=x*image.width/size.width,x1=std::min(image.width,std::max(x0+1,(x+1)*image.width/size.width));
            std::uint32_t sums[4]{};const auto count=std::uint32_t((x1-x0)*(y1-y0));
            if(image.depth==8||image.depth==24||image.depth==32||image.depth==64)
                for(int sy=y0;sy<y1;++sy){ledger.poll();for(int sx=x0;sx<x1;++sx){const auto p=image.pixel(sx,sy);if(image.depth!=8){sums[0]+=p.b>>8;sums[1]+=p.g>>8;sums[2]+=p.r>>8;}sums[3]+=image.depth==24?255:p.a>>8;}}
            const std::uint8_t pixel[]{std::uint8_t(sums[0]/count),std::uint8_t(sums[1]/count),std::uint8_t(sums[2]/count),std::uint8_t(sums[3]/count)};put(*result.image,x,y,pixel);
        }
    }return result;
}
std::optional<ModelThumbnail> decode(std::span<const std::uint8_t> bytes,Ledger& ledger){
    ledger.poll();if(bytes.size()>memory_limit)throw std::length_error("FF07 exceeds512MiB");
    Reader r(bytes,Endian::Little);r.take(12);const auto w=r.u16(),h=r.u16(),combined=r.u16();
    // 608150 passes the WHOLE depth/descriptor word to606D90.
    if(combined!=8&&combined!=24&&combined!=32)return std::nullopt;
    if(!w||!h||w>32767||h>32767)throw FormatError(12,"Unsafe signed FF07 dimensions");
    dimensions(w,h);const auto stride=combined/8;
    ledger.visit(std::uint64_t(w)*h*2);ledger.allocate(tile_storage(w,h,combined));
    // Check every packet before allocating the destination or expanding pixels.
    auto scan=r;for(int y=0;y<h;++y)for(int x=0;x<w;){ledger.visit(1);const auto packet=scan.u8();const int count=(packet&127)+1;scan.take(std::size_t(packet&128?1:count)*stride);x+=std::min(count,w-x);}
    Raster image(w,h,combined,144);dense(image);
    for(int y=h-1;y>=0;--y){ledger.poll();for(int x=0;x<w;){const auto packet=r.u8();const int count=(packet&127)+1,used=std::min(count,w-x);auto data=r.take(std::size_t(packet&128?1:count)*stride);
        for(int n=0;n<used;++n){if(combined==8){const std::uint8_t a=packet&128?std::uint8_t(255-data[0]):255;put(image,x+n,y,{&a,1});}
            else put(image,x+n,y,data.subspan(std::size_t(packet&128?0:n)*stride,stride));}x+=used;
    }}return ModelThumbnail{w,h,int(combined),std::move(image)};
}
// Preflight the same offscreen that decode_offscreen will expand. Suffixes
// and sparse/count0 images retain the shared decoder's admission semantics.
void offscreen_storage(Reader parent,Ledger& ledger){
    auto outer=parent.section(),header=outer.section();const auto version=header.u16();const auto w=header.u32(),h=header.u32(),d=header.u32();const auto dpi=header.f64();const auto count=header.u32();(void)header.u8();
    if(version>0x1ff||!w||!h||w>100000||h>100000||std::uint64_t(w)*h>268435456||!depth_valid(int(d))||!std::isfinite(dpi)||dpi<=0||count>100000)throw FormatError(header.offset(),"Invalid model preview offscreen header");
    ledger.visit(1);if(!count)return;
    auto extent=outer.section();const auto left=extent.i32(),top=extent.i32(),right=extent.i32(),bottom=extent.i32();const bool compressed=extent.u8()!=0;
    if(left<0||top<0||right<=left||bottom<=top||left%256||top%256||right>int(w)+255||bottom>int(h)+255||std::uint64_t((right-left+255)/256)*((bottom-top+255)/256)!=count)throw FormatError(extent.offset(),"Invalid model preview tile extent");
    auto blocks=outer.section();const auto tile_size=256ull*256*d/8;
    for(std::uint32_t i=0;i<count;++i){ledger.visit(1);const auto present=blocks.u8();if(!present)continue;if(present!=1||blocks.u32()!=tile_size)throw FormatError(blocks.offset(),"Invalid model preview tile");
        ledger.allocate(std::size_t(tile_size)+128+(compressed?65536:0));ledger.visit(tile_size);const auto n=compressed?blocks.u32():std::size_t(tile_size);ledger.visit(n);blocks.take(n);
    }
}
std::optional<ModelThumbnail> read(std::span<const std::uint8_t> bytes,ModelThumbnailContainer container,Ledger& ledger){
    ledger.poll();if(bytes.size()>memory_limit)throw std::length_error("Model thumbnail file exceeds512MiB");
    if(container==ModelThumbnailContainer::Cel||container==ModelThumbnailContainer::Dga){
        Reader file(bytes);if(file.u32()!=(container==ModelThumbnailContainer::Cel?0x4343454cu:0x43444741u)||file.u16()>0x1ff)throw FormatError(0,"Invalid CEL/DGA preview header");
        auto previews=file.section();while(previews.remaining()){ledger.visit(1);const auto key=previews.u16();if(key!=1){(void)previews.section();continue;}
            offscreen_storage(previews,ledger);auto image=decode_offscreen(previews);ledger.poll();return ModelThumbnail{image.width,image.height,image.depth,std::move(image)};
        }return std::nullopt;
    }
    if(container!=ModelThumbnailContainer::Generic)throw std::invalid_argument("Unknown model thumbnail container");
    if(bytes.size()<=retas_resource_directory_size)return std::nullopt;
    const auto at=bytes.size()-retas_resource_directory_size;auto directory=bytes.subspan(at);
    constexpr std::uint8_t magic[]{'R','E','T','A','S'};if(!std::equal(std::begin(magic),std::end(magic),directory.begin()))return std::nullopt;
    Reader entries(directory.subspan(8),Endian::Little);
    for(std::size_t i=0;i<retas_resource_directory_capacity;++i){ledger.visit(1);const auto key=entries.u16();const auto length=entries.u32(),offset=entries.u32();if(key!=thumbnail_key)continue;
        if(offset>bytes.size()||bytes.size()-offset<2||length>bytes.size()-offset-2||bytes[offset]!=0xff||bytes[offset+1]!=7)return std::nullopt;
        return decode(bytes.subspan(std::size_t(offset)+2,length),ledger);
    }return std::nullopt;
}
Bytes encode(const ModelThumbnail& thumbnail,Ledger& ledger){
    ledger.poll();if(!thumbnail.image||thumbnail.width!=thumbnail.image->width||thumbnail.height!=thumbnail.image->height||thumbnail.depth!=thumbnail.image->depth)throw std::invalid_argument("A nonempty consistent model thumbnail is required");
    const auto& image=*thumbnail.image;source(image,ledger);
    if(image.width>32767||image.height>32767||(image.depth!=8&&image.depth!=24&&image.depth!=32&&image.depth!=64))throw std::invalid_argument("Unsupported model thumbnail encoding layout");
    ledger.visit(std::uint64_t(image.width)*image.height*4);ledger.allocate(std::size_t(image.width)*image.height*10+88);
    if(image.depth!=8){auto bytes=encode_tga(image,{true,32});ledger.poll();return bytes;}
    ledger.allocate(tile_storage(image.width,image.height,32));Raster expanded(image.width,image.height,32,144);dense(expanded);
    for(int y=0;y<image.height;++y){ledger.poll();for(int x=0;x<image.width;++x){const std::uint8_t pixel[]{0,0,0,std::uint8_t(image.pixel(x,y).a>>8)};put(expanded,x,y,pixel);}}
    auto bytes=encode_tga(expanded,{true,32});ledger.poll();return bytes;
}
Raster decode_source(std::span<const std::uint8_t> bytes,std::string_view ext,Ledger& ledger){
    ledger.poll();if(bytes.size()>memory_limit)throw std::length_error("Model source exceeds512MiB");ledger.visit(bytes.size());
    if(ext==".pict"||ext==".pct"){
        PictReadContext context{[&](std::uint64_t work,std::size_t allocation){ledger.visit(work);ledger.allocate(allocation);}};
        auto image=decode_pict(bytes,context);ledger.poll();return image;
    }
    int w{},h{};std::uint64_t extra_memory=4096,extra_work{};
    if(ext==".bmp"||ext==".dib"){
        Reader r(bytes,Endian::Little);if(r.u16()!=0x4d42)throw FormatError(0,"Invalid BMP signature");r.take(12);const auto header=r.u32();
        if(header==12){w=r.u16();h=r.u16();}else if(header==40){w=r.i32();h=r.i32();}else throw FormatError(14,"Unsupported BMP header");
    }else if(ext==".tga"){
        Reader r(bytes,Endian::Little);r.take(12);w=r.u16();h=r.u16();
    }else if(ext==".pic"){
        Reader r(bytes);r.take(92);w=r.u16();h=r.u16();
    }else if(ext==".tif"||ext==".tiff"){
        if(bytes.size()<8||!((bytes[0]=='I'&&bytes[1]=='I')||(bytes[0]=='M'&&bytes[1]=='M')))throw FormatError(0,"Invalid TIFF header");
        const auto endian=bytes[0]=='I'?Endian::Little:Endian::Big;Reader r(bytes,endian);r.u16();if(r.u16()!=42)throw FormatError(2,"Unsupported TIFF version");const auto offset=r.u32();
        if(offset<8||offset>bytes.size())throw FormatError(4,"Invalid TIFF directory");Reader entries(bytes.subspan(offset),endian);const auto count=entries.u16();entries.require_count(count,12,4096);
        for(unsigned i=0;i<count;++i){ledger.visit(1);const auto id=entries.u16(),type=entries.u16();const auto n=entries.u32();auto field=entries.take(4);
            if(!(id==256||id==257||id==258||id==259||id==262||id==273||id==277||id==278||id==279||id==282||id==284))continue;
            const unsigned unit=type==3||type==8?2:(type==4||type==9?4:(type==5?8:0));if(!unit||!n||n>100000)throw FormatError(entries.offset(),"Invalid TIFF tag");const auto length=std::size_t(unit)*n;
            if(length>4){Reader pointer(field,endian);const auto start=pointer.u32();if(start>bytes.size()||length>bytes.size()-start)throw FormatError(entries.offset(),"Invalid TIFF tag bounds");field=bytes.subspan(start,length);}
            ledger.allocate(length*4+256);ledger.visit(length*4);
            if(id==256||id==257){if((type!=3&&type!=4)||n!=1)throw FormatError(entries.offset(),"Invalid TIFF extent tag");Reader value(field,endian);const auto scalar=type==3?value.u16():value.u32();if(scalar>100000)throw FormatError(entries.offset(),"Invalid TIFF extent");(id==256?w:h)=int(scalar);}
        }
    }else if(ext==".lif"){
        Reader r(bytes,Endian::Little);r.u32();const auto version=r.u32();r.take(8);const auto compression=r.u32();r.take(32);w=r.i32();h=r.i32();const auto depth=r.i32();
        if(depth!=24&&depth!=32)throw FormatError(60,"Invalid LIF depth");dimensions(w,h);
        if(version==0x10000||compression==1){const auto scratch=((std::uint64_t(w)*(depth/8)+3)&~std::uint64_t(3))*h;extra_memory+=scratch+65536;extra_work+=scratch;}
    }else if(ext==".sgi"){
        Reader r(bytes);r.take(6);w=r.u16();h=r.u16();const auto channels=r.u16();if(channels!=3&&channels!=4)throw FormatError(10,"Invalid SGI channels");dimensions(w,h);
        extra_memory+=std::uint64_t(channels)*h*(std::uint64_t(w)+8);extra_work+=std::uint64_t(w)*h*channels*2;
    }else if(ext==".psd"){
        Reader r(bytes);r.take(12);const auto channels=r.u16();const auto height=r.u32(),width=r.u32();if(channels<3||channels>56||width>100000||height>100000)throw FormatError(12,"Invalid PSD header");w=int(width);h=int(height);dimensions(w,h);
        // Composite import parses <=300 layers with <=56 metadata channels,
        // and still visits every composite channel, including ignored extras.
        extra_memory+=2ull*1024*1024+std::uint64_t(channels)*h*(std::uint64_t(w)+2);extra_work+=std::uint64_t(w)*h*(2*channels+1);
    }else if(ext==".rla"){
        Reader r(bytes);const auto header=r.take(740);const auto word=[&](std::size_t at){Reader v(header.subspan(at,2));return std::bit_cast<std::int16_t>(v.u16());};w=int(word(10))-word(8)+1;h=int(word(14))-word(12)+1;dimensions(w,h);
        unsigned flags{};bool scan{};constexpr std::string_view letters="ZEOUNRCB";constexpr unsigned components[]{4,1,2,8,4,4,1,3};
        for(const auto c:header.subspan(400,128)){if(!c||c==')')break;if(c=='('){scan=true;continue;}if(scan){const auto i=letters.find(char(c));if(i!=std::string_view::npos)flags|=1u<<i;}}
        unsigned channels=word(22)>0?5:4;for(unsigned i=0;i<8;++i)if(flags&(1u<<i))channels+=components[i];
        extra_memory+=std::uint64_t(channels)*w*h+std::uint64_t(h)*4096;extra_work+=std::uint64_t(channels)*w*h*2;
        unsigned levels=1;for(auto rows=h;rows>1;rows=(rows+1)/2)++levels;extra_work+=16ull*h*levels;
    }else if(ext==".skf"){
        Reader file(bytes);if(file.u32()!=0x57534b46)throw FormatError(0,"Invalid SKF signature");const auto version=file.u16(),sections=file.u16();if(version>0x10ff||!sections)throw FormatError(4,"Invalid SKF header");file.require_count(sections,6,4096);
        for(unsigned i=0;i<sections;++i){ledger.visit(1);const auto id=file.u16();auto r=file.section();if(id!=1)continue;
            const auto sx=r.f64(),sy=r.f64();const auto unit=r.i32();const auto dpi=r.f64(),base_x=r.f64(),base_y=r.f64(),percent=r.f64();r.i32();double margins[4];for(auto& v:margins)v=r.f64();
            for(const auto v:{sx,sy,dpi,base_x,base_y,percent,margins[0],margins[1],margins[2],margins[3]})if(!std::isfinite(v))throw FormatError(r.offset(),"Invalid SKF geometry");if(dpi<=0||percent<=0)throw FormatError(r.offset(),"Invalid SKF scale");
            const double conversion=unit?0.3937007874015748:1.0,scale=100/percent;const auto width=(margins[0]*conversion+margins[2]*conversion+sx*(base_x*conversion)*scale)*dpi,height=(margins[1]*conversion+margins[3]*conversion+sy*(base_y*conversion)*scale)*dpi;
            if(!std::isfinite(width)||!std::isfinite(height)||width<1||height<1||width>65535||height>65535)throw FormatError(r.offset(),"Invalid SKF dimensions");w=int(width);h=int(height);break;
        }dimensions(w,h);extra_memory+=std::uint64_t(w)*h*96+65536;extra_work+=std::uint64_t(w)*h*128;
    }else throw std::invalid_argument("Model thumbnail generation requires a supported core image decoder");
    dimensions(w,h);const auto area=std::uint64_t(w)*h;
    ledger.visit(area*2+extra_work);if(extra_memory>memory_limit)throw std::length_error("Model source scratch exceeds512MiB");ledger.allocate(tile_storage(w,h,32));ledger.allocate(std::size_t(extra_memory));
    auto image=[&](){
        if(ext==".bmp"||ext==".dib")return decode_bmp(bytes);if(ext==".tga")return decode_tga(bytes);if(ext==".tif"||ext==".tiff")return decode_tiff(bytes);if(ext==".sgi")return decode_sgi(bytes);if(ext==".pic")return decode_pic(bytes);if(ext==".lif")return decode_lif(bytes);if(ext==".psd")return decode_psd(bytes);if(ext==".rla")return decode_rla(bytes).raster;return decode_skf(bytes);
    }();ledger.poll();return image;
}
}
ModelThumbnailSize fit_model_thumbnail_size(int w,int h){
    dimensions(w,h);if(std::fegetround()!=FE_TONEAREST)throw std::invalid_argument("Model thumbnail fit requires round-to-nearest");
    using numeric_detail::binary64;int dw=128,dh=96;
    if(w>=h&&binary64(128.0/96.0)<=binary64(double(w)/h))dh=int(binary64(binary64(128.0/w)*h));
    else dw=int(binary64(binary64(96.0/h)*w));
    return {128-2*((128-dw)/2),96-2*((96-dh)/2)};
}
ModelThumbnail make_model_thumbnail(const Raster& image,ModelThumbnailSize size,ModelThumbnailContext context){Ledger ledger(context);return sample(image,size,ledger);}
ModelThumbnail make_model_thumbnail(const Raster& image,ModelThumbnailContext context){Ledger ledger(context);ledger.poll();return sample(image,fit_model_thumbnail_size(image.width,image.height),ledger);}
Raster decode_model_thumbnail_source(std::span<const std::uint8_t> bytes,std::string_view extension,ModelThumbnailContext context){Ledger ledger(context);return decode_source(bytes,extension,ledger);}
std::optional<ModelThumbnail> decode_model_thumbnail(std::span<const std::uint8_t> bytes,ModelThumbnailContext context){Ledger ledger(context);return decode(bytes,ledger);}
std::optional<ModelThumbnail> read_model_thumbnail(std::span<const std::uint8_t> bytes,ModelThumbnailContainer container,ModelThumbnailContext context){Ledger ledger(context);return read(bytes,container,ledger);}
Bytes encode_model_thumbnail(const ModelThumbnail& image,ModelThumbnailContext context){Ledger ledger(context);return encode(image,ledger);}
Bytes store_model_thumbnail(std::span<const std::uint8_t> file,const ModelThumbnail& image,ModelThumbnailContext context){
    Ledger ledger(context);ledger.poll();if(file.size()>memory_limit)throw std::length_error("Model thumbnail file exceeds512MiB");
    auto payload=encode(image,ledger);
    // The canonical writer copies retained resources and grows its output.
    ledger.visit(file.size());ledger.allocate(file.size()*4+payload.size()*4+retas_resource_directory_size*2);
    const RetasResourceUpdate update{thumbnail_key,std::move(payload)};auto bytes=rewrite_retas_resource_archive(file,{&update,1});ledger.poll();return bytes;
}
}
