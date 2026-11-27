#include "retas/raster_stroke_pixels.hpp"
#include "retas/text_tool.hpp"
#include "retas/raster_stroke_geometry.hpp"
#include "raster_stroke_pixels_internal.hpp"
#include "retas/fill.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>

namespace retas {
bool raster_stroke_surface_supported(const Raster& r) noexcept{
    return r.can_edit()||(r.color_order==0&&(r.depth==1||r.depth==2||r.depth==8||r.depth==16))
        ||(r.depth==24&&r.color_order==4);
}
Pixel raster_stroke_surface_pixel(const Raster& r,int x,int y){
    if(!raster_stroke_surface_supported(r))throw std::invalid_argument("Unsupported stroke offscreen format");
    if(!r.contains(x,y))return {0,0,0,0};
    const auto found=r.tiles.find({x/256,y/256});if(found==r.tiles.end())return {0,0,0,0};
    if(!found->second||found->second->size()!=r.tile_bytes())throw std::invalid_argument("Malformed stroke offscreen tile");
    if(r.depth>=24)return r.pixel(x,y);
    const auto& b=*found->second;const auto p=std::size_t(y%256)*256+x%256;
    if(r.depth==16)return Pixel::rgba8(b[p*2+1],b[p*2+1],b[p*2+1],b[p*2]);
    if(r.depth==8)return {0,0,0,std::uint16_t(b[p]*257)};
    const unsigned code=(b[p*r.depth/8]>>(8-r.depth-(p*r.depth)%8))&((1u<<r.depth)-1);
    if(r.depth==2&&code==2)return Pixel::rgba8(255,255,255);
    return {0,0,0,std::uint16_t(code==unsigned((1<<r.depth)-1)?65535:0)};
}
bool RasterStrokeReferenceMask::allows(int x,int y) const{
    if(!std::isfinite(scale)||scale<=0)throw std::invalid_argument("Invalid reference mask DPI scale");
    const double mapped_x=std::trunc(x*scale);const auto mapped_y=legacy_rt_double_to_int(y*scale);
    if(!std::isfinite(mapped_x)||mapped_x<INT32_MIN||mapped_x>INT32_MAX||!mapped_y)
        throw std::invalid_argument("Reference mask coordinate exceeds int32");
    const auto p=raster_stroke_surface_pixel(surface,int(mapped_x),*mapped_y);
    if(alpha_only)return (p.a>>8)==(color.a>>8);
    if(!(p.a>>8)&&!(color.a>>8))return true;
    if(white_key&&(p.r>>8)==255&&(p.g>>8)==255&&(p.b>>8)==255
        &&(color.r>>8)==255&&(color.g>>8)==255&&(color.b>>8)==255)return true;
    return (p.r>>8)==(color.r>>8)&&(p.g>>8)==(color.g>>8)&&(p.b>>8)==(color.b>>8)&&(p.a>>8)==(color.a>>8);
}
namespace {
Pixel surface_current(const Raster& r,int x,int y){
    //Destination cursor allocation uses transparent WHITE background. This
    //logical comparison differs from guarded source reads at absent blocks.
    return r.depth>=24?r.pixel(x,y):raster_stroke_surface_pixel(r,x,y);
}
bool surface_unchanged(const Raster& r,int x,int y,Pixel value){
    if(surface_current(r,x,y)!=value)return false;
    //Code1 is a distinct stored transparent2-bit value. Set8/Set1 clearing
    //writes code0, so logical alpha equality must not suppress that change.
    if(r.depth==2&&!value.a){const auto tile=r.tiles.find({x/256,y/256});
        if(tile!=r.tiles.end()){const auto p=std::size_t(y%256)*256+x%256;
            return (((*tile->second)[p/4]>>(6-2*(p%4)))&3u)==0;}}
    return true;
}
void surface_store(Raster& r,int x,int y,Pixel value){
    if(r.can_edit()){r.set_pixel(x,y,value);return;}
    auto& tile=r.tiles[{x/256,y/256}];
    if(!tile)tile=std::make_shared<Bytes>(r.tile_bytes(),r.depth==24?255:0);
    else if(tile.use_count()!=1)tile=std::make_shared<Bytes>(*tile);
    const auto p=std::size_t(y%256)*256+x%256;auto& b=*tile;
    if(r.depth==16){b[p*2]=std::uint8_t(value.a>>8);b[p*2+1]=std::uint8_t(value.r>>8);}
    else if(r.depth==24){b[p*3]=std::uint8_t(value.b>>8);b[p*3+1]=std::uint8_t(value.g>>8);b[p*3+2]=std::uint8_t(value.r>>8);}
    else if(r.depth==8)b[p]=std::uint8_t(value.a>>8);
    else{
        const unsigned shift=8-r.depth-(p*r.depth)%8,mask=((1u<<r.depth)-1)<<shift;
        const bool set=r.depth==2?(value.a>>8)>=127:(value.a>>8)>127;
        b[p*r.depth/8]=std::uint8_t((b[p*r.depth/8]&~mask)|(set?mask:0));
    }
}
Pixel surface_mix(Pixel old,Pixel source,int depth,RasterStrokeTerminal terminal,unsigned weight){
    if(depth>=32)return raster_stroke_mix_pixel(old,source,depth,terminal,std::uint16_t(weight));
    // All Do24 terminals share Set24BitColor. Gray16 stores opaque weighted
    // RGB, not the generic Raster accessor's 16-bit coverage interpretation.
    if(depth==24)return Pixel::rgba8(source.r>>8,source.g>>8,source.b>>8);
    if(depth==16){const auto gray=((source.r>>8)+4*(source.g>>8)+2*(source.b>>8))/7;return Pixel::rgba8(gray,gray,gray);}
    unsigned value=weight;
    if(terminal==RasterStrokeTerminal::Blend||terminal==RasterStrokeTerminal::AlphaBlend)
        value=(old.a>>8)+(255-(old.a>>8))*weight/255;
    if(depth==1)value=value>127?255:0;
    if(depth==2)value=value>=127?255:0;
    return {0,0,0,std::uint16_t(value*257)};
}
using Channels=std::array<std::uint32_t,4>;
Channels channels(Pixel p,int depth){
    if(depth==32)return {unsigned(p.r>>8),unsigned(p.g>>8),unsigned(p.b>>8),unsigned(p.a>>8)};
    return {p.r,p.g,p.b,p.a};
}
Pixel pixel(const Channels& p,int depth){
    const unsigned scale=depth==32?257:1;
    return {std::uint16_t(p[0]*scale),std::uint16_t(p[1]*scale),std::uint16_t(p[2]*scale),std::uint16_t(p[3]*scale)};
}
std::uint32_t wrapped_move(std::uint32_t d,std::uint32_t s,unsigned w,int depth){
    const auto z=std::uint32_t((std::int64_t(s)-d)*w);
    const auto q=(std::uint64_t(depth==32?0x80808081u:0x80008001u)*z)>>(depth==32?39:47);
    return std::uint32_t(d+q)&(depth==32?255:65535);
}
unsigned gaussian_weight(int x,int y,int cx,int cy,int size,int hardness,unsigned weight){
    if(hardness>=100)return weight;
    static const auto table=[]{
        std::array<unsigned,193> values{};
        for(unsigned i=0;i<values.size();++i)values[i]=unsigned(4096*std::exp(-double(i*i)/8192.0));
        return values;
    }();
    const auto index=[size](int v,int c){return 192LL*std::max(0LL,2*std::abs(std::int64_t(v)-c)-1)/size;};
    const auto ix=index(x,cx),iy=index(y,cy);
    // Host geometry normally guarantees table bounds. Reject a mismatched
    // caller packet atomically rather than perform the native unchecked read.
    if(ix>192||iy>192)throw std::invalid_argument("Gaussian map exceeds native lookup domain");
    const auto p=(table[std::size_t(ix)]*table[std::size_t(iy)])>>12;
    const auto q=unsigned(p*127*std::min(size/16.0,1.0))>>12;
    const unsigned b=255-unsigned(hardness*255/100);
    return b?std::min(weight,q*weight/b):weight;
}
unsigned aa_coverage(int x,int y,Point c,double radius){
    // Native corner classifier uses 1/32 coordinates; boundary samples use
    // half-pixel integer coordinates, with truncation before the comparison.
    if(radius>3){
        const auto cx=std::int64_t(c.x*32),cy=std::int64_t(c.y*32),r=std::int64_t(radius*32);
        int corners=0;
        for(int j=0;j<2;++j)for(int i=0;i<2;++i){
            const auto dx=std::int64_t(x+i)*32-cx,dy=std::int64_t(y+j)*32-cy;
            corners+=dx*dx+dy*dy<=r*r;
        }
        if(corners==0)return 0;
        if(corners==4)return 255;
    }
    const auto cx=std::int64_t(c.x*2),cy=std::int64_t(c.y*2),rr=std::int64_t(4*radius*radius);
    unsigned count=0;
    for(int j=0;j<3;++j)for(int i=0;i<3;++i){
        const auto dx=2LL*x-cx+i,dy=2LL*y-cy+j;
        count+=dx*dx+dy*dy<=rr;
    }
    return count*255/9;
}
}

void bake_text_raster(Raster& target,const Raster& glyphs,bool alpha_only){
    if(!raster_stroke_surface_supported(target)||glyphs.depth!=32||target.width!=glyphs.width||target.height!=glyphs.height)throw std::invalid_argument("Unsupported text raster target");
    if(!glyphs.can_edit())throw std::invalid_argument("Unsupported text glyph storage");
    const auto validate=[](const Raster& raster){
        if(raster.width<=0||raster.height<=0)throw std::invalid_argument("Invalid text raster geometry");
        for(const auto& [tile,bytes]:raster.tiles)if(tile.x<0||tile.y<0||std::int64_t(tile.x)*256>=raster.width||std::int64_t(tile.y)*256>=raster.height||!bytes||bytes->size()!=raster.tile_bytes())throw std::invalid_argument("Malformed text raster tile");
    };
    validate(target);validate(glyphs);
    auto candidate=target;
    for(const auto& [tile,backing]:glyphs.tiles)for(int y=tile.y*256;y<std::min(glyphs.height,(tile.y+1)*256);++y)for(int x=tile.x*256;x<std::min(glyphs.width,(tile.x+1)*256);++x){
        auto source=glyphs.pixel(x,y);if(!source.a)continue;
        // Native TEXT mode0 writes monochrome glyph bits at full opacity.
        Pixel mixed=source;mixed.a=65535;
        if(candidate.depth<32)mixed=Pixel::rgba8(0,0,0);
        if(alpha_only&&candidate.depth>=32){const auto gray=(4897u*unsigned(source.r>>8)+9611u*unsigned(source.g>>8)+1876u*unsigned(source.b>>8))>>14;
            const auto old=candidate.pixel(x,y);mixed={old.r,old.g,old.b,std::uint16_t(gray*257)};}
        if(alpha_only&&candidate.depth<=8)mixed={};
        surface_store(candidate,x,y,mixed);
    }
    target=std::move(candidate);
}
Pixel raster_stroke_mix_pixel(Pixel destination,Pixel source,int depth,RasterStrokeTerminal terminal,std::uint16_t weight){
    if((depth!=32&&depth!=64)||(depth==32&&weight>255))throw std::invalid_argument("Unsupported raster stroke depth or weight");
    auto d=channels(destination,depth);const auto s=channels(source,depth);
    const unsigned n=depth==32?255:65535,sa=s[3],da=d[3];
    switch(terminal){
    case RasterStrokeTerminal::Base:return pixel(s,depth);
    case RasterStrokeTerminal::Blend:
        if(!sa)return pixel(d,depth);
        if(!da||sa==n)return pixel(s,depth);
        if(depth==32&&da!=n){
            // x87 complement-space branch uses the unrounded alpha divisor.
            const long double survival=static_cast<long double>(da)*(n-sa)/255.0L;
            const long double alpha=sa+survival;
            for(int i=0;i<3;++i)d[i]=n-unsigned(((n-d[i])*survival+(n-s[i])*sa)/alpha+0.5L);
            d[3]=unsigned(alpha+0.5L);
        }else{
            const auto survival=std::uint64_t(da)*(n-sa)/n;
            const auto alpha=n-std::uint64_t(n-da)*(n-sa)/n;
            for(int i=0;i<3;++i)d[i]=n-unsigned(((n-d[i])*survival+std::uint64_t(n-s[i])*sa)/alpha);
            d[3]=unsigned(alpha);
        }
        break;
    case RasterStrokeTerminal::AlphaCopy:d[3]=unsigned(std::uint64_t(s[0])*sa/n);break;
    case RasterStrokeTerminal::AlphaBlend:{
        if(!sa||!s[0])break;
        const auto effective=std::uint64_t(s[0])*sa/n;
        d[3]=n-unsigned(std::uint64_t(n-da)*(n-effective)/n);break;
    }
    case RasterStrokeTerminal::WeightCopy:
        if(weight==n)return pixel(s,depth);
        for(int i=0;i<4;++i){
            if(depth==32)d[i]=unsigned(int(d[i])+(int(s[i])-int(d[i]))*int(weight)/255);
            else d[i]=wrapped_move(d[i],s[i],weight,depth);
        }
        break;
    case RasterStrokeTerminal::AlphaWeightCopy:d[3]=wrapped_move(da,sa,weight,depth);break;
    default:throw std::invalid_argument("Unsupported raster stroke terminal");
    }
    return pixel(d,depth);
}

static RasterStrokePixelResult render_pixels(Raster& target,Point center,const RasterStrokeDab& dab,Pixel color,
    const std::optional<RasterSelection>& selection,std::stop_token stop,bool clear,const RasterStrokeReferenceMask* reference=nullptr){
    RasterStrokePixelResult result;
    try{
        if(!raster_stroke_surface_supported(target)||target.width<=0||target.height<=0)
            throw std::invalid_argument("Raster stroke requires a canonical offscreen target");
        for(const auto& [key,tile]:target.tiles)
            if(key.x<0||key.y<0||std::int64_t(key.x)*256>=target.width||std::int64_t(key.y)*256>=target.height||!tile||tile->size()!=target.tile_bytes())
                throw std::invalid_argument("Malformed stroke offscreen tile");
        // Bound all native integer conversions and squared geometry arithmetic.
        constexpr double limit=10000000;
        if(!std::isfinite(center.x)||!std::isfinite(center.y)||std::abs(center.x)>limit||std::abs(center.y)>limit||
           !std::isfinite(dab.radius_pixels)||dab.radius_pixels<0||dab.radius_pixels>limit||
           !std::isfinite(dab.strength)||dab.strength<0||dab.strength>1||dab.hardness<0||dab.hardness>100)
            throw std::invalid_argument("Invalid raster stroke dab geometry or strength");
        if(dab.pattern!=RasterStrokePattern::Circle&&dab.pattern!=RasterStrokePattern::Square)
            throw std::invalid_argument("Unsupported raster stroke pattern");
        // Host always reduces style packed64 to packed32 before scaling alpha.
        Pixel source=Pixel::rgba8(color.r>>8,color.g>>8,color.b>>8,unsigned((color.a>>8)*dab.strength));
        //537420 selects a binary token from the original alpha WORD before
        //strength/fade for1/2-bit. Normalized tokens then use Do8/Set8.
        if(target.depth<8)source={0,0,0,std::uint16_t(color.a?65535:0)};
        const auto initial_weight=target.depth==64?unsigned(source.a):unsigned(source.a>>8);
        (void)raster_stroke_mix_pixel({},source,target.depth==64?64:32,dab.terminal,std::uint16_t(initial_weight));
        if(stop.stop_requested()){result.cancelled=true;result.changed_pixels=0;return result;}
        const int cx=int(center.x),cy=int(center.y),diameter=std::max(1,int(2*dab.radius_pixels));
        const int ox=cx-diameter/2,oy=cy-diameter/2,map_size=int(2*dab.radius_pixels)+1;
        const bool aa=dab.pattern==RasterStrokePattern::Circle&&dab.antialias;
        // AA radius zero has an explicit native no-op, integer paths retain D=1.
        if(aa&&dab.radius_pixels==0)return result;
        const int x0=std::clamp(aa?int(center.x-dab.radius_pixels):ox,0,target.width);
        const int y0=std::clamp(aa?int(center.y-dab.radius_pixels):oy,0,target.height);
        const int x1=std::clamp(aa?int(center.x+dab.radius_pixels)+1:ox+diameter,0,target.width);
        const int y1=std::clamp(aa?int(center.y+dab.radius_pixels)+1:oy+diameter,0,target.height);
        Raster staged=target;
        int minx=target.width,miny=target.height,maxx=-1,maxy=-1;
        for(int y=y0;y<y1;++y){
            if(stop.stop_requested()){result.cancelled=true;result.changed_pixels=0;return result;}
            auto span=std::array<int,2>{x0,x1-1};
            if(!aa&&dab.pattern==RasterStrokePattern::Circle)span=raster_stroke_detail::circle_span(ox,diameter,y-oy);
            for(int x=std::max(x0,span[0]);x<std::min(x1,span[1]+1);++x){
                const unsigned coverage=aa?aa_coverage(x,y,center,dab.radius_pixels):255;
                if(!coverage||(selection&&!selection->contains(x,y))||(reference&&!reference->allows(x,y)))continue;
                auto normalized=source;
                // AA normalizes packed64 after scaling its 16-bit alpha. A
                // packed32 destination therefore takes the resulting high byte.
                normalized.a=std::uint16_t(unsigned(source.a)*coverage/255);
                // CircleFillAA copies each normalized run back into mixer+10
                // before dispatch (rcwCmn asm165495-165502). Thus the separate
                // weight starts from the AA-scaled alpha too; Gaussian still
                // modifies only that separate argument, never packed alpha.
                const auto coverage_weight=target.depth==64?unsigned(normalized.a):unsigned(normalized.a>>8);
                const auto weight=gaussian_weight(x,y,cx,cy,map_size,dab.hardness,coverage_weight);
                const auto old=surface_current(target,x,y);
                const auto cleared=dab.terminal==RasterStrokeTerminal::AlphaCopy
                    ?(target.depth>=32?Pixel{old.r,old.g,old.b,0}:Pixel::rgba8(0,0,0))
                    :target.depth<=16?Pixel{0,0,0,0}:target.depth==24?Pixel::rgba8(255,255,255):Pixel{};
                const auto next=clear?cleared
                    :surface_mix(old,normalized,target.depth,dab.terminal,weight);
                if(surface_unchanged(target,x,y,next))continue;
                surface_store(staged,x,y,next);++result.changed_pixels;
                minx=std::min(minx,x);miny=std::min(miny,y);maxx=std::max(maxx,x);maxy=std::max(maxy,y);
            }
        }
        if(stop.stop_requested()){result.cancelled=true;result.changed_pixels=0;return result;}
        if(result.changed_pixels){
            target=std::move(staged);result.changed=true;result.dirty={minx,miny,maxx-minx+1,maxy-miny+1};
        }
    }catch(const std::exception& e){result.handled=false;result.changed_pixels=0;result.error=e.what();}
    if(result.cancelled)result.changed_pixels=0;
    return result;
}
RasterStrokePixelResult raster_stroke_pixels(Raster& target,Point center,const RasterStrokeDab& dab,Pixel color,
    const std::optional<RasterSelection>& selection,std::stop_token stop,const RasterStrokeReferenceMask* reference){return render_pixels(target,center,dab,color,selection,stop,false,reference);}
RasterStrokePixelResult clear_raster_stroke_pixels(Raster& target,Point center,const RasterStrokeDab& dab,
    const std::optional<RasterSelection>& selection,std::stop_token stop){return render_pixels(target,center,dab,Pixel{},selection,stop,true);}
RasterStrokePixelResult transfer_raster_stroke_pixels(Raster& target,const Raster& coverage,Pixel color,RasterStrokeTerminal terminal,Rect rect,std::stop_token stop){
    RasterStrokePixelResult result;Raster candidate=target;
    const int x0=std::clamp(rect.x,0,target.width),y0=std::clamp(rect.y,0,target.height);
    const int x1=int(std::clamp<std::int64_t>(std::int64_t(rect.x)+rect.width,0,target.width));
    const int y1=int(std::clamp<std::int64_t>(std::int64_t(rect.y)+rect.height,0,target.height));
    int minx=x1,miny=y1,maxx=-1,maxy=-1;
    for(int y=y0;y<y1;++y){if(stop.stop_requested()){result.cancelled=true;return result;}
        for(int x=x0;x<x1;++x){const auto mask=coverage.pixel(x,y).a;if(!mask)continue;
            auto source=color;source.a=std::uint16_t(std::uint64_t(mask)*color.a/65535);
            const auto old=surface_current(target,x,y);
            const auto next=surface_mix(old,source,target.depth,terminal,target.depth==64?source.a:source.a>>8);
            if(surface_unchanged(target,x,y,next))continue;surface_store(candidate,x,y,next);++result.changed_pixels;
            minx=std::min(minx,x);miny=std::min(miny,y);maxx=std::max(maxx,x);maxy=std::max(maxy,y);
        }
    }
    if(result.changed_pixels){target=std::move(candidate);result.changed=true;result.dirty={minx,miny,maxx-minx+1,maxy-miny+1};}
    return result;
}
template<class Contains>
static RasterStrokePixelResult erase_transfer(Raster& target,int width,int height,double source_dpi,Contains contains,Rect rect,std::stop_token stop,std::optional<Pixel> override_color={}){
    RasterStrokePixelResult result;Raster candidate=target;int minx=target.width,miny=target.height,maxx=-1,maxy=-1;
    if(target.resolution<source_dpi)return result;
    const auto scale=std::trunc(target.resolution/source_dpi);
    if(!std::isfinite(scale)||scale<1||scale>100000)throw std::invalid_argument("Invalid eraser mask scale");
    const auto factor=std::int64_t(scale);
    const int left=int(std::clamp<std::int64_t>(std::int64_t(rect.x)*factor,0,target.width));
    const int top=int(std::clamp<std::int64_t>(std::int64_t(rect.y)*factor,0,target.height));
    const int right=int(std::clamp<std::int64_t>(std::min<std::int64_t>(width,std::int64_t(rect.x)+rect.width)*factor,left,target.width));
    const int bottom=int(std::clamp<std::int64_t>(std::min<std::int64_t>(height,std::int64_t(rect.y)+rect.height)*factor,top,target.height));
    if(std::uint64_t(right-left)*(bottom-top)>128ull*1024*1024)throw std::length_error("Eraser mask exceeds128Mi pixel visits");
    const auto clear=override_color.value_or(target.depth<=16?Pixel{0,0,0,0}:target.depth==24?Pixel::rgba8(255,255,255):Pixel{});
    for(int y=top;y<bottom;++y){if(stop.stop_requested()){result.cancelled=true;return result;}
        for(int x=left;x<right;++x)if(contains(int(x/factor),int(y/factor))){
            if(surface_unchanged(target,x,y,clear))continue;surface_store(candidate,x,y,clear);++result.changed_pixels;
            minx=std::min(minx,x);miny=std::min(miny,y);maxx=std::max(maxx,x);maxy=std::max(maxy,y);
        }
    }
    if(result.changed_pixels){target=std::move(candidate);result.changed=true;result.dirty={minx,miny,maxx-minx+1,maxy-miny+1};}return result;
}
RasterStrokePixelResult transfer_eraser_mask(Raster& target,const BinaryMask& mask,const std::optional<RasterSelection>& selection,std::stop_token stop,double source_dpi){
    return erase_transfer(target,mask.width(),mask.height(),source_dpi?source_dpi:target.resolution,
        [&](int x,int y){return mask.test(x,y)&&(!selection||selection->contains(x,y));},{0,0,mask.width(),mask.height()},stop);
}
RasterStrokePixelResult transfer_eraser_coverage(Raster& target,const Raster& coverage,Rect rect,std::stop_token stop){
    if(target.depth==16||target.depth==24)return {};
    return erase_transfer(target,coverage.width,coverage.height,coverage.resolution,[&](int x,int y){return coverage.pixel(x,y).a!=0;},rect,stop);
}
RasterStrokePixelResult transfer_pencil_line(Raster& target,const Raster& coverage,Pixel color,Rect rect,std::stop_token stop){
    if(target.depth==16||target.depth==24)return {};
    if(target.depth<=2)color={0,0,0,std::uint16_t(color.a?65535:0)};
    else if(target.depth==8)color={0,0,0,std::uint16_t((255-(color.r>>8))*257)};
    else if(target.depth==32)color=Pixel::rgba8(color.r>>8,color.g>>8,color.b>>8,color.a>>8);
    return erase_transfer(target,coverage.width,coverage.height,coverage.resolution,[&](int x,int y){return coverage.pixel(x,y).a>127*257;},rect,stop,color);
}
}
