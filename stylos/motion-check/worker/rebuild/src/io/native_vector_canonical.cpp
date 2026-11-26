#include "native_vector_canonical.hpp"
#include "retas/vector_extension_archive.hpp"
#include <algorithm>
#include <map>

namespace retas::native_vector_detail {
namespace {
bool line(unsigned f){return f==0x22100000u||f==0x24400000u||f==0x20000200u;}
bool paint(unsigned f){return f==0x21200000u||f==0x20800000u;}
bool supported(const Layer& l){return !l.opaque&&l.specific.size()<=64ull*1024*1024&&(line(l.family)||paint(l.family)||l.family==0x40004000u||l.family==0x40002000u);}
int pointer(int id){return id<0?-1:id;}
void pointers(std::vector<int>& ids){for(auto& id:ids)id=pointer(id);}
template<class T> int maximum(const std::vector<VectorPoolRecord<T>>& pool){int high=-1;for(const auto& r:pool)high=std::max(high,r.index);return high;}
template<class T> void ordered(std::vector<VectorPoolRecord<T>>& pool){std::ranges::sort(pool,{},&VectorPoolRecord<T>::index);}
template<class T> void live_high(int& high,const std::vector<VectorPoolRecord<T>>& pool,std::optional<int> hint){if(hint){if(*hint<maximum(pool)||*hint<-1)throw std::invalid_argument("Canonical vector live high-water is below an occupied identity");high=*hint;}}
void loaded(VectorLineArchive& a){
    a.high_water={maximum(a.vertices),maximum(a.scales),maximum(a.subcurves),maximum(a.models),maximum(a.crosspoints),maximum(a.lines)};
    std::map<int,VectorLineScale*> scales;for(auto& r:a.scales)scales.emplace(r.index,&r.value);
    //5A6C70 writes first0 then last1; a singleton therefore ends at1.
    for(const auto& r:a.subcurves)if(!r.value.scales.empty()){scales.at(r.value.scales.front())->parameter_bits=0;scales.at(r.value.scales.back())->parameter_bits=0x3f800000u;}
}
void loaded(VectorPaintArchive& a){loaded(a.base);a.face_high_water={maximum(a.loops),maximum(a.regions),maximum(a.faces),maximum(a.gradnodes),maximum(a.gradations),maximum(a.textures)};load_position(a.border.common);}
void written(VectorLineArchive& a,const Layer& owner){
    live_high(a.high_water[0],a.vertices,owner.runtime_vector_high_water[0]);live_high(a.high_water[1],a.scales,owner.runtime_vector_high_water[1]);live_high(a.high_water[2],a.subcurves,owner.runtime_vector_high_water[2]);live_high(a.high_water[3],a.models,owner.runtime_vector_high_water[3]);live_high(a.high_water[4],a.crosspoints,owner.runtime_vector_high_water[4]);live_high(a.high_water[5],a.lines,owner.runtime_vector_high_water[5]);
    for(auto& r:a.vertices)pointers(r.value.subcurves);
    for(auto& r:a.subcurves){auto& v=r.value;v.vertex_a=pointer(v.vertex_a);v.vertex_b=pointer(v.vertex_b);v.model=pointer(v.model);}
    for(auto& r:a.models){auto& v=r.value;v.flags&=0xffffu;v.line=pointer(v.line);for(unsigned side=0;side<2;++side){v.crosspoints[side]=pointer(v.crosspoints[side]);if(v.targets[side]<0){v.targets[side]=-1;v.target_kinds[side]=0;}else v.target_kinds[side]=v.target_kinds[side]!=0;}}
    ordered(a.vertices);ordered(a.scales);ordered(a.subcurves);ordered(a.models);ordered(a.crosspoints);ordered(a.lines);
}
void written(VectorPaintArchive& a,const Layer& owner){
    written(a.base,owner);live_high(a.face_high_water[0],a.loops,owner.runtime_vector_high_water[6]);live_high(a.face_high_water[1],a.regions,owner.runtime_vector_high_water[7]);live_high(a.face_high_water[2],a.faces,owner.runtime_vector_high_water[8]);live_high(a.face_high_water[3],a.gradnodes,owner.runtime_vector_high_water[9]);live_high(a.face_high_water[4],a.gradations,owner.runtime_vector_high_water[10]);live_high(a.face_high_water[5],a.textures,owner.runtime_vector_high_water[11]);
    for(auto& r:a.loops)r.value.region=pointer(r.value.region);
    for(auto& r:a.regions){r.value.outer.region=pointer(r.value.outer.region);pointers(r.value.faces);}
    for(auto& r:a.faces){auto& f=r.value;f.region=pointer(f.region);f.flags&=0xffffu;f.material=pointer(f.material);}
    //Opaque inactive leaves are a transport boundary. A valid known leaf is
    //normalized in full; an invalid leaf retains all original bytes.
    for(auto& r:a.gradations)try{auto v=decode_vector_gradation(r.value);v.flags&=0xffffu;pointers(v.faces);r.value=encode_vector_gradation(v);}catch(const FormatError&){}
    for(auto& r:a.textures)try{auto v=decode_vector_texture(r.value);v.flags&=0xffffu;pointers(v.faces);r.value=encode_vector_texture(v);}catch(const FormatError&){}
    ordered(a.loops);ordered(a.regions);ordered(a.faces);ordered(a.gradnodes);ordered(a.gradations);ordered(a.textures);
}
Bytes convert(const Layer& l,bool loading){
    if(line(l.family)){auto a=decode_vector_line_archive(l);if(loading)loaded(a);else written(a,l);return encode_vector_line_archive(a);}
    if(paint(l.family)){auto a=decode_vector_paint_archive(l);if(loading)loaded(a);else written(a,l);return encode_vector_paint_archive(a);}
    if(l.family==0x40004000u){auto a=decode_vector_ruler_archive(l);if(loading){loaded(a.base);a.high_water=maximum(a.rulers);}else{written(a.base,l);live_high(a.high_water,a.rulers,l.runtime_vector_high_water[6]);for(auto& r:a.rulers){r.value.line=pointer(r.value.line);r.value.flags&=0x00f0ffffu;}ordered(a.rulers);}return encode_vector_ruler_archive(a);}
    auto a=decode_vector_shape_archive(l);if(loading){loaded(a.paint);a.high_water=maximum(a.shapes);}else{written(a.paint,l);for(auto& r:a.shapes){r.value.line=pointer(r.value.line);r.value.region=pointer(r.value.region);r.value.flags&=0xffffu;}ordered(a.shapes);}return encode_vector_shape_archive(a);
}
}
void load_position(CommonElement& c){for(auto& p:c.position){if(p.unit==-2)p.unit=-1;if(!(p.reference>0))p.reference=0;}}
void load_owner(Layer& l){if(!supported(l))return;try{auto bytes=convert(l,true);l.specific=std::move(bytes);}catch(const FormatError&){/*Preserve an unsupported typed archive as an opaque transport payload.*/}}
Bytes canonical_owner(const Layer& l){if(!supported(l))return l.specific;try{return convert(l,false);}catch(const FormatError&){return l.specific;}}
bool same_allocator_output(const Layer& a,const Layer& b) noexcept {
    if(a.runtime_vector_high_water==b.runtime_vector_high_water||!supported(a))return true;
    try{
        Reader outer(a.specific);auto container=(paint(a.family)||a.family==0x40002000u)?outer.section():outer;
        auto base=container.section();std::array<int,12> high{};unsigned count=6;for(unsigned i=0;i<6;++i)high[i]=base.i32();
        if(paint(a.family)||a.family==0x40002000u){auto face=container.section();for(unsigned i=6;i<12;++i)high[i]=face.i32();count=12;}
        else if(a.family==0x40004000u){auto extension=container.section();high[6]=extension.i32();count=7;}
        for(unsigned i=0;i<count;++i)if(a.runtime_vector_high_water[i].value_or(high[i])!=b.runtime_vector_high_water[i].value_or(high[i]))return false;
        return true;
    }catch(...){return true;} //Malformed framing is transported without typed normalization.
}
}
