#include "retas/floating_transform.hpp"
#include "retas/document.hpp"
#include "retas/floating.hpp"
#include "retas/fill.hpp"
#include "retas/layer_move.hpp"
#include "retas/numeric.hpp"
#include "floating_preview_probe.hpp"
#include "vector_indication.hpp"
#include "selection_extension.hpp"
#include "raster_history.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <set>
#include <tuple>

namespace retas {
namespace {
using numeric_detail::binary64;
constexpr std::uint64_t storage_limit=512ull*1024*1024,work_limit=128ull*1024*1024;
struct Budget {
    std::uint64_t bytes{},work{};std::stop_token stop;
    void allocate(std::uint64_t n){if(n>storage_limit-bytes)throw std::length_error("Floating transform storage exceeds512MiB");bytes+=n;}
    void visit(std::uint64_t n){if(n>work_limit-work)throw std::length_error("Floating transform work exceeds128Mi");work+=n;poll();}
    void poll() const {if(stop.stop_requested())throw std::runtime_error("Floating transform cancelled");}
};
int coord(std::int64_t n){if(n<std::numeric_limits<int>::min()||n>std::numeric_limits<int>::max())throw std::invalid_argument("Floating transform coordinate overflow");return int(n);}
int integer(double n){if(!std::isfinite(n)||n<double(std::numeric_limits<int>::min())||n>double(std::numeric_limits<int>::max()))throw std::invalid_argument("Floating transform integer boundary overflow");return int(n);}
void point(FloatingTransformPoint p){if(!std::isfinite(p.x)||!std::isfinite(p.y)||std::abs(p.x)>double(std::numeric_limits<int>::max())||std::abs(p.y)>double(std::numeric_limits<int>::max()))throw std::invalid_argument("Invalid floating transform point");}
void packet(const FloatingTransformPacket& p){for(auto q:p.quad)point(q);point(p.pivot);if(p.reversals&~0x30u)throw std::invalid_argument("Invalid transform crossing bits");(void)floating_transform_bounds(p.quad);}
Rect translated(Rect r,int x,int y){r.x=coord(std::int64_t(r.x)+x);r.y=coord(std::int64_t(r.y)+y);return r;}
std::uint64_t raster_storage(const Raster& r){
    if(r.width<1||r.height<1||r.width>100000||r.height>100000||std::int64_t(r.width)*r.height>268435456||!std::isfinite(r.resolution)||r.resolution<=0||(r.depth!=1&&r.depth!=2&&r.depth!=8&&r.depth!=16&&r.depth!=24&&r.depth!=32&&r.depth!=64))throw std::invalid_argument("Invalid floating raster metadata");
    std::uint64_t n{};for(const auto& [p,b]:r.tiles){if(p.x<0||p.y<0||std::int64_t(p.x)*256>=r.width||std::int64_t(p.y)*256>=r.height||!b||b->size()!=r.tile_bytes())throw std::invalid_argument("Invalid floating raster block");n+=b->size()+128;if(n>storage_limit)throw std::length_error("Floating raster storage exceeds512MiB");}return n;
}
std::uint64_t document_storage(const DocumentData& d){
    if(d.layers.size()>100000)throw std::length_error("Too many transform source layers");std::uint64_t n=sizeof(d)+d.preview.size()+d.trailing.size()+d.layer_suffix.size()+d.base.suffix.size();
    for(const auto& l:d.layers){if(l.planes.size()>100000)throw std::length_error("Too many transform source planes");n+=sizeof(l)+l.specific.size()+l.suffix.size()+l.plane_suffix.size()+l.opaque_record.size()+l.common.name.size()+l.common.suffix.size();for(const auto& p:l.planes){n+=sizeof(p)+p.specific.size()+p.suffix.size()+p.opaque_record.size()+p.common.name.size()+p.common.suffix.size();if(p.raster)n+=raster_storage(*p.raster);if(n>storage_limit)throw std::length_error("Transform source exceeds512MiB");}}return n;
}
std::uint64_t document_metadata(const DocumentData& d){auto n=document_storage(d);for(const auto& l:d.layers)for(const auto& p:l.planes)if(p.raster)for(const auto& [tile,bytes]:p.raster->tiles)n-=bytes->size();return n;}
Raster clone(const Raster& r,Budget& b){b.allocate(raster_storage(r));Raster out=r;for(auto& [p,bytes]:out.tiles){b.poll();bytes=std::make_shared<Bytes>(*bytes);}b.poll();return out;}
DocumentData clone(const DocumentData& d,Budget& b){b.allocate(document_storage(d));auto out=d;if(d.original_content){if(d.original_content->original_content)throw std::invalid_argument("Nested transform persistence baseline unsupported");out.original_content=std::make_shared<const DocumentData>(clone(*d.original_content,b));}for(auto& l:out.layers)for(auto& p:l.planes)if(p.raster){for(auto& [q,bytes]:p.raster->tiles){b.poll();bytes=std::make_shared<Bytes>(*bytes);}}return out;}
std::uint64_t selection_storage(const RegionSelection& s){if(!s.mask)return 0;const auto w=std::uint64_t(s.mask->width()),h=std::uint64_t(s.mask->height());return ((w+31)/32)*h*4+((w+255)/256)*((h+255)/256);}
RegionSelection clone(const RegionSelection& s,Budget& b){auto out=s;if(s.mask){b.allocate(selection_storage(s));out.mask=std::make_shared<const BinaryMask>(*s.mask);}return out;}
std::array<std::uint64_t,2> floating_storage(const FloatingSelection& f){
    std::uint64_t metadata=sizeof(FloatingSelection),images{};if(f.layers){if(f.layers->size()>100000)throw std::length_error("Too many floating transform layers");for(const auto& l:*f.layers){if(l.planes.size()>100000)throw std::length_error("Too many floating transform planes");metadata+=sizeof(l)+l.common.name.size()+l.common.suffix.size()+l.specific.size()+l.suffix.size()+l.plane_suffix.size()+l.opaque_record.size();for(const auto& p:l.planes){metadata+=sizeof(p)+p.common.name.size()+p.common.suffix.size()+p.specific.size()+p.suffix.size()+p.opaque_record.size();if(p.raster)images+=raster_storage(*p.raster);if(metadata>storage_limit||images>storage_limit)throw std::length_error("Floating transform payload storage excessive");}}}if(f.sources){if(f.sources->size()>100000)throw std::length_error("Too many floating transform source mappings");metadata+=f.sources->size()*sizeof(FloatingSource);for(const auto& source:*f.sources){if(source.planes.size()>100000)throw std::length_error("Too many floating transform plane mappings");metadata+=source.planes.size()*sizeof(std::size_t);}}if(metadata>storage_limit||images>storage_limit-metadata)throw std::length_error("Floating transform snapshot storage excessive");return {metadata,images};
}
std::shared_ptr<const FloatingSelection> clone(const std::shared_ptr<const FloatingSelection>& f,Budget& b,bool omit_frame=false){
    if(!f)return {};const auto storage=floating_storage(*f);if(storage[0]>storage_limit-b.bytes||storage[1]>storage_limit-b.bytes-storage[0])throw std::length_error("Floating transform payload snapshot exceeds512MiB");b.allocate(storage[0]);auto out=std::make_shared<FloatingSelection>(*f);if(omit_frame)out->capture_frame={};if(f->layers){auto ls=std::make_shared<std::vector<Layer>>(*f->layers);for(auto& l:*ls)for(auto& p:l.planes)if(p.raster)p.raster=clone(*p.raster,b);out->layers=std::move(ls);}if(f->sources)out->sources=std::make_shared<const std::vector<FloatingSource>>(*f->sources);b.poll();return out;
}
bool real_equal(double a,double b){return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);}
bool common_equal(const CommonElement& a,const CommonElement& b){
    if(!real_equal(a.resolution,b.resolution))return false;for(std::size_t i=0;i<2;++i)if(a.position[i].unit!=b.position[i].unit||!real_equal(a.position[i].value,b.position[i].value)||!real_equal(a.position[i].reference,b.position[i].reference))return false;
    return std::tie(a.width,a.height,a.depth,a.name,a.flags,a.color,a.x,a.y,a.opacity,a.opacity_mode,a.field_14,a.suffix,a.runtime_operable)==std::tie(b.width,b.height,b.depth,b.name,b.flags,b.color,b.x,b.y,b.opacity,b.opacity_mode,b.field_14,b.suffix,b.runtime_operable);
}
bool raster_equal(const Raster& a,const Raster& b){if(a.width!=b.width||a.height!=b.height||a.depth!=b.depth||a.color_order!=b.color_order||!real_equal(a.resolution,b.resolution)||a.tiles.size()!=b.tiles.size())return false;auto i=a.tiles.begin(),j=b.tiles.begin();for(;i!=a.tiles.end();++i,++j)if(i->first!=j->first||!i->second||!j->second||*i->second!=*j->second)return false;return true;}
bool layers_equal(const std::vector<Layer>& a,const std::vector<Layer>& b){if(a.size()!=b.size())return false;for(std::size_t i=0;i<a.size();++i){const auto& l=a[i];const auto& r=b[i];if(l.opaque!=r.opaque||l.family!=r.family||!common_equal(l.common,r.common)||l.specific!=r.specific||l.suffix!=r.suffix||l.plane_suffix!=r.plane_suffix||l.opaque_record!=r.opaque_record||l.current_plane!=r.current_plane||l.planes.size()!=r.planes.size()||l.visibility_modified!=r.visibility_modified||l.runtime_vector_high_water!=r.runtime_vector_high_water||!vector_runtime_detail::face_colors_equal(l,r))return false;for(std::size_t j=0;j<l.planes.size();++j){const auto& p=l.planes[j];const auto& q=r.planes[j];if(p.opaque!=q.opaque||p.family!=q.family||!common_equal(p.common,q.common)||p.specific!=q.specific||p.suffix!=q.suffix||p.opaque_record!=q.opaque_record||p.pixels_changed!=q.pixels_changed||p.visibility_modified!=q.visibility_modified||bool(p.raster)!=bool(q.raster)||(p.raster&&!raster_equal(*p.raster,*q.raster)))return false;}}return true;}
bool document_equal(const DocumentData& a,const DocumentData& b,unsigned nesting=0){if(nesting>1)return false;if(a.version!=b.version||a.dga!=b.dga||a.current_layer!=b.current_layer||a.preview!=b.preview||a.layer_suffix!=b.layer_suffix||a.trailing!=b.trailing||bool(a.original)!=bool(b.original)||(a.original&&*a.original!=*b.original)||bool(a.original_content)!=bool(b.original_content)||(a.original_content&&!document_equal(*a.original_content,*b.original_content,nesting+1))||a.unrestored_external_content!=b.unrestored_external_content)return false;const auto& x=a.base;const auto& y=b.base;if(x.frame_mode!=y.frame_mode||x.flags!=y.flags||x.suffix!=y.suffix||!real_equal(x.scale,y.scale)||!real_equal(x.dpi,y.dpi)||!real_equal(x.dialog_dpi,y.dialog_dpi))return false;for(std::size_t i=0;i<x.lengths.size();++i)if(x.lengths[i].unit!=y.lengths[i].unit||!real_equal(x.lengths[i].value,y.lengths[i].value)||!real_equal(x.lengths[i].reference,y.lengths[i].reference))return false;return layers_equal(a.layers,b.layers);}
bool selection_equal(const RegionSelection& a,const RegionSelection& b){if(a.state!=b.state||a.bounds!=b.bounds||a.origin!=b.origin||a.carrier_size!=b.carrier_size||!real_equal(a.carrier_dpi,b.carrier_dpi)||bool(a.mask)!=bool(b.mask))return false;if(!a.mask)return true;if(a.mask->width()!=b.mask->width()||a.mask->height()!=b.mask->height())return false;for(int y=0;y<a.mask->height();++y)for(int x=0;x<a.mask->width();++x)if(a.mask->test(x,y)!=b.mask->test(x,y))return false;for(int y=0;y<(a.mask->height()+255)/256;++y)for(int x=0;x<(a.mask->width()+255)/256;++x)if(a.mask->block_present(x,y)!=b.mask->block_present(x,y))return false;return true;}
bool floating_equal(const std::shared_ptr<const FloatingSelection>& a,const std::shared_ptr<const FloatingSelection>& b){if(bool(a)!=bool(b))return false;if(!a)return true;if(a->kind!=b->kind||a->same_document_source!=b->same_document_source||a->delta!=b->delta||a->tight_bounds!=b->tight_bounds||a->capture_frame!=b->capture_frame||a->target!=b->target||bool(a->layers)!=bool(b->layers)||bool(a->sources)!=bool(b->sources))return false;if(a->layers&&!layers_equal(*a->layers,*b->layers))return false;if(a->sources){if(a->sources->size()!=b->sources->size())return false;for(std::size_t i=0;i<a->sources->size();++i)if((*a->sources)[i].layer!=(*b->sources)[i].layer||(*a->sources)[i].planes!=(*b->sources)[i].planes)return false;}return true;}
void profile(const DocumentData& d,const FloatingSelection& f,Budget& budget){
    if(f.delta[0]==std::numeric_limits<int>::min()||f.delta[1]==std::numeric_limits<int>::min())throw std::invalid_argument("Transform floating origin outside admitted coordinate domain");
    if(f.kind!=FloatingKind::General||f.target||!f.same_document_source||!f.sources)throw std::invalid_argument("Transform supports General raster floating only");validate_local_floating(d,f);
    std::set<std::size_t> owners;
    for(const auto& mapping:*f.sources){budget.allocate(128);budget.visit(1);if(!owners.insert(mapping.layer).second)throw std::invalid_argument("Transform requires unique source owners");
        std::set<std::size_t> planes;for(auto plane:mapping.planes){budget.allocate(128);budget.visit(1);if(!planes.insert(plane).second)throw std::invalid_argument("Transform requires unique source planes");}}
    for(std::size_t i=0;i<f.layers->size();++i){const auto& l=(*f.layers)[i];if(l.common.x==std::numeric_limits<int>::min()||l.common.y==std::numeric_limits<int>::min())throw std::invalid_argument("Transform owner outside admitted coordinate domain");if(l.common.opacity!=100||l.common.opacity_mode!=1)throw std::invalid_argument("Unsupported transform owner material mode");for(std::size_t p=0;p<l.planes.size();++p){const auto& src=l.planes[p];if(!src.raster||!src.raster->can_edit())throw std::invalid_argument("Transform requires raster payloads");if(src.raster->width!=src.common.width||src.raster->height!=src.common.height||src.raster->depth!=src.common.depth)throw std::invalid_argument("Transform source metadata mismatch");const auto& dst=d.layers[(*f.sources)[i].layer].planes[(*f.sources)[i].planes[p]];if(!src.raster->can_edit()||!dst.raster||!dst.raster->can_edit()||src.common.resolution!=d.base.dpi||dst.common.resolution!=d.base.dpi||src.raster->resolution!=d.base.dpi||dst.raster->resolution!=d.base.dpi)throw std::invalid_argument("Transform requires same-DPI32/64 raster planes");if(src.common.opacity!=100||src.common.opacity_mode!=1||dst.common.opacity!=100||dst.common.opacity_mode!=1)throw std::invalid_argument("Unsupported transform plane material mode");}}
}
void put(Raster& r,int x,int y,Pixel c){if(r.depth!=1){r.set_pixel(x,y,c);return;}auto& b=r.tiles[{x/256,y/256}];if(!b)b=std::make_shared<Bytes>(8192,0);const auto at=std::size_t(y%256)*32+std::size_t(x%256)/8;const auto mask=std::uint8_t(0x80u>>(x%8));if(c.a)(*b)[at]|=mask;else (*b)[at]&=std::uint8_t(~mask);}
std::uint64_t surface(int w,int h,int depth){if(w<1||h<1||w>100000||h>100000||std::int64_t(w)*h>268435456)throw std::invalid_argument("Transform output dimensions unsupported");return std::uint64_t((w+255)/256)*std::uint64_t((h+255)/256)*256*256*std::uint64_t(depth)/8;}
Raster crop(const Raster& src,Rect r,Budget& b){b.allocate(surface(r.width,r.height,src.depth));b.visit(std::uint64_t(r.width)*std::uint64_t(r.height));Raster out(r.width,r.height,src.depth,src.resolution);for(int y=0;y<r.height;++y){b.poll();for(int x=0;x<r.width;++x){const auto sx=coord(std::int64_t(r.x)+x),sy=coord(std::int64_t(r.y)+y);if(src.contains(sx,sy)&&src.tiles.contains({sx/256,sy/256}))put(out,x,y,src.pixel(sx,sy));}}return out;}
struct Cell {int begin{},end{};};
Pixel average(const Raster& src,Cell xs,Cell ys){std::array<std::uint32_t,4> sum{};std::uint32_t k{},ones{},n{};for(int y=ys.begin;y<ys.end;++y)for(int x=xs.begin;x<xs.end;++x){const auto c=src.pixel(x,y);++n;if(src.depth==1){ones+=c.a!=0;continue;}sum[3]+=src.depth==32?c.a/257:c.a;if(c.a){++k;sum[0]+=src.depth==32?c.r/257:c.r;sum[1]+=src.depth==32?c.g/257:c.g;sum[2]+=src.depth==32?c.b/257:c.b;}}
    if(src.depth==1)return {0,0,0,std::uint16_t(ones>n-ones?65535:0)};if(!k)return {0,0,0,0};const auto mul=src.depth==32?257u:1u;return {std::uint16_t(sum[0]/k*mul),std::uint16_t(sum[1]/k*mul),std::uint16_t(sum[2]/k*mul),std::uint16_t(sum[3]/n*mul)};
}
Raster scale(const Raster& src,int w,int h,Budget& b){
    if(w==src.width&&h==src.height)return clone(src,b); // Native wrapper bypasses Low integer products for raw Copy.
    if(std::int64_t(src.width)*w>std::numeric_limits<int>::max()||std::int64_t(src.height)*h>std::numeric_limits<int>::max())throw std::invalid_argument("Native transform partition product exceeds signed DWORD domain");
    b.visit(std::uint64_t((src.width+255)/256+(w+255)/256+2)*std::uint64_t((src.height+255)/256+(h+255)/256+2));
    b.allocate(surface(w,h,src.depth));Raster out(w,h,src.depth,src.resolution);

    if(src.depth==64){std::uint64_t nx{},ny{};for(int x=0;x<w;++x){const auto a=std::int64_t(src.width)*x/w,z=std::max(a+1,std::int64_t(src.width)*(x+1)/w);nx+=std::uint64_t(z-a);}for(int y=0;y<h;++y){const auto a=std::int64_t(src.height)*y/h,z=std::max(a+1,std::int64_t(src.height)*(y+1)/h);ny+=std::uint64_t(z-a);}b.visit(nx*ny+std::uint64_t(w)*std::uint64_t(h));
        for(int y=0;y<h;++y){b.poll();const auto sy=int(std::int64_t(src.height)*y/h),ey=std::max(sy+1,int(std::int64_t(src.height)*(y+1)/h));for(int x=0;x<w;++x){const auto sx=int(std::int64_t(src.width)*x/w),ex=std::max(sx+1,int(std::int64_t(src.width)*(x+1)/w));put(out,x,y,average(src,{sx,ex},{sy,ey}));}}return out;}
    // 105581D0 partitions per destination block and then per backed source
    // block. Its integer remapping deliberately differs from the64 global bins.
    struct Job {Rect target,source;};std::vector<Job> jobs;
    for(int dy=0;dy<h;dy+=256)for(int dx=0;dx<w;dx+=256){Rect d{dx,dy,std::min(256,w-dx),std::min(256,h-dy)};const int sl=int(std::int64_t(src.width)*dx/w),st=int(std::int64_t(src.height)*dy/h),sr=int(std::int64_t(src.width)*(dx+d.width)/w),sb=int(std::int64_t(src.height)*(dy+d.height)/h);
        if(sl>=sr||st>=sb)continue;for(int sy=st/256;sy<=(sb-1)/256;++sy)for(int sx=sl/256;sx<=(sr-1)/256;++sx){if(!src.tiles.contains({sx,sy}))continue;Rect t{std::max(sl,sx*256),std::max(st,sy*256),0,0};const int tr=std::min(sr,(sx+1)*256),tb=std::min(sb,(sy+1)*256);t.width=tr-t.x;t.height=tb-t.y;const int l=std::max(dx,int(std::int64_t(w)*t.x/src.width)),top=std::max(dy,int(std::int64_t(h)*t.y/src.height));const int r=tr==sr?dx+d.width:int(std::int64_t(w)*tr/src.width),bottom=tb==sb?dy+d.height:int(std::int64_t(h)*tb/src.height);if(l>=r||top>=bottom)continue;
            const auto nw=r-l,nh=bottom-top;std::uint64_t nx{},ny{};for(int x=0;x<nw;++x){const auto a=std::int64_t(t.width)*x/nw,z=std::int64_t(t.width)*(x+1)/nw;nx+=std::uint64_t(std::max(a+1,z)-a);}for(int y=0;y<nh;++y){const auto a=std::int64_t(t.height)*y/nh,z=std::int64_t(t.height)*(y+1)/nh;ny+=std::uint64_t(std::max(a+1,z)-a);}b.visit(nx*ny+std::uint64_t(nw)*std::uint64_t(nh));jobs.push_back({{l,top,nw,nh},t});}}
    for(const auto& j:jobs)for(int y=0;y<j.target.height;++y){b.poll();const int sy=j.source.y+int(std::int64_t(j.source.height)*y/j.target.height),ey=std::max(sy+1,j.source.y+int(std::int64_t(j.source.height)*(y+1)/j.target.height));for(int x=0;x<j.target.width;++x){const int sx=j.source.x+int(std::int64_t(j.source.width)*x/j.target.width),ex=std::max(sx+1,j.source.x+int(std::int64_t(j.source.width)*(x+1)/j.target.width));put(out,j.target.x+x,j.target.y+y,average(src,{sx,ex},{sy,ey}));}}return out;
}
Raster mirror(const Raster& src,bool vertical,Budget& b){b.allocate(surface(src.width,src.height,src.depth));b.visit(std::uint64_t(src.width)*std::uint64_t(src.height));Raster out(src.width,src.height,src.depth,src.resolution);for(int y=0;y<src.height;++y){b.poll();for(int x=0;x<src.width;++x)put(out,x,y,src.pixel(vertical?x:src.width-1-x,vertical?src.height-1-y:y));}return out;}
Raster rotate(const Raster& src,std::int32_t degrees,Budget& b){
    const auto neg=std::bit_cast<std::int32_t>(0u-std::bit_cast<std::uint32_t>(degrees));const auto theta=binary64(double(neg)*0x1.1df46a2529d39p-6);const auto c=binary64(std::cos(theta)),s=binary64(std::sin(theta));
    const std::array<int,4> xs{0,integer(binary64(src.width*c)),integer(binary64(binary64(src.height*s)+binary64(src.width*c))),integer(binary64(src.height*s))};
    const std::array<int,4> ys{0,integer(binary64(-double(src.width)*s)),integer(binary64(binary64(src.height*c)-binary64(src.width*s))),integer(binary64(src.height*c))};
    const int w=*std::max_element(xs.begin(),xs.end())-*std::min_element(xs.begin(),xs.end()),h=*std::max_element(ys.begin(),ys.end())-*std::min_element(ys.begin(),ys.end());b.allocate(surface(w,h,src.depth));b.visit(std::uint64_t(w)*std::uint64_t(h));Raster out(w,h,src.depth,src.resolution);
    const auto C=integer(binary64(c*4096)),S=integer(binary64(s*4096));const auto rx=std::int64_t(src.width/2)*4096+std::int64_t(S)*(h/2)-std::int64_t(C)*(w/2),ry=std::int64_t(src.height/2)*4096-std::int64_t(C)*(h/2)-std::int64_t(S)*(w/2);
    for(int y=0;y<h;++y){b.poll();for(int x=0;x<w;++x){const auto px=coord(rx-std::int64_t(S)*y+std::int64_t(C)*x),py=coord(ry+std::int64_t(C)*y+std::int64_t(S)*x);const int sx=px>=0?px/4096:-int((-std::int64_t(px)+4095)/4096),sy=py>=0?py/4096:-int((-std::int64_t(py)+4095)/4096);if(src.contains(sx,sy))put(out,x,y,src.pixel(sx,sy));}}return out;
}
}
Rect floating_transform_bounds(const FloatingTransformQuad& q){double l=q[0].x,r=l,t=q[0].y,b=t;for(auto p:q){point(p);l=std::min(l,p.x);r=std::max(r,p.x);t=std::min(t,p.y);b=std::max(b,p.y);}const auto x=integer(std::floor(l)),y=integer(std::floor(t)),right=integer(std::ceil(r)),bottom=integer(std::ceil(b));const auto w=std::int64_t(right)-x,h=std::int64_t(bottom)-y;(void)surface(coord(w),coord(h),32);return {x,y,int(w),int(h)};}
FloatingTransformPacket floating_transform_scale_packet(const FloatingTransformPacket& old,double x,double y,FloatingTransformPoint a){packet(old);point(a);if(!std::isfinite(x)||!std::isfinite(y)||x==0||y==0)throw std::invalid_argument("Invalid transform scale");auto p=old;const auto tx=binary64(binary64(-a.x*x)+a.x),ty=binary64(binary64(-a.y*y)+a.y);for(auto& q:p.quad){q.x=binary64(binary64(q.x*x)+tx);q.y=binary64(binary64(q.y*y)+ty);}p.pivot={binary64(binary64(p.pivot.x*x)+tx),binary64(binary64(p.pivot.y*y)+ty)};p.angle=0;p.reversals=(x<0?0x10u:0)|(y<0?0x20u:0);packet(p);return p;}
FloatingTransformPacket floating_transform_rotate_packet(const FloatingTransformPacket& old,std::int32_t degrees,FloatingTransformPoint pivot){packet(old);point(pivot);auto p=old;const auto theta=binary64(double(degrees)*0x1.1df46a2529d39p-6),c=binary64(std::cos(theta)),s=binary64(std::sin(theta));const auto tx=binary64(binary64(binary64(-pivot.x*c)+binary64(pivot.y*s))+pivot.x),ty=binary64(binary64(binary64(-pivot.x*s)+binary64(-pivot.y*c))+pivot.y);for(auto& q:p.quad){const auto x=q.x,y=q.y;q.x=binary64(binary64(binary64(x*c)+binary64(y*-s))+tx);q.y=binary64(binary64(binary64(x*s)+binary64(y*c))+ty);}p.pivot=pivot;p.angle=degrees;p.reversals=0;packet(p);return p;}
FloatingTransformPacket floating_transform_translate_packet(const FloatingTransformPacket& old,double x,double y){packet(old);if(!std::isfinite(x)||!std::isfinite(y))throw std::invalid_argument("Invalid transform translation");auto p=old;for(auto& q:p.quad){q.x=binary64(q.x+x);q.y=binary64(q.y+y);}p.pivot={binary64(p.pivot.x+x),binary64(p.pivot.y+y)};packet(p);return p;}
struct FloatingTransformSession::Impl {
    struct Record {std::uint32_t flags{};Rect bounds;FloatingTransformPacket packet;std::int32_t angle{};std::array<int,2> delta{},base{};};
    const Document* owner{};
    std::array<std::uint64_t,3> generations{};
    DocumentData original,cut,expected;
    RegionSelection original_selection,selected,expected_selection;
    std::shared_ptr<const FloatingSelection> prior,payload,live,expected_floating;
    std::shared_ptr<const raster_history_detail::Edit> capture_history;
    FloatingTransformState view;
    Rect initial_frame;
    std::array<int,2> displacement{};
    int helper_width{},helper_height{};
    std::vector<Record> records;
    std::uint64_t retained{},reservation{},selected_storage{};
    bool bound{},failed_capture{},source_history{},indication_active{},indication_redraw{};
    std::shared_ptr<const DocumentData> baseline;
};
namespace {
FloatingTransformPacket rectangle_packet(Rect r){const auto right=double(std::int64_t(r.x)+r.width),bottom=double(std::int64_t(r.y)+r.height);FloatingTransformPacket p;p.quad={FloatingTransformPoint{double(r.x),double(r.y)},{right,double(r.y)},{right,bottom},{double(r.x),bottom}};p.pivot={binary64((double(r.x)+right)*.5),binary64((double(r.y)+bottom)*.5)};return p;}
void operation(FloatingTransformOperation op){if(op!=FloatingTransformOperation::Scale&&op!=FloatingTransformOperation::Rotate)throw std::invalid_argument("Unsupported floating transform operation");}
Raster selection_crop(const RegionSelection& s,Rect r,double dpi,Budget& b){b.allocate(surface(r.width,r.height,1));b.visit(std::uint64_t(r.width)*std::uint64_t(r.height));Raster out(r.width,r.height,1,dpi);for(int y=0;y<r.height;++y){b.poll();for(int x=0;x<r.width;++x){const auto gx=coord(std::int64_t(r.x)+x),gy=coord(std::int64_t(r.y)+y);if(s.contains_document(gx,gy))put(out,x,y,{0,0,0,65535});}}return out;}
template<class Records> Raster replay_pixels(Raster input,const Records& records,Budget& b){bool parity=true;for(std::size_t i=0;i<records.size();++i){b.poll();const auto flags=records[i].flags;if(!flags)continue;if(i+1<records.size()&&flags==records[i+1].flags){if(flags==16||flags==32)parity=!parity;continue;}
        if(flags==4)input=rotate(input,records[i].angle,b);else if(flags&2){input=scale(input,records[i].bounds.width,records[i].bounds.height,b);if(flags&16)input=mirror(input,false,b);if(flags&32)input=mirror(input,true,b);}else if(flags==16||flags==32){if(parity)input=mirror(input,flags==32,b);else parity=true;}else throw std::logic_error("Unsupported transform pixel record");}return input;}

}
FloatingTransformSession::FloatingTransformSession(std::shared_ptr<const Impl> p):impl_(std::move(p)){}
std::shared_ptr<FloatingTransformSession> FloatingTransformSession::prepare(const Document& d,FloatingTransformOperation op,std::uint64_t origin,std::stop_token stop,FloatingTransformBegin& result){
    operation(op);result=FloatingTransformBegin::NoSelection;if(!d.floating_selection_&&!d.region_selection.active()&&!d.vector_indication_active)return {};
    const bool indication_active=d.vector_indication_active;const auto flags=d.application_vector_selection_flags.value_or(d.region_selection.vector_flags);
    const auto generations=std::array<std::uint64_t,3>{d.state_generation_,d.region_selection_generation_,d.floating_selection_generation_};
    if(!d.floating_selection_&&!d.region_selection.active())for(const auto& layer:d.data.layers)
        if(layer.common.flags[1]&&layer.common.flags[2]&&layer.common.runtime_operable&&((layer.family&0x20000000u)||selection_detail::extension_move_owner(layer)))
            throw std::invalid_argument("State-zero vector Transform capture is outside the raster profile");
    Budget b{0,0,stop};b.poll();const auto docbytes=document_storage(d.data);
    // One admission reservation covers the complete Begin graph, rather than
    // independent per-phase caps: rollback/cut/payload, published data and
    // floater, expected data and floater, plus capture/rollback publication.
    // Private live geometry shares the immutable payload pixels. Source-graph equivalents
    // conservatively cover selective-copy metadata, partial-cut COW and raw
    // snapshot working peaks; later releases share this retained reservation.
    std::uint64_t source=docbytes,metadata=document_metadata(d.data);const auto selection_bytes=selection_storage(d.region_selection);if(selection_bytes>storage_limit-source)throw std::length_error("Transform source mask exceeds512MiB");source+=selection_bytes;if(d.floating_selection_){const auto n=floating_storage(*d.floating_selection_);if(n[0]+n[1]>storage_limit-source)throw std::length_error("Transform source graph exceeds512MiB");source+=n[0]+n[1];metadata+=n[0];if(d.floating_selection_->layers)for(const auto& l:*d.floating_selection_->layers)for(const auto& p:l.planes)if(p.raster)metadata+=p.raster->tiles.size()*128;}
    if(d.data.original){if(d.data.original->size()>storage_limit-source)throw std::length_error("Transform raw source exceeds512MiB");source+=d.data.original->size();}
    const auto baseline_charge=[&](const std::shared_ptr<const DocumentData>& baseline){if(!baseline)return;if(baseline->original_content)throw std::invalid_argument("Nested transform persistence baseline unsupported");const auto n=document_storage(*baseline);if(n>storage_limit-source)throw std::length_error("Transform semantic baseline exceeds512MiB");source+=n;metadata+=document_metadata(*baseline);if(baseline->original&&baseline->original!=d.data.original){if(baseline->original->size()>storage_limit-source)throw std::length_error("Transform baseline raw source exceeds512MiB");source+=baseline->original->size();}};
    baseline_charge(d.raw_baseline_);if(d.data.original_content!=d.raw_baseline_)baseline_charge(d.data.original_content);
    // Eight retained/publication roles plus two working roles make Cancel
    // possible without exceeding the same aggregate resident-storage cap.
    if(source>storage_limit/10)throw std::length_error("Complete transform Begin/Cancel snapshot peak exceeds512MiB");
    if(metadata>(storage_limit-source*10)/10)throw std::length_error("Complete transform metadata snapshot peak exceeds512MiB");
    // Value-owned metadata is copied by staged Impl construction as well as
    // publication. Reserve eight additional metadata equivalents for those
    // transient old/candidate/bound snapshots; raster pixels remain shared
    // across Impl copies and retain their eight-role reservation.
    const auto reserved=source*8+metadata*8;
    if(d.region_selection.mask){const auto count=std::uint64_t(d.region_selection.mask->width())*std::uint64_t(d.region_selection.mask->height());b.visit(count);}
    if(!d.floating_selection_)admit_transform_floating_capture_work(d.data,d.region_selection,work_limit-b.work);
    auto p=std::make_shared<Impl>();p->reservation=reserved;p->retained=reserved;p->owner=&d;p->generations=generations;p->indication_active=indication_active;p->original=clone(d.data,b);
    const auto freeze_bytes=[&](const std::shared_ptr<const Bytes>& source){if(!source)return std::shared_ptr<const Bytes>{};b.allocate(source->size());return std::make_shared<const Bytes>(*source);};
    p->original.original=freeze_bytes(d.data.original);
    const auto freeze_baseline=[&](const std::shared_ptr<const DocumentData>& source){if(!source)return std::shared_ptr<const DocumentData>{};if(source->original_content)throw std::invalid_argument("Nested transform persistence baseline unsupported");auto frozen=clone(*source,b);frozen.original=source->original==d.data.original?p->original.original:freeze_bytes(source->original);return std::make_shared<const DocumentData>(std::move(frozen));};
    p->baseline=freeze_baseline(d.raw_baseline_);p->original.original_content=d.data.original_content==d.raw_baseline_?p->baseline:freeze_baseline(d.data.original_content);
    p->original_selection=clone(d.region_selection,b);p->prior=clone(d.floating_selection_,b);
    auto capture_selection=p->original_selection;capture_selection.vector_flags=flags;
    const auto capture_tail=[&]{
        const bool mask=capture_selection.state==SelectionLayerState::Mask||capture_selection.state==SelectionLayerState::VectorIndicated;
        const double dpi=mask&&!(flags&1u)?selection_carrier_dpi(capture_selection,p->cut.base.dpi):0;
        p->indication_active=vector_indication_detail::refresh_marks(p->cut,indication_active,capture_selection,flags,dpi,b.bytes,b.work,stop);
        p->indication_redraw=indication_active||p->indication_active;b.poll();
    };
    const auto failed_capture=[&]{
        p->failed_capture=true;b.poll();
        result=FloatingTransformBegin::NoUsablePayload;
        return std::shared_ptr<FloatingTransformSession>(new FloatingTransformSession(p));
    };
    if(p->prior){profile(p->original,*p->prior,b);p->payload=clone(p->prior,b);p->cut=clone(p->original,b);}else{
        auto capture_work=work_limit-b.work,capture_bytes=storage_limit-b.bytes;auto capture=prepare_transform_floating_capture(p->original,capture_selection,capture_work,capture_bytes,stop);b.work=work_limit-capture_work;b.bytes=storage_limit-capture_bytes;p->source_history=capture.source_history;p->cut=std::move(capture.remainder);capture_tail();if(!capture.floating)return failed_capture();profile(p->cut,*capture.floating,b);p->payload=clone(capture.floating,b,true);
        const auto history_start=b.bytes;raster_history_detail::Budget history_budget{b.bytes,b.work,stop};
        p->capture_history=raster_history_detail::Edit::capture(p->original,*p->payload,p->original_selection,history_budget);
        b.bytes=history_budget.bytes;b.work=history_budget.work;
        // Missing64 blocks can become present in both payload and history
        // offscreens. Charge their growth over the source-sized role credits
        // before the payload gains publication/expected copies.
        const auto payload_storage=floating_storage(*p->payload);const auto payload_bytes=payload_storage[0]+payload_storage[1];
        const auto payload_growth=payload_bytes>docbytes?payload_bytes-docbytes:0;
        const auto history_growth=b.bytes-history_start>docbytes?b.bytes-history_start-docbytes:0;
        const auto cancel_working=2*(source+metadata);
        if(payload_growth>(storage_limit-reserved-cancel_working)/4)throw std::length_error("Expanded transform payload exceeds Begin/Cancel storage peak");
        const auto retained=reserved+payload_growth*4;
        if(history_growth>storage_limit-retained-cancel_working)throw std::length_error("Expanded transform history exceeds Begin/Cancel storage peak");
        p->retained=retained+history_growth;p->reservation=p->retained;
        auto preview_work=work_limit-b.work;
        const bool usable=floating_preview_detail::transform_has_blocks(p->cut,*p->payload,preview_work,storage_limit-b.bytes,stop);b.work=work_limit-preview_work;
        if(!usable)return failed_capture();
    }
    p->selected=clone(p->original_selection,b);p->initial_frame=translated(p->payload->tight_bounds,p->payload->delta[0],p->payload->delta[1]);(void)surface(p->initial_frame.width,p->initial_frame.height,32);p->view={rectangle_packet(p->initial_frame),p->payload->tight_bounds,p->payload->delta,{},op,origin,1};packet(p->view.packet);p->helper_width=p->original.base.width();p->helper_height=p->original.base.height();
    p->records.push_back({0,p->initial_frame,p->view.packet,0,p->view.delta,{}});p->live=p->payload;
    auto session=std::shared_ptr<FloatingTransformSession>(new FloatingTransformSession(std::move(p)));result=FloatingTransformBegin::Started;return session;
}
void FloatingTransformSession::bind(const Document& d,const DocumentData& data,const RegionSelection& selected,std::shared_ptr<const FloatingSelection> floating,std::array<std::uint64_t,3> generations,std::stop_token stop){
    if(impl_->failed_capture)throw std::logic_error("A failed Transform preparation cannot become a session");
    if(stop.stop_requested())throw std::runtime_error("Floating transform cancelled");auto p=std::make_shared<Impl>(*impl_);
    // Expected snapshots replace their pre-admitted Begin roles. Do not bill
    // them a second time on top of the complete reservation at every Preview.
    Budget b{storage_limit-p->reservation-p->selected_storage/2,0,stop};b.poll();
    p->owner=&d;p->generations=generations;p->expected=clone(data,b);p->expected_selection=clone(selected,b);p->expected_floating=clone(floating,b);p->bound=true;b.poll();impl_=std::move(p);
}
bool FloatingTransformSession::matches(const Document& d) const {const auto& p=*impl_;return p.owner==&d&&p.generations==std::array<std::uint64_t,3>{d.state_generation_,d.region_selection_generation_,d.floating_selection_generation_}&&document_equal(d.data,p.bound?p.expected:p.original)&&selection_equal(d.region_selection,p.bound?p.expected_selection:p.original_selection)&&floating_equal(d.floating_selection_,p.bound?p.expected_floating:p.prior);}
FloatingTransformState FloatingTransformSession::state() const{return impl_->view;}
const DocumentData& FloatingTransformSession::before() const{return impl_->original;}
DocumentData FloatingTransformSession::snapshot_before() const{Budget b{storage_limit-impl_->reservation,0,{}};return clone(impl_->original,b);}
const DocumentData& FloatingTransformSession::admitted_data() const{return impl_->bound?impl_->expected:impl_->original;}
bool FloatingTransformSession::failed_capture_changed() const{return impl_->failed_capture&&(impl_->source_history||impl_->indication_redraw);}
bool FloatingTransformSession::failed_capture_source_history() const{return impl_->failed_capture&&impl_->source_history;}
bool FloatingTransformSession::capture_indication_active() const{return impl_->indication_active;}
std::shared_ptr<const DocumentData> FloatingTransformSession::raw_baseline() const{if(!impl_->baseline)return {};Budget b{storage_limit-impl_->reservation,0,{}};return std::make_shared<const DocumentData>(clone(*impl_->baseline,b));}
DocumentData FloatingTransformSession::remainder() const{Budget b{storage_limit-impl_->reservation,0,{}};return clone(impl_->cut,b);}
const RegionSelection& FloatingTransformSession::selection_before() const{return impl_->original_selection;}
const RegionSelection& FloatingTransformSession::selection_live() const{return impl_->selected;}
std::shared_ptr<const FloatingSelection> FloatingTransformSession::prior_floating() const{Budget b{storage_limit-impl_->reservation,0,{}};return clone(impl_->prior,b);}
std::shared_ptr<const FloatingSelection> FloatingTransformSession::live_floating() const{Budget b{storage_limit-impl_->reservation,0,{}};return clone(impl_->live,b);}
std::shared_ptr<const raster_history_detail::Edit> FloatingTransformSession::capture_history() const{return impl_->capture_history;}
std::array<std::uint64_t,2> FloatingTransformSession::available_budget() const{return {work_limit,storage_limit-impl_->retained};}
std::shared_ptr<FloatingTransformSession> FloatingTransformSession::command(std::uint32_t flags,std::stop_token stop) const{
    if(flags!=2&&flags!=4&&flags!=16&&flags!=32)throw std::invalid_argument("Invalid native Transform command flags");
    if(stop.stop_requested())throw std::runtime_error("Floating transform command cancelled");
    //617550 restores the service quad from released live bounds + delta.
    // Numeric preview never creates a payload record at this boundary.
    auto value=impl_->records.back().packet;
    const auto& state=impl_->view;const auto box=translated(state.bounds,state.delta[0],state.delta[1]);
    const int right=coord(std::int64_t(box.x)+box.width),bottom=coord(std::int64_t(box.y)+box.height);
    value.quad={FloatingTransformPoint{double(box.x),double(box.y)},{double(right),double(box.y)},{double(right),double(bottom)},{double(box.x),double(bottom)}};
    value.angle=0;value.reversals=0; //New command interaction, existing records stay intact.
    if(flags&0x30)return release(value,flags,0,stop);
    auto p=std::make_shared<Impl>(*impl_);p->view.packet=value;p->view.operation=FloatingTransformOperation(flags);
    return std::shared_ptr<FloatingTransformSession>(new FloatingTransformSession(std::move(p)));
}
std::shared_ptr<FloatingTransformSession> FloatingTransformSession::switch_operation(FloatingTransformOperation op) const{operation(op);auto p=std::make_shared<Impl>(*impl_);p->view.operation=op;return std::shared_ptr<FloatingTransformSession>(new FloatingTransformSession(std::move(p)));}
std::shared_ptr<FloatingTransformSession> FloatingTransformSession::preview(const FloatingTransformPacket& value,std::stop_token stop) const{packet(value);if(stop.stop_requested())throw std::runtime_error("Floating transform preview cancelled");auto p=std::make_shared<Impl>(*impl_);p->view.packet=value;return std::shared_ptr<FloatingTransformSession>(new FloatingTransformSession(std::move(p)));}
std::shared_ptr<FloatingTransformSession> FloatingTransformSession::position(int dx,int dy,std::optional<FloatingTransformPoint> pivot)const{
    auto p=std::make_shared<Impl>(*impl_);
    auto& view=p->view;
    auto& released=p->records.back();
    view.packet=floating_transform_translate_packet(released.packet,dx,dy);
    if(pivot){point(*pivot);view.packet.pivot=*pivot;}
    released.packet=view.packet;
    released.bounds=translated(released.bounds,dx,dy);
    for(int i=0;i<2;++i){
        const int offset=i?dy:dx;
        p->displacement[i]=coord(std::int64_t(p->displacement[i])+offset);
        view.delta[i]=coord(std::int64_t(view.delta[i])+offset);
        if(p->selected.active())p->selected.origin[i]=coord(std::int64_t(p->selected.origin[i])+offset);
    }
    auto live=std::make_shared<FloatingSelection>(*p->live);
    live->delta=view.delta;p->live=std::move(live);
    return std::shared_ptr<FloatingTransformSession>(new FloatingTransformSession(std::move(p)));
}
std::shared_ptr<FloatingTransformSession> FloatingTransformSession::release(const FloatingTransformPacket& value,std::uint32_t flags,std::int32_t angle,std::stop_token stop) const{
    packet(value);if(flags!=4&&flags!=16&&flags!=32&&flags!=2&&flags!=18&&flags!=34&&flags!=50)throw std::invalid_argument("Unsupported floating transform release flags");auto p=std::make_shared<Impl>(*impl_);if(p->records.size()>=4097)throw std::length_error("Too many floating transform records");Budget b{p->retained,0,stop};b.poll();p->view.packet=value;auto frame=floating_transform_bounds(value.quad);
    if(flags&2){if(flags&16){std::swap(p->view.packet.quad[0],p->view.packet.quad[1]);std::swap(p->view.packet.quad[2],p->view.packet.quad[3]);}if(flags&32){std::swap(p->view.packet.quad[0],p->view.packet.quad[3]);std::swap(p->view.packet.quad[1],p->view.packet.quad[2]);}}
    // Helper expansion moves its origin by the negative crop edge, advancing
    // both live delta and B0/B4; final target position subtracts B0/B4 again.
    auto local=translated(frame,coord(-std::int64_t(p->view.delta[0])),coord(-std::int64_t(p->view.delta[1])));const auto padded=[](int edge){return edge<0?coord(-(((-std::int64_t(edge)+255)/256)*256)):0;};const int px=padded(local.x),py=padded(local.y);if((flags&6)&& (px||py||std::int64_t(local.x)+local.width>p->helper_width||std::int64_t(local.y)+local.height>p->helper_height)){
        p->helper_width=coord(std::max<std::int64_t>(p->helper_width,std::int64_t(local.x)+local.width)-px);p->helper_height=coord(std::max<std::int64_t>(p->helper_height,std::int64_t(local.y)+local.height)-py);p->view.delta={coord(std::int64_t(p->view.delta[0])+px),coord(std::int64_t(p->view.delta[1])+py)};p->view.base_compensation={coord(std::int64_t(p->view.base_compensation[0])+px),coord(std::int64_t(p->view.base_compensation[1])+py)};if(p->view.delta[0]==std::numeric_limits<int>::min()||p->view.delta[1]==std::numeric_limits<int>::min())throw std::invalid_argument("Transform padded origin outside admitted coordinate domain");(void)surface(p->helper_width,p->helper_height,32);local=translated(local,coord(-std::int64_t(px)),coord(-std::int64_t(py)));}
    b.allocate(sizeof(Impl::Record));p->retained+=sizeof(Impl::Record);p->view.bounds=local;p->records.push_back({flags,frame,p->view.packet,angle,p->view.delta,p->view.base_compensation});p->view.records=p->records.size();
    // Preview resources are produced independently from raster payloads. The
    // selection is a1bit cropped snapshot, not a payload alpha approximation.
    if(p->original_selection.active()){
    auto mask=selection_crop(p->original_selection,p->initial_frame,p->original.base.dpi,b);mask=replay_pixels(std::move(mask),p->records,b);const auto size=selection_carrier_size(p->original_selection,p->original.base.width(),p->original.base.height());auto origin=p->original_selection.origin;for(int i=0;i<2;++i)origin[i]=coord(std::int64_t(origin[i])+p->displacement[i]);const auto left=coord(std::int64_t(frame.x)-origin[0]),top=coord(std::int64_t(frame.y)-origin[1]);
    const auto storage=std::uint64_t((size[0]+31)/32)*std::uint64_t(size[1])*4+std::uint64_t((size[0]+255)/256)*std::uint64_t((size[1]+255)/256);
    if(storage*2>storage_limit-(p->retained-p->selected_storage))throw std::length_error("Transform retained selection snapshots exceed512MiB");
    b.allocate(storage*2);b.visit(std::uint64_t(mask.width)*std::uint64_t(mask.height));auto selected=std::make_shared<BinaryMask>(size[0],size[1]);for(int y=0;y<mask.height;++y){b.poll();for(int x=0;x<mask.width;++x)if(mask.pixel(x,y).a)selected->set(coord(std::int64_t(left)+x),coord(std::int64_t(top)+y));}
    p->retained=p->retained-p->selected_storage+storage*2;p->selected_storage=storage*2;
    p->selected={SelectionLayerState::Mask,selected->extent({0,0,size[0],size[1]}),origin,std::move(selected),size};
    p->selected.carrier_dpi=p->original_selection.carrier_dpi;p->selected.vector_flags=p->original_selection.vector_flags;}
    auto live=std::make_shared<FloatingSelection>(*p->payload);live->delta=p->view.delta;live->tight_bounds=local;p->live=std::move(live);
    // Validate every payload's ordered kernels now, so invalid releases fail
    // before publishing selection/geometry and Accept cannot hide a bad record.
    for(const auto& l:*p->payload->layers)for(const auto& plane:l.planes){const auto local_initial=translated(p->initial_frame,coord(-std::int64_t(l.common.x)-p->payload->delta[0]),coord(-std::int64_t(l.common.y)-p->payload->delta[1]));auto tmp=crop(*plane.raster,local_initial,b);(void)replay_pixels(std::move(tmp),p->records,b);}
    return std::shared_ptr<FloatingTransformSession>(new FloatingTransformSession(std::move(p)));
}
FloatingMaterialization FloatingTransformSession::materialize(std::stop_token stop,std::uint64_t* work_remaining,std::uint64_t* bytes_remaining,std::vector<std::shared_ptr<const raster_history_detail::Edit>>* paste_history) const{
    const auto& p=*impl_;Budget b{bytes_remaining?storage_limit-*bytes_remaining:p.retained,work_remaining?work_limit-*work_remaining:0,stop};b.poll();auto payload=std::make_shared<FloatingSelection>(*p.payload);auto layers=std::make_shared<std::vector<Layer>>(*p.payload->layers);const auto frame=p.records.back().bounds;
    for(auto& l:*layers){const auto dx=coord(std::int64_t(p.view.delta[0])-p.view.base_compensation[0]),dy=coord(std::int64_t(p.view.delta[1])-p.view.base_compensation[1]);l.common.x=coord(std::int64_t(l.common.x)+dx);l.common.y=coord(std::int64_t(l.common.y)+dy);auto target=translated(frame,coord(-std::int64_t(l.common.x)),coord(-std::int64_t(l.common.y)));
        if(target.x<0||target.y<0||std::int64_t(target.x)+target.width>l.common.width||std::int64_t(target.y)+target.height>l.common.height){const auto left=std::min(0,target.x),top=std::min(0,target.y);layer_move_detail::expand_raster_layer(l,{left,top,coord(std::max<std::int64_t>(l.common.width,std::int64_t(target.x)+target.width)-left),coord(std::max<std::int64_t>(l.common.height,std::int64_t(target.y)+target.height)-top)},b.bytes,b.work,stop);target=translated(frame,coord(-std::int64_t(l.common.x)),coord(-std::int64_t(l.common.y)));}
        const auto initial=translated(p.initial_frame,coord(std::int64_t(p.displacement[0])-l.common.x),coord(std::int64_t(p.displacement[1])-l.common.y));
        for(auto& plane:l.planes){auto tmp=crop(*plane.raster,initial,b);tmp=replay_pixels(std::move(tmp),p.records,b);b.allocate(surface(plane.raster->width,plane.raster->height,plane.raster->depth));const auto columns=std::min(target.width,tmp.width),rows=std::min(target.height,tmp.height);b.visit(std::uint64_t(columns)*std::uint64_t(rows));Raster out(plane.raster->width,plane.raster->height,plane.raster->depth,plane.raster->resolution);for(int y=0;y<rows;++y){b.poll();for(int x=0;x<columns;++x)if(tmp.tiles.contains({x/256,y/256}))put(out,coord(std::int64_t(target.x)+x),coord(std::int64_t(target.y)+y),tmp.pixel(x,y));}plane.raster=std::move(out);plane.pixels_changed=true;}
    }
    payload->layers=std::move(layers);payload->delta={};payload->tight_bounds=frame;
    if(paste_history){raster_history_detail::Budget history_budget{b.bytes,b.work,stop};*paste_history=raster_history_detail::Edit::paste(p.cut,*payload,history_budget);b.bytes=history_budget.bytes;b.work=history_budget.work;}
    auto base=clone(p.cut,b);auto remaining_work=work_limit-b.work,remaining_bytes=storage_limit-b.bytes;auto result=prepare_transform_floating_materialization(base,*payload,remaining_work,remaining_bytes,stop);if(work_remaining)*work_remaining=remaining_work;if(bytes_remaining)*bytes_remaining=remaining_bytes;return result;
}
Raster FloatingTransformSession::transformed_plane(std::size_t layer,std::size_t plane,std::stop_token stop) const {const auto& p=*impl_;const auto& l=p.payload->layers->at(layer);const auto& source=*l.planes.at(plane).raster;Budget b{p.retained,0,stop};const auto initial=translated(p.initial_frame,coord(-std::int64_t(l.common.x)-p.payload->delta[0]),coord(-std::int64_t(l.common.y)-p.payload->delta[1]));auto tmp=crop(source,initial,b);return replay_pixels(std::move(tmp),p.records,b);}
Raster FloatingTransformSession::preview_raster(int target_depth,std::stop_token stop) const{
    if(target_depth!=32&&target_depth!=64)throw std::invalid_argument("Transform preview depth must be32 or64");auto preview_session=std::shared_ptr<FloatingTransformSession>(new FloatingTransformSession(impl_));if(impl_->view.packet!=impl_->records.back().packet){
        const auto& released=impl_->records.back().packet;const auto& pending=impl_->view.packet;
        const auto dx=pending.quad[0].x-released.quad[0].x,dy=pending.quad[0].y-released.quad[0].y;
        const bool move=dx==std::trunc(dx)&&dy==std::trunc(dy)&&dx>=INT_MIN&&dx<=INT_MAX&&dy>=INT_MIN&&dy<=INT_MAX&&
            std::ranges::equal(pending.quad,released.quad,[&](auto a,auto b){return a.x==b.x+dx&&a.y==b.y+dy;});
        if(move)preview_session=position(int(dx),int(dy),pending.pivot);
        else {
            const auto flags=impl_->view.operation==FloatingTransformOperation::Rotate?4u:2u|pending.reversals;
            preview_session=release(pending,flags,pending.angle,stop);
        }
    }
    Budget b{impl_->retained,0,stop};b.allocate(surface(impl_->original.base.width(),impl_->original.base.height(),target_depth));for(const auto& l:impl_->original.layers)if(l.common.flags[1])for(const auto& p:l.planes)if(p.common.flags[1]){if(!p.raster)throw std::invalid_argument("Transform preview requires renderable raster companions");if(p.raster->depth==1&&p.raster->resolution==impl_->original.base.dpi&&l.common.opacity_mode==2)b.visit(std::uint64_t(p.raster->width)*std::uint64_t(p.raster->height));else for(const auto& [q,bytes]:p.raster->tiles){const auto w=std::min(256,p.raster->width-q.x*256),h=std::min(256,p.raster->height-q.y*256);b.visit(std::uint64_t(w)*std::uint64_t(h));}}auto remaining_work=work_limit-b.work,remaining_bytes=storage_limit-b.bytes;auto materialized=preview_session->materialize(stop,&remaining_work,&remaining_bytes);b.work=work_limit-remaining_work;b.bytes=storage_limit-remaining_bytes;auto image=materialized.data.composite(target_depth,stop);b.poll();return clone(image,b);
}
}
