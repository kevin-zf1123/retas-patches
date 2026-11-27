#include "retas/vector_extension_archive.hpp"
#include "vector_archive_budget.hpp"
#include "vector_face_projection.hpp"
#include <set>

namespace retas {
namespace {
constexpr std::size_t byte_limit=64U*1024U*1024U,count_limit=100000;
[[noreturn]] void bad(const char* s){throw FormatError(0,s);}
void owner(const Layer& l,std::uint32_t family){
    if(l.opaque||l.family!=family)throw std::invalid_argument("Unsupported vector extension owner");
    if(l.specific.size()>byte_limit)throw std::length_error("Vector extension archive exceeds64MiB");
}
int marker(Reader& r){const int m=r.i32();if(m<-1)bad("Invalid vector extension high-water marker");return m;}
template<class T,class Decode> auto pool(Reader r,int high,Decode decode){
    std::vector<VectorPoolRecord<T>> out;std::set<int> seen;
    while(r.remaining()){
        if(out.size()>=count_limit)bad("Vector extension record limit exceeded");
        const int id=r.i32();auto leaf=r.section();
        if(id<0||id>high||!seen.insert(id).second)bad("Invalid vector extension pool identity");
        auto value=decode(leaf);out.push_back({id,value,leaf.rest()});
    }
    return out;
}
template<class T> std::set<int> identities(const std::vector<VectorPoolRecord<T>>& records){
    std::set<int> out;for(const auto& r:records)out.insert(r.index);return out;
}
void nullable(const std::set<int>& live,int id){if(id>=0&&!live.contains(id))bad("Dead vector extension nullable target");}
void validate(const VectorRulerArchive& a){const auto lines=identities(a.base.lines);for(const auto& r:a.rulers)nullable(lines,r.value.line);}
void validate(const VectorShapeArchive& a){const auto lines=identities(a.paint.base.lines),regions=identities(a.paint.regions);for(const auto& r:a.shapes){nullable(lines,r.value.line);nullable(regions,r.value.region);}}
template<class T> std::size_t extra_size(int high,const std::vector<VectorPoolRecord<T>>& records,std::size_t fixed,const Bytes& header,const Bytes& body){
    if(high<-1||records.size()>count_limit)bad("Invalid vector extension pool size/marker");
    std::size_t total=16;const auto add=[&](std::size_t n){if(n>byte_limit-total)throw std::length_error("Vector extension archive exceeds64MiB");total+=n;};
    add(header.size());add(body.size());std::set<int> seen;
    for(const auto& r:records){if(r.index<0||r.index>high||!seen.insert(r.index).second)bad("Invalid vector extension pool identity");add(8+fixed);add(r.suffix.size());}
    return total;
}
void combined_size(std::size_t base,std::size_t extra){if(extra>byte_limit-base)throw std::length_error("Vector extension archive exceeds64MiB");}
template<class T,class Encode> Bytes body(const std::vector<VectorPoolRecord<T>>& records,const Bytes& suffix,Encode encode){
    Writer out,p;for(const auto& r:records){Writer leaf;encode(leaf,r.value);leaf.raw(r.suffix);p.i32(r.index);p.section(leaf.data);}out.section(p.data);out.raw(suffix);return std::move(out.data);
}
Bytes header(int high,const Bytes& suffix){Writer w;w.i32(high);w.raw(suffix);return std::move(w.data);}
std::vector<int> refs(Reader& r){const auto n=r.i32();r.require_count(n,4,count_limit);std::vector<int> out;out.reserve(std::size_t(n));for(int i=0;i<n;++i)out.push_back(r.i32());return out;}
}
VectorRulerArchive decode_vector_ruler_archive(const Layer& layer){
    owner(layer,0x40004000);Reader outer(layer.specific);auto bh=outer.section(),eh=outer.section(),bb=outer.section(),eb=outer.section();
    VectorRulerArchive a;a.high_water=marker(eh);a.header_suffix=eh.rest();
    a.rulers=pool<VectorArchiveRuler>(eb.section(),a.high_water,[](Reader& r){VectorArchiveRuler v;v.line=r.i32();v.flags=r.u32();v.x=r.i32();v.y=r.i32();v.length_20=r.i32();v.length_24=r.i32();v.angle_bits=r.u32();v.field_2c=r.i32();v.length_30=r.i32();v.length_34=r.i32();return v;});a.body_suffix=eb.rest();
    Layer base;base.family=0x20000200;Writer normalized;normalized.section(bh.take(bh.remaining()));normalized.section(bb.take(bb.remaining()));normalized.raw(outer.take(outer.remaining()));base.specific=std::move(normalized.data);
    a.base=decode_vector_line_archive(base);validate(a);return a;
}
Bytes encode_vector_ruler_archive(const VectorRulerArchive& a){
    const auto extra=extra_size(a.high_water,a.rulers,40,a.header_suffix,a.body_suffix);combined_size(vector_archive_detail::line_wire_size(a.base),extra);validate(a);
    const auto base=encode_vector_line_archive(a.base);Reader r(base);auto bh=r.section(),bb=r.section();Writer out;
    out.section(bh.take(bh.remaining()));out.section(header(a.high_water,a.header_suffix));out.section(bb.take(bb.remaining()));
    out.section(body(a.rulers,a.body_suffix,[](Writer& w,const VectorArchiveRuler& v){w.i32(v.line);w.u32(v.flags);w.i32(v.x);w.i32(v.y);w.i32(v.length_20);w.i32(v.length_24);w.u32(v.angle_bits);w.i32(v.field_2c);w.i32(v.length_30);w.i32(v.length_34);}));out.raw(r.take(r.remaining()));return std::move(out.data);
}
VectorShapeArchive decode_vector_shape_archive(const Layer& layer){
    owner(layer,0x40002000);Reader outer(layer.specific);auto core=outer.section();auto bh=core.section(),fh=core.section(),eh=core.section(),bb=core.section(),fb=core.section(),eb=core.section();
    VectorShapeArchive a;a.high_water=marker(eh);a.header_suffix=eh.rest();
    a.shapes=pool<VectorArchiveShape>(eb.section(),a.high_water,[](Reader& r){VectorArchiveShape v;v.line=r.i32();v.region=r.i32();v.flags=r.u32();v.x=r.i32();v.y=r.i32();v.length_24=r.i32();v.length_28=r.i32();v.angle_bits=r.u32();v.field_30=r.i32();return v;});a.body_suffix=eb.rest();
    Writer normalized_core;for(auto* s:{&bh,&fh,&bb,&fb})normalized_core.section(s->take(s->remaining()));normalized_core.raw(core.take(core.remaining()));
    Writer normalized;normalized.section(normalized_core.data);normalized.raw(outer.take(outer.remaining()));Layer paint;paint.family=0x21200000;paint.specific=std::move(normalized.data);paint.runtime_vector_face_colors=layer.runtime_vector_face_colors;
    a.paint=decode_vector_paint_archive(paint);validate(a);return a;
}
Bytes encode_vector_shape_archive(const VectorShapeArchive& a){
    const auto extra=extra_size(a.high_water,a.shapes,36,a.header_suffix,a.body_suffix);combined_size(vector_archive_detail::paint_wire_size(a.paint),extra);validate(a);
    const auto paint=encode_vector_paint_archive(a.paint);Reader outer(paint);auto core=outer.section(),bh=core.section(),fh=core.section(),bb=core.section(),fb=core.section();Writer c,out;
    c.section(bh.take(bh.remaining()));c.section(fh.take(fh.remaining()));c.section(header(a.high_water,a.header_suffix));c.section(bb.take(bb.remaining()));c.section(fb.take(fb.remaining()));
    c.section(body(a.shapes,a.body_suffix,[](Writer& w,const VectorArchiveShape& v){w.i32(v.line);w.i32(v.region);w.u32(v.flags);w.i32(v.x);w.i32(v.y);w.i32(v.length_24);w.i32(v.length_28);w.u32(v.angle_bits);w.i32(v.field_30);}));c.raw(core.take(core.remaining()));out.section(c.data);out.raw(outer.take(outer.remaining()));return std::move(out.data);
}
void store_vector_shape_archive(Layer& layer,const VectorShapeArchive& archive){
    vector_archive_detail::publish_paint_storage(layer,archive.paint,encode_vector_shape_archive(archive));
}
VectorExtensionPlaneReferences vector_extension_plane_references(const Plane& p){
    if(p.opaque||(p.family!=0x40000001&&p.family!=0x40000002))throw std::invalid_argument("Unsupported vector extension plane");
    if(p.specific.size()>byte_limit)throw std::length_error("Vector extension plane exceeds64MiB");
    VectorExtensionPlaneReferences v;Reader r(p.specific);auto bundle=r.section(),topology=bundle.section();v.vertices=refs(topology);v.models=refs(topology);
    if(p.family==0x40000001){auto l=bundle.section();v.lines=refs(l);}
    else{v.regions=refs(bundle);auto l=r.section();v.lines=refs(l);auto paint=r.section();v.faces=refs(paint);v.gradations=refs(paint);v.textures=refs(paint);}
    v.extensions=refs(r);return v;
}
}
