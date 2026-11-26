#include "layer_plane_internal.hpp"
#include "retas/raster_stroke_pixels.hpp"
#include "native_raster_copy.hpp"
#include "resolution_raster_internal.hpp"
#include "selection_raster.hpp"
#include <algorithm>
#include <cmath>
#include <bit>

namespace retas::stack_detail {
namespace {
int integer(double value){if(!std::isfinite(value)||value<INT32_MIN||value>INT32_MAX)throw std::length_error("Stack coordinate exceeds signed32");return int(value);}
void validate(const Raster& r){
    if(r.width<=0||r.height<=0||r.width>100000||r.height>100000||std::int64_t(r.width)*r.height>268435456||!std::isfinite(r.resolution)||r.resolution<=0)throw std::invalid_argument("Invalid stack raster geometry");
    if(!raster_stroke_surface_supported(r))throw std::invalid_argument("Unsupported stack raster format");
    if(r.tiles.size()>512ull*1024*1024/r.tile_bytes())throw std::length_error("Stack raster exceeds 512 MiB");
    for(const auto& [q,b]:r.tiles)if(q.x<0||q.y<0||std::int64_t(q.x)*256>=r.width||std::int64_t(q.y)*256>=r.height||!b||b->size()!=r.tile_bytes())throw std::invalid_argument("Malformed stack raster tile");
}
unsigned sample(const Raster& r,int x,int y){
    if(!r.contains(x,y))return 0;
    auto it=r.tiles.find({x/256,y/256});if(it==r.tiles.end())return 0;
    const auto offset=std::size_t(y%256)*256+x%256;
    return r.depth==8?(*it->second)[offset]:((*it->second)[offset*r.depth/8]>>(8-r.depth-(offset*r.depth)%8))&((1u<<r.depth)-1);
}
void store_sample(Raster& r,int x,int y,unsigned value){
    auto& tile=r.tiles[{x/256,y/256}];if(!tile)tile=std::make_shared<Bytes>(r.tile_bytes(),0);else if(tile.use_count()!=1)tile=std::make_shared<Bytes>(*tile);
    const auto offset=std::size_t(y%256)*256+x%256;
    if(r.depth==8)(*tile)[offset]=std::uint8_t(value);
    else {const unsigned shift=8-r.depth-(offset*r.depth)%8,mask=((1u<<r.depth)-1)<<shift;auto& byte=(*tile)[offset*r.depth/8];byte=std::uint8_t((byte&~mask)|value<<shift);}
}
unsigned lower(Pixel p,int depth){
    const auto gray=((p.r>>8)+4u*(p.g>>8)+2u*(p.b>>8))/7;
    const auto ink=((255-gray)*((p.a>>8)+1))>>8;
    if(depth==1)return ink>=128;
    if(depth==2)return (p.a>>8)<=127?0:gray<127?3:2;
    return ink;
}
void store(Raster& r,int x,int y,Pixel p){
    if(r.can_edit()){r.set_pixel(x,y,p);return;}
    if(r.depth<=8){store_sample(r,x,y,lower(p,r.depth));return;}
    auto& b=r.tiles[{x/256,y/256}];if(!b)b=std::make_shared<Bytes>(r.tile_bytes(),r.depth==24?255:0);else if(b.use_count()!=1)b=std::make_shared<Bytes>(*b);
    const auto n=std::size_t(y%256)*256+x%256;
    if(r.depth==24){(*b)[3*n]=p.b>>8;(*b)[3*n+1]=p.g>>8;(*b)[3*n+2]=p.r>>8;}
    else{(*b)[2*n]=p.a>>8;(*b)[2*n+1]=std::uint8_t(((p.r>>8)+4u*(p.g>>8)+2u*(p.b>>8))/7);}
}
Rect block_extent(const Raster& r){
        int left=r.width,top=r.height,right=0,bottom=0;
        for(const auto& [q,b]:r.tiles){left=std::min(left,q.x*256);top=std::min(top,q.y*256);right=std::max(right,std::min(r.width,(q.x+1)*256));bottom=std::max(bottom,std::min(r.height,(q.y+1)*256));}
        return left<right&&top<bottom?Rect{left,top,right-left,bottom-top}:Rect{};
}
template<class F> void visit(const Raster& r,F f,bool include_holes=false){
    std::uint64_t work{};
    if(include_holes){
        const auto extent=block_extent(r);
        if(std::uint64_t(extent.width)*extent.height>128ull*1024*1024)throw std::length_error("Stack raster work exceeds 128M pixels");
        for(int y=extent.y;y<extent.y+extent.height;++y)for(int x=extent.x;x<extent.x+extent.width;++x)f(x,y);return;
    }
    for(const auto& [q,b]:r.tiles)for(int y=q.y*256;y<std::min(r.height,q.y*256+256);++y)for(int x=q.x*256;x<std::min(r.width,q.x*256+256);++x){if(++work>128ull*1024*1024)throw std::length_error("Stack raster work exceeds 128M pixels");f(x,y);}
}
}
Raster convert_raster(const Raster& source,int depth){
    validate(source);if(source.depth==depth)return source;
    Raster target(source.width,source.height,depth,source.resolution);validate(target);
    visit(source,[&](int x,int y){auto p=raster_stroke_surface_pixel(source,x,y);if(depth==32)p=Pixel::rgba8(p.r>>8,p.g>>8,p.b>>8,p.a>>8);store(target,x,y,p);});return target;
}
int plane_default_mode(std::uint32_t family) noexcept{return family==0x11110001||family==0x11110002||family==0x11410001||family==0x12100001||family==0x12100002||family==0x12400001?5:1;}
Rect merged_owner_bounds(const CommonElement& target,const Layer& layer){
    const auto& source=layer.common;
    if(!std::isfinite(target.resolution)||!std::isfinite(source.resolution)||target.resolution<=0||source.resolution<=0)throw std::invalid_argument("Invalid stack owner DPI");
    Rect extent{};
    if(layer.family&0x20000000u){if(layer.runtime_vector_cache_present)extent=layer.runtime_vector_cache_extent;}
    else{double last_dpi=source.resolution;
        for(const auto& p:layer.planes)if(p.raster){const auto& r=*p.raster;validate(r);last_dpi=r.resolution;
            for(const auto& [tile,bytes]:r.tiles){(void)bytes;const int l=tile.x*256,t=tile.y*256,rr=std::min(r.width,l+256),bb=std::min(r.height,t+256);
                if(!extent.width||!extent.height)extent={l,t,rr-l,bb-t};else{const auto x=std::min(extent.x,l),y=std::min(extent.y,t);extent={x,y,std::max(extent.x+extent.width,rr)-x,std::max(extent.y+extent.height,bb)-y};}}
        }
        const double scale=source.resolution/last_dpi;
        const auto right=integer((double(extent.x)+extent.width)*scale),bottom=integer((double(extent.y)+extent.height)*scale);extent.x=integer(extent.x*scale);extent.y=integer(extent.y*scale);extent.width=right-extent.x;extent.height=bottom-extent.y;
    }
    if(extent.width<=0||extent.height<=0)return {0,0,target.width,target.height};
    //620BD0 unions cached content-block extents in logical owner coordinates.
    const auto left=std::min(0,integer(double(source.x)+extent.x-target.x)),top=std::min(0,integer(double(source.y)+extent.y-target.y));
    const auto right=std::max(target.width,integer(double(source.x)+extent.x+extent.width-target.x)),bottom=std::max(target.height,integer(double(source.y)+extent.y+extent.height-target.y));
    return {left,top,integer(double(right)-left),integer(double(bottom)-top)};
}
namespace {
void materialize_line(Plane& target,const CommonElement& to,const Plane& source,const CommonElement& from){
    const auto& raw=*source.raster;const auto& dst=*target.raster;
    const int k=integer(raw.resolution/from.resolution);
    if(k<=0)throw std::invalid_argument("Native line converter requires a positive child/owner DPI multiplier");
    Raster input=raw;int dx=integer((double(from.x)-to.x)*k),dy=integer((double(from.y)-to.y)*k);
    if(from.resolution!=to.resolution){
        const double scale=raw.resolution/to.resolution;
        const int sx=integer(double(integer(to.x*scale))-integer(double(from.x)*k)),sy=integer(double(integer(to.y*scale))-integer(double(from.y)*k));
        resolution_detail::Budget budget;
        input=resolution_detail::resample(raw,{sx,sy,integer(dst.width*scale),integer(dst.height*scale)},integer(double(dst.width)*k),integer(double(dst.height)*k),to.resolution*k,budget,true,1,true);dx=dy=0;
    }
    Raster tmp(dst.width,dst.height,32,dst.resolution);std::uint64_t visits{};
    //5ECD60 positions source block cursors by the actual integer DPI ratio;
    //64BBA0 then scans whole bytes in groups of eight rows, regardless of k.
    for(int oy=0;oy<dst.height;oy+=256)for(int ox=0;ox<dst.width;ox+=256){
        const auto left=std::int64_t(ox)*k-dx,top=std::int64_t(oy)*k-dy;
        const auto right=std::int64_t(std::min(dst.width,ox+256))*k-dx,bottom=std::int64_t(std::min(dst.height,oy+256))*k-dy;
        const int cl=int(std::clamp<std::int64_t>(left,0,input.width)),ct=int(std::clamp<std::int64_t>(top,0,input.height));
        const int cr=int(std::clamp<std::int64_t>(right,0,input.width)),cb=int(std::clamp<std::int64_t>(bottom,0,input.height));
        for(int ty=ct/256;ty*256<cb;++ty)for(int tx=cl/256;tx*256<cr;++tx){
            const auto tile=input.tiles.find({tx,ty});if(tile==input.tiles.end())continue;
            const int sl=std::max(cl,tx*256),st=std::max(ct,ty*256),sr=std::min(cr,(tx+1)*256),sb=std::min(cb,(ty+1)*256);
            const int columns=(sr-sl)/8,rows=(sb-st)/8;if(!columns||!rows)continue;
            const auto px=(std::int64_t(sl)+dx)/k,py=(std::int64_t(st)+dy)/k;
            //The native writer advances a raw pointer in one tile, including
            //padding and row aliasing. Only accesses beyond that backing reject.
            if(px<0||py<0||px>=dst.width||py>=dst.height)throw std::invalid_argument("Native line scan has an invalid destination cursor");
            const auto dest_start=(std::size_t(py%256)*256+px%256)*4;
            if(dest_start+std::size_t(rows-1)*1024+std::size_t(columns)*4>tmp.tile_bytes())throw std::invalid_argument("Native line scan would exceed destination tile backing");
            tmp.set_pixel(int(px),int(py),tmp.pixel(int(px),int(py)));
            auto& output=*tmp.tiles.at({int(px/256),int(py/256)});
            const auto pitch=std::size_t(256*input.depth/8),start=std::size_t(st%256)*pitch+std::size_t(sl%256)*input.depth/8;
            for(int y=0;y<rows;++y)for(int x=0;x<columns;++x){
                if((visits+=64)>128ull*1024*1024)throw std::length_error("Line converter exceeds work limit");unsigned ink{};
                for(int row=0;row<8;++row)ink+=std::popcount(unsigned((*tile->second)[start+std::size_t(y*8+row)*pitch+x]));
                const unsigned coverage=(255*ink)>>6;if(!coverage)continue;
                const auto at=dest_start+std::size_t(y)*1024+std::size_t(x)*4;const auto old=output[at+3];
                const auto alpha=255-(255-old)*(255-coverage)/255;
                output[at]=output[at+1]=output[at+2]=0;output[at+3]=std::uint8_t(alpha);
            }
        }
    }
    for(int y=0;y<dst.height;++y)for(int x=0;x<dst.width;++x){
        if(++visits>128ull*1024*1024)throw std::length_error("Line converter exceeds work limit");
        if((tmp.pixel(x,y).a>>8)<127){tmp.set_pixel(x,y,{65535,65535,65535,0});continue;}
        const auto c=source.common.flags[4]?source.common.color:0xffff000000000000ull;
        tmp.set_pixel(x,y,dst.depth>=32?Pixel::rgba8((c>>8)&255,(c>>24)&255,(c>>40)&255,(c>>56)&255):Pixel::rgba8(0,0,0,1));
    }
    merge_raster(*target.raster,tmp,0,0,1,1,true);
}
}
void transfer_raster(Plane& target,const CommonElement& to,const Plane& source,const CommonElement& from,bool conversion){
    if(!target.raster||!source.raster)throw std::invalid_argument("Missing stack raster");
    auto& dst=*target.raster;const auto& src=*source.raster;validate(dst);validate(src);
    if(!std::isfinite(to.resolution)||!std::isfinite(from.resolution)||to.resolution<=0||from.resolution<=0)throw std::invalid_argument("Invalid stack owner DPI");
    const bool specialized=(target.family&0x00f00000u)==0x00200000u;
    if(conversion&&!specialized&&(source.family&0x00f00000u)==0x00200000u){materialize_line(target,to,source,from);return;}
    const double child=dst.resolution/to.resolution,ratio=from.resolution/to.resolution;
    const int dx=integer((double(from.x)-to.x)*child),dy=integer((double(from.y)-to.y)*child);
    if(!conversion&&dst.resolution==src.resolution){merge_raster(dst,src,dx,dy,target.common.opacity_mode,source.common.opacity_mode);return;}
    //63C210 materialization and the scaled63EF00 branch dispatch by the
    //destination subtype.5EADF0 uses an integer child multiplier and mode0;
    //5EC960 uses owner coordinates, mode1, source white-key and destination alpha.
    const int multiplier=specialized?integer(child):1;
    const bool stage=from.resolution!=to.resolution||(!specialized&&source.common.flags[4]);
    const int sx=integer(double(integer(to.x*ratio)-std::int64_t(from.x))*multiplier);
    const int sy=integer(double(integer(to.y*ratio)-std::int64_t(from.y))*multiplier);
    if(!stage){
        Raster same=src;same.resolution=dst.resolution;
        merge_raster(dst,same,integer(-double(sx)),integer(-double(sy)),specialized?1:source.common.opacity_mode,!specialized&&target.common.opacity_mode==4?4:1,true);return;
    }
    const int width=integer(dst.width*ratio),height=integer(dst.height*ratio);
    if(width<=0||height<=0)return;
    resolution_detail::Budget budget;
    auto staged=resolution_detail::resample(src,{sx,sy,width,height},dst.width,dst.height,dst.resolution,budget,true,specialized?1:dst.depth,specialized);
    if(!specialized&&source.common.flags[4]&&dst.depth>=32){
        const auto color=source.common.color;
        visit(staged,[&](int x,int y){auto p=raster_stroke_surface_pixel(staged,x,y);p.r=std::uint16_t(color);p.g=std::uint16_t(color>>16);p.b=std::uint16_t(color>>32);store(staged,x,y,p);});
    }
    merge_raster(dst,staged,0,0,specialized?1:source.common.opacity_mode,!specialized&&target.common.opacity_mode==4?4:1,true);
}
void merge_raster(Raster& target,const Raster& source,int dx,int dy,int mode,int source_mode,bool full_request,resolution_detail::Budget* budget,std::optional<Rect> source_request){
    validate(target);validate(source);
    if(source_request&&target.depth<32)throw std::invalid_argument("Explicit blend requests require32/64 destination; low depths use native copy staging");
    if(target.resolution!=source.resolution)throw std::invalid_argument("Stack raster merge requires matching plane resolution");
    if(budget){budget->poll();budget->allocate(sizeof(Raster)+target.tiles.size()*128ull);}
    Raster candidate=target;
    const auto extent=block_extent(source);
    const bool copy_all=source.depth==candidate.depth&&dx==0&&dy==0&&source.width==candidate.width&&source.height==candidate.height&&(full_request||extent==Rect{0,0,source.width,source.height});
    const auto materialize=[&](int x,int y){const auto tile=candidate.tiles.find({x/256,y/256});if(tile==candidate.tiles.end()||tile->second.use_count()!=1){if(budget){budget->allocate(candidate.tile_bytes()+128);budget->visit(65536);}selection_detail::store_raw_pixel(candidate,x,y,selection_detail::raw_pixel(candidate,x,y));if(candidate.tiles.size()>512ull*1024*1024/candidate.tile_bytes())throw std::length_error("Stack output exceeds512MiB");}};
    const auto copy_pixel=[&](int x,int y){
        if(source_request&&(x<source_request->x||y<source_request->y||std::int64_t(x)>=std::int64_t(source_request->x)+source_request->width||std::int64_t(y)>=std::int64_t(source_request->y)+source_request->height))return;
        if(budget)budget->visit(1);
        const auto xx=std::int64_t(x)+dx,yy=std::int64_t(y)+dy;if(xx<0||yy<0||xx>=candidate.width||yy>=candidate.height)return;
        const int tx=int(xx),ty=int(yy);
        if(candidate.depth>=32||source_mode==4){
            //Write-locked CommonCopy3 cursors exist before alpha/white gates.
            materialize(tx,ty);
            const auto end=std::min(source.width,(x/256+1)*256);
            //Only the low-source branch advances past its final pixel.
            if(source.depth<=8&&x+1==end&&dx%256&&(xx+1)%256==0&&xx+1<candidate.width)materialize(int(xx+1),ty);
        }
        if(source_mode==4){
            auto p=raster_stroke_surface_pixel(source,x,y);if(!(p.a>>8)||(source.depth==2&&sample(source,x,y)!=3))return;
            if(source.depth<=8)p={0,0,0,0};
            const unsigned alpha=p.a>>8;
            if(candidate.depth<=8)store_sample(candidate,tx,ty,candidate.depth==1?unsigned(alpha>127):candidate.depth==2?unsigned(alpha>=127?3:0):alpha);
            else if(candidate.depth<32){p.a=65535;store(candidate,tx,ty,p);}
            else store(candidate,tx,ty,raster_copy_detail::writeback_pixel(raster_stroke_surface_pixel(candidate,tx,ty),p,candidate.depth,true));
            return;
        }
        if(candidate.depth<=8){
            const auto old=sample(candidate,tx,ty);const auto s=source.depth==candidate.depth?sample(source,x,y):lower(raster_stroke_surface_pixel(source,x,y),candidate.depth);
            //5EAC70 stages source, then composes the old destination into it.
            const auto output=raster_copy_detail::low_copy_pixel(s,old,candidate.depth);
            store_sample(candidate,tx,ty,output);return;
        }
        auto p=raster_stroke_surface_pixel(source,x,y);
        const bool source_backed=source.contains(x,y)&&source.tiles.contains({x/256,y/256});
        if(candidate.depth==24&&source.depth==8){const auto gray=255-sample(source,x,y);p=Pixel::rgba8(gray,gray,gray);}
        if(!source_backed&&candidate.depth<32){
            p=candidate.depth==24||(!copy_all&&source.contains(x,y))?Pixel::rgba8(255,255,255,0):Pixel{0,0,0,0};
        }
        p=Pixel::rgba8(p.r>>8,p.g>>8,p.b>>8,p.a>>8); //CommonCopy3 always samples packed32.
        if(!p.a&&candidate.depth>=32)return;
        //Its low-source Get8 branch uses zero base tint, not black coverage.
        if(candidate.depth>=32&&source.depth<=8)p={0,0,0,0};
        if(candidate.depth>=32&&mode==5&&p.r==65535&&p.g==65535&&p.b==65535)return;
        const auto old=raster_stroke_surface_pixel(candidate,tx,ty);
        if(candidate.depth>=32)store(candidate,tx,ty,raster_copy_detail::writeback_pixel(old,p,candidate.depth,false));
        else if(candidate.depth==16){const auto output=raster_copy_detail::low_copy_pixel((p.a>>8)|(std::uint64_t(((p.r>>8)+4u*(p.g>>8)+2u*(p.b>>8))/7)<<8),(old.a>>8)|(std::uint64_t(old.r>>8)<<8),16);store(candidate,tx,ty,Pixel::rgba8((output>>8)&255,(output>>8)&255,(output>>8)&255,output&255));}
        else store(candidate,tx,ty,p);
    };
    if(full_request&&candidate.depth<32&&source_mode!=4){
        if(std::uint64_t(candidate.width)*candidate.height>128ull*1024*1024)throw std::length_error("Stack raster work exceeds 128M pixels");
        for(int y=0;y<candidate.height;++y)for(int x=0;x<candidate.width;++x)copy_pixel(integer(double(x)-dx),integer(double(y)-dy));
    }else visit(source,copy_pixel,candidate.depth<32&&source_mode!=4);
    if(budget)budget->poll();target=std::move(candidate);
}
}
