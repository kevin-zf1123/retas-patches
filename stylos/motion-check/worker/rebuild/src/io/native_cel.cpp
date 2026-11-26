#include "retas/document.hpp"
#include "native_vector_canonical.hpp"
#include "../core/native_storage.hpp"
#include "../core/vector_line_output.hpp"
#include <algorithm>
#include <bit>
#include <limits>
#include <set>

namespace retas {
namespace {
const std::set<std::uint32_t> layer_families{
    0x10000100,0x10010000,0x10000400,0x10008000,0x10020000,0x10040000,0x11000800,
    0x12000200,0x14002000,0x11001000,0x18004000,0x40000800,0x40001000,0x40008000,
    0x40010000,0x40020000,0x48040800,0x40000200,0x22100000,0x24400000,0x20000200,
    0x21200000,0x20800000,0x40004000,0x40002000};
const std::set<std::uint32_t> plane_families{
    0x10000001,0x11110002,0x11410001,0x11210001,0x11220001,0x11240001,0x12100001,
    0x12400001,0x11110001,0x21110001,0x21120001,0x21210001,0x21220001,0x24100001,
    0x22100001,0x22100002,0x40000001,0x40000002,0x40000010,0x40000004,0x40000080,
    0x40001008,0x40002008};
const std::set<std::uint32_t> raster_families{
    0x10000001,0x11110002,0x11410001,0x11210001,0x11220001,0x11240001,0x12100001,0x12400001,0x11110001};
Length length(Reader& r){Length l;l.value=r.f64();l.unit=r.i32();l.reference=r.f64();return l;}
void length(Writer& w,const Length& l){w.f64(l.value);w.i32(l.unit);w.f64(l.reference);}
CommonElement common(Reader r){
    CommonElement c;c.width=r.i32();c.height=r.i32();c.depth=r.i32();c.resolution=r.f64();c.name=r.string();
    for(auto& v:c.flags)v=r.i32();c.color=r.u64();c.x=r.i32();c.y=r.i32();
    for(auto& l:c.position)l=length(r);c.opacity=r.i32();c.opacity_mode=r.i32();c.field_14=r.i32();c.suffix=r.rest();native_vector_detail::load_position(c);return c;
}
Bytes common(const CommonElement& c,int ordinal){
    Writer w;w.i32(c.width);w.i32(c.height);w.i32(c.depth);w.f64(c.resolution);w.string(c.name);
    for(auto v:c.flags)w.i32(v);w.u64(c.color);w.i32(c.x);w.i32(c.y);
    for(const auto& l:c.position)length(w,l);w.i32(c.opacity);w.i32(c.opacity_mode);w.i32(ordinal);w.raw(c.suffix);return w.data;
}
BaseInfo base(Reader r){
    BaseInfo b;for(int i=0;i<2;++i)b.lengths[i]=length(r);b.frame_mode=r.i32();
    for(int i=2;i<14;++i)b.lengths[i]=length(r);b.scale=r.f64();
    b.lengths[14]=length(r);b.lengths[15]=length(r);b.dpi=r.f64();b.dialog_dpi=r.f64();b.flags=r.i32();b.suffix=r.rest();
    (void)b.width();(void)b.height();return b;
}
Bytes base(const BaseInfo& b){
    Writer w;for(int i=0;i<2;++i)length(w,b.lengths[i]);w.i32(b.frame_mode);
    for(int i=2;i<14;++i)length(w,b.lengths[i]);w.f64(b.scale);
    length(w,b.lengths[14]);length(w,b.lengths[15]);w.f64(b.dpi);w.f64(b.dialog_dpi);w.i32(b.flags);w.raw(b.suffix);return w.data;
}
void validate_index(int index,int count,Reader& r){
    if(index < -1 || index>=count)throw FormatError(r.offset(),"Invalid active collection index");
}
int collection_count(std::size_t size){
    if(size>static_cast<std::size_t>(std::numeric_limits<int>::max()))throw std::length_error("Native collection exceeds signed 32-bit count");
    return static_cast<int>(size);
}
Plane plane(std::uint32_t id,Reader r){
    Plane p;p.family=id;
    if(!plane_families.contains(id)){p.opaque=true;p.opaque_record=r.rest();return p;}
    p.common=common(r.section());p.specific=r.section().rest();p.suffix=r.rest();
    if(raster_families.contains(id)){
        Reader payload(p.specific);p.raster=decode_offscreen(payload);
        auto& im=*p.raster;
        if(im.width!=p.common.width||im.height!=p.common.height||im.depth!=p.common.depth||
           std::bit_cast<std::uint64_t>(im.resolution)!=std::bit_cast<std::uint64_t>(p.common.resolution))
            throw FormatError(r.offset(),"Plane/offscreen metadata mismatch");
    }
    return p;
}
bool matches_offscreen(const Plane& plane){
    if(!plane.raster||plane.specific.empty())return false;
    Reader reader(plane.specific);const auto stored=decode_offscreen(reader);const auto& live=*plane.raster;
    if(stored.width!=live.width||stored.height!=live.height||stored.depth!=live.depth||stored.color_order!=live.color_order
       ||std::bit_cast<std::uint64_t>(stored.resolution)!=std::bit_cast<std::uint64_t>(live.resolution)||stored.tiles.size()!=live.tiles.size())return false;
    auto other=stored.tiles.begin();
    for(const auto& [key,backing]:live.tiles){if(key!=other->first||!backing||!other->second||*backing!=*other->second)return false;++other;}
    return true;
}
Bytes plane(const Plane& p,int ordinal,std::optional<int> compression){
    if(p.opaque)return p.opaque_record;
    Writer w;w.section(common(p.common,ordinal));
    // The canonical document candidate has already applied native Optimize.
    // Only changed topology/pixels replaces the original offscreen section.
    if(raster_families.contains(p.family)&&p.raster&&(compression||p.pixels_changed||!matches_offscreen(p))){
        Writer specific;specific.raw(encode_offscreen(*p.raster,compression.value_or(6)));
        if(!p.specific.empty()){
            Reader previous(p.specific);(void)previous.section();
            specific.raw(previous.rest()); // family bytes after the offscreen
        }
        w.section(specific.data);
    }else w.section(p.specific);
    w.raw(p.suffix);return w.data;
}
Layer layer(std::uint32_t id,Reader r){
    Layer l;l.family=id;
    if(!layer_families.contains(id)){l.opaque=true;l.opaque_record=r.rest();return l;}
    l.common=common(r.section());l.specific=r.section().rest();
    auto planes=r.section();const auto count=planes.i32();l.current_plane=planes.i32();
    planes.require_count(count,8);validate_index(l.current_plane,count,planes);
    for(int i=0;i<count;++i){const auto family=planes.u32();l.planes.push_back(plane(family,planes.section()));}
    //4346C0 is the PaintMan host's runtime family table, not a wire flag.
    //Fresh constructors keep +1C=1; file load applies this owner gate to all children.
    constexpr std::array<std::uint32_t,9> operable{0x11000800,0x11001000,0x21200000,0x18004000,0x40001000,0x40020000,0x40000200,0x10008000,0x10000100};
    l.common.runtime_operable=std::ranges::find(operable,id)!=operable.end();
    for(auto& child:l.planes)child.common.runtime_operable=l.common.runtime_operable;
    l.plane_suffix=planes.rest();l.suffix=r.rest();native_vector_detail::load_owner(l);return l;
}
Bytes layer(const Layer& l,int ordinal,std::optional<int> compression){
    if(l.opaque)return l.opaque_record;
    Writer w,p;w.section(common(l.common,ordinal));w.section(native_vector_detail::canonical_owner(l));
    p.i32(collection_count(l.planes.size()));p.i32(l.current_plane);
    for(std::size_t i=0;i<l.planes.size();++i){const auto& pl=l.planes[i];p.u32(pl.family);p.section(plane(pl,static_cast<int>(i)+1,compression));}
    p.raw(l.plane_suffix);w.section(p.data);w.raw(l.suffix);return w.data;
}
struct PreviewSize {int width,height;};
PreviewSize fit_preview(int width,int height,int box_width,int box_height){
    if(std::int64_t(width)*box_height>=std::int64_t(height)*box_width){
        const auto fitted=std::max(1,int(std::int64_t(box_width)*height/width));
        const auto inset=(box_height-fitted)/2;
        return {box_width,box_height-2*inset};
    }
    const auto fitted=std::max(1,int(std::int64_t(box_height)*width/height));
    const auto inset=(box_width-fitted)/2;
    return {box_width-2*inset,box_height};
}
Raster thumbnail(const Raster& source,PreviewSize size){
    Raster image(size.width,size.height,32,source.resolution);
    const auto white=Pixel::rgba8(255,255,255);
    for(int y=0;y<image.height;++y){
        const auto top=std::int64_t(y)*source.height/image.height;
        const auto bottom=std::max(top+1,std::int64_t(y+1)*source.height/image.height);
        for(int x=0;x<image.width;++x){
            const auto left=std::int64_t(x)*source.width/image.width;
            const auto right=std::max(left+1,std::int64_t(x+1)*source.width/image.width);
            std::uint64_t red=0,green=0,blue=0,count=0;
            for(auto sy=top;sy<std::min<std::int64_t>(bottom,source.height);++sy)
                for(auto sx=left;sx<std::min<std::int64_t>(right,source.width);++sx){
                    const auto pixel=over(white,source.pixel(static_cast<int>(sx),static_cast<int>(sy)),65535);
                    red+=pixel.r;green+=pixel.g;blue+=pixel.b;++count;
                }
            image.set_pixel(x,y,{static_cast<std::uint16_t>(red/count),static_cast<std::uint16_t>(green/count),
                                 static_cast<std::uint16_t>(blue/count),65535});
        }
    }
    return image;
}
Bytes preview(const DocumentData& doc,Raster source,int compression,bool v5=false){
    Writer w;
    if(v5){w.u16(8);w.raw(encode_offscreen(source,compression));}
    auto small=thumbnail(source,fit_preview(source.width,source.height,128,96));
    w.u16(1);w.raw(encode_offscreen(small,compression));
    auto icon=thumbnail(small,fit_preview(source.width,source.height,64,48));
    w.u16(2);w.raw(encode_offscreen(icon,compression));
    if(!doc.dga){
        auto large=thumbnail(source,fit_preview(source.width,source.height,160,120));
        w.u16(4);w.raw(encode_offscreen(large,compression));
    }
    return w.data;
}
}
DocumentData decode_cel(std::span<const std::uint8_t> bytes){
    Reader r(bytes);auto magic=r.bytes(4);
    if(magic!=ascii("CCEL")&&magic!=ascii("CDGA"))throw FormatError(0,"Not a CCEL/CDGA document");
    DocumentData d;d.dga=magic==ascii("CDGA");d.version=r.u16();
    if((d.version>>8)>1)throw FormatError(4,"Unsupported future document version");
    d.preview=r.section().rest();d.base=base(r.section());
    auto layers=r.section();const auto count=layers.i32();d.current_layer=layers.i32();
    layers.require_count(count,8);validate_index(d.current_layer,count,layers);
    for(int i=0;i<count;++i){const auto family=layers.u32();d.layers.push_back(layer(family,layers.section()));}
    d.layer_suffix=layers.rest();d.trailing=r.rest();d.original=std::make_shared<Bytes>(bytes.begin(),bytes.end());d.original_content=std::make_shared<const DocumentData>(d);return d;
}
Bytes encode_cel(const DocumentData& source,SavePolicy policy,bool unchanged,std::optional<int> compression,bool v5_compatible){
    if(policy==SavePolicy::ExactRaw){
        if(!unchanged || !source.original)throw std::logic_error("ExactRaw requires an unchanged original document");
        return *source.original;
    }
    if(compression&&(*compression < -1||*compression > 9))throw std::invalid_argument("Invalid native compression level");
    if(source.unrestored_external_content)throw std::logic_error("External image contains an unrestored plane; use unchanged raw saving");
    auto d=source;storage_detail::optimize(d);
    Writer w,l;w.raw(ascii(d.dga?"CDGA":"CCEL"));w.u16(0x100);
    std::optional<Raster> native;bool rejected_native{};
    try{native=vector_render_detail::render_native_line_document(d,32,v5_compatible?vector_render_detail::LineDocumentOutput::QualityPreview:vector_render_detail::LineDocumentOutput::PlainPreview);}
    // Canonical raw saving retains its existing unsupported-geometry policy.
    // Rendering an image still propagates these errors; resource limits here
    // also propagate and can never silently select an old saved preview.
    catch(const FormatError&){rejected_native=true;}
    catch(const std::invalid_argument&){rejected_native=true;}
    if(v5_compatible&&!native)throw std::invalid_argument("Document cannot produce a RETAS v5 quality preview");
    std::optional<Bytes> refreshed;if(native)refreshed=preview(d,std::move(*native),compression.value_or(6),v5_compatible);
    // Request61 filtering/admission happens before the old general predicate,
    // so excluded selection content cannot suppress a valid native thumbnail.
    w.section(refreshed?*refreshed:((rejected_native||d.has_unrendered_content())?d.preview:preview(d,d.composite(),compression.value_or(6))));w.section(base(d.base));
    l.i32(collection_count(d.layers.size()));l.i32(d.current_layer);
    for(std::size_t i=0;i<d.layers.size();++i){const auto& item=d.layers[i];l.u32(item.family);l.section(layer(item,static_cast<int>(i)+1,compression));}
    l.raw(d.layer_suffix);w.section(l.data);w.raw(d.trailing);return w.data;
}
}
