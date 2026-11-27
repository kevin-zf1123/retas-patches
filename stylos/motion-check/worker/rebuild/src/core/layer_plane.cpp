#include "layer_plane_internal.hpp"
#include "retas/vector_geometry.hpp"
#include "retas/vector_face_archive.hpp"
#include "retas/legacy_text.hpp"
#include "retas/layer_move.hpp"
#include "retas/resolution_raster.hpp"
#include "selection_clipboard_target.hpp"
#include "retas/text_font_backend.hpp"
#include "text_font_backend_internal.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
#include <cctype>

namespace retas {
namespace {
constexpr ElementFamily families[]{
    {0x11000800,"2值彩色图层"},{0x11001000,"色阶彩色图层"},{0x21200000,"矢量彩色图层"},
    {0x18004000,"光栅指示图层"},{0x40001000,"画框图层"},{0x40020000,"消失点图层"},
    {0x40000200,"调整图层"},{0x10008000,"选区图层"},{0x10000100,"光栅普通图层"}};
constexpr ElementFamily planes[]{
    {0x10000001,"光栅平面"},{0x11110002,"2值主线平面"},{0x11110001,"色阶主线平面"},{0x12100001,"彩色平面"},
    {0x11210001,"2值线条平面"},{0x11220001,"色阶线条平面"},{0x11240001,"细节线条平面"},
    {0x21110001,"矢量主线平面"},{0x21120001,"矢量色阶主线平面"},{0x21210001,"矢量线条平面"},
    {0x21220001,"矢量色阶线条平面"},{0x22100001,"矢量彩色平面"},{0x22100002,"矢量平面"},
    {0x40001008,"原点画框"},{0x40002008,"摄影画框"},{0x40000080,"消失点"},{0x40000010,"调整平面"}};
bool contains(const std::vector<std::uint32_t>& values,std::uint32_t id){return std::ranges::find(values,id)!=values.end();}
int checked(std::int64_t n){if(n<INT32_MIN||n>INT32_MAX)throw std::length_error("Layer geometry exceeds signed32");return int(n);}
int scaled(double n){if(!std::isfinite(n)||n<INT32_MIN||n>INT32_MAX)throw std::length_error("Layer scaled geometry exceeds signed32");return int(std::trunc(n));}
Layer& current(DocumentData& d){if(d.unrestored_external_content||d.current_layer<0||d.current_layer>=int(d.layers.size()))throw std::invalid_argument("No editable layer");auto& l=d.layers[d.current_layer];if(l.opaque)throw std::invalid_argument("Opaque layer");return l;}
void select_plane(Layer& l,std::size_t index){l.current_plane=int(index);for(std::size_t i=0;i<l.planes.size();++i)l.planes[i].common.flags[2]=i==index;}
void select_layer(DocumentData& d,std::size_t index){d.current_layer=int(index);for(std::size_t i=0;i<d.layers.size();++i)d.layers[i].common.flags[2]=i==index;}
void set_raster(Plane& p,Raster r){p.common.width=r.width;p.common.height=r.height;p.common.depth=r.depth;p.common.resolution=r.resolution;p.raster=std::move(r);p.pixels_changed=true;p.specific.clear();}
void validate_name(const Bytes& name){if(name.size()>255)throw std::invalid_argument("Element name exceeds 255 bytes");}
}
std::span<const ElementFamily> new_layer_families(){return families;}
std::string_view layer_family_name(std::uint32_t f){for(const auto& e:families)if(e.id==f)return e.name;return "图层";}
std::string_view plane_family_name(std::uint32_t f){for(const auto& e:planes)if(e.id==f)return e.name;return "平面";}
std::vector<std::uint32_t> new_plane_families(const Layer& l){
    if(l.opaque)return {};
    switch(l.family){
    case 0x11000800:case 0x11001000:case 0x18004000:return {0x10000001};
    case 0x12000200:case 0x14002000:return {0x11210001,0x11220001};
    case 0x22100000:case 0x24400000:return {0x21210001,0x21220001,0x11240001};
    case 0x21200000:return {0x21110001,0x21120001,0x22100001,0x10000001};
    case 0x40001000:return {0x40002008};
    case 0x40000200:return {0x40000010};
    default:return {};
    }
}
namespace {
std::vector<std::uint32_t> layer_choices(const DocumentData& d,bool cel,bool dga){
    std::vector<std::uint32_t> out;bool paint=false,frame=false,draw=false;
    for(const auto& l:d.layers){paint|=l.family==0x11000800||l.family==0x11001000||l.family==0x21200000;frame|=l.family==0x40001000;draw|=l.family==0x12000200||l.family==0x22100000;}
    for(const auto& f:families){if((paint||dga||(!cel&&draw))&&(f.id==0x11000800||f.id==0x11001000||f.id==0x21200000))continue;if(frame&&f.id==0x40001000)continue;out.push_back(f.id);}return out;
}
}
std::vector<std::uint32_t> new_layer_families(const DocumentData& d){return layer_choices(d,false,false);}
std::vector<std::uint32_t> new_layer_families(const Document& document){
    auto extension=document.path.extension().string();
    std::transform(extension.begin(),extension.end(),extension.begin(),[](unsigned char c){return static_cast<char>(std::tolower(c));});
    //436250: CEL mode excludes Draw catalog entries; Draw inventory only
    //suppresses Paint candidates for unnamed and external-image documents.
    return layer_choices(document.data,extension==".cel",extension==".dga");
}
bool reset_plane_alpha_editing(DocumentData& data) noexcept{
    bool changed=false;for(auto& layer:data.layers)for(auto& plane:layer.planes)if(plane.common.opacity_mode==4){plane.common.opacity_mode=1;changed=true;}return changed;
}
std::vector<std::uint32_t> convert_plane_families(const Layer& l){
    if(l.family!=0x11000800&&l.family!=0x11001000&&l.family!=0x12000200&&l.family!=0x14002000&&l.family!=0x21200000&&l.family!=0x22100000&&l.family!=0x24400000)return {};
    auto out=new_plane_families(l);std::vector<std::uint32_t> source;
    bool line=false,other=false,color=false,raster=false;
    for(const auto& p:l.planes)if(p.common.flags[2]&&p.common.flags[1]&&p.common.flags[0]!=1){source.push_back(p.family);raster|=(p.family&0x10000000u)!=0;if(p.family&0x10000u)line=true;else if(p.family&0x02000000u)color=true;else if(p.family&0x20000u)other=true;}
    if(source.empty())return {};
    if(l.family==0x11000800){out.push_back(0x11110002);out.push_back(0x12100001);}
    if(l.family==0x11001000){out.push_back(0x11110001);out.push_back(0x12100001);}
    std::erase_if(out,[&](auto f){if(source.size()==1&&source[0]==f)return true;if(!(l.family&0x01000000u))return false;
        if(f==0x11110001||f==0x11110002)return !line||color;
        if(f==0x12100001)return line||!color;
        if(f==0x21110001||f==0x21120001)return color||raster;
        if(f==0x22100001)return line||other||raster;return false;});return out;
}
std::vector<std::uint32_t> convert_layer_families(const Layer& l){
    if(l.opaque||l.family==0x10008000||l.family==0x20000100||l.family==0x40000200||l.family==0x40001000||l.family==0x40002000||l.family==0x40004000||l.family==0x40020000)return {};
    std::vector<std::uint32_t> out{0x10000100,0x11000800,0x11001000,0x18004000};
    std::erase(out,l.family);
    if(l.family==0x12000200||l.family==0x14002000||l.family==0x22100000||l.family==0x24400000){std::erase(out,0x11000800u);std::erase(out,0x11001000u);}return out;
}
std::vector<std::uint32_t> convert_layer_families(const DocumentData& data){
    std::vector<const Layer*> sources;bool draw=false,unselected_paint=false;
    for(const auto& l:data.layers){const bool selected=l.common.flags[2]&&l.common.flags[1]&&l.common.flags[0]!=1&&!convert_layer_families(l).empty();if(selected)sources.push_back(&l);
        draw|=l.family==0x12000200||l.family==0x14002000||l.family==0x22100000||l.family==0x24400000;
        unselected_paint|=!selected&&(l.family==0x11000800||l.family==0x11001000||l.family==0x21200000);}
    if(sources.empty())return {};
    std::vector<std::uint32_t> out{0x10000100,0x11000800,0x11001000,0x18004000};
    if(sources.size()==1)std::erase(out,sources.front()->family);
    if(draw||unselected_paint){std::erase(out,0x11000800u);std::erase(out,0x11001000u);}return out;
}
bool plane_conversion_keep_source(const Layer& l,std::uint32_t family,bool requested){
    std::vector<std::uint32_t> source;for(const auto& p:l.planes)if(p.common.flags[2]&&p.common.flags[1]&&p.common.flags[0]!=1)source.push_back(p.family);
    if(l.family&0x20000000u)return (family&0x20000000u)?false:requested;
    if(l.family&0x01000000u){if(source.size()==1&&(source[0]&0x03000000u))return true;
        if(source.size()>1){if(family&0x03000000u)return false;if(std::ranges::any_of(source,[](auto f){return (f&0x03000000u)!=0;}))return true;}}
    return requested;
}
bool layer_conversion_keep_source(const DocumentData& data,std::uint32_t family,bool requested){
    if(family&0x01000000u)for(const auto& l:data.layers)if(l.common.flags[2]&&l.common.flags[1]&&l.common.flags[0]!=1&&(l.family&0x01000000u))return false;
    return requested;
}
Plane make_plane(const Layer& owner,std::uint32_t family,Bytes name,std::uint64_t color,const AdjustParameters& adjust){
    validate_name(name);Plane p;p.family=family;p.common=owner.common;p.common.runtime_identity=next_element_identity();
    p.common.x=p.common.y=0;p.common.position={Length{0,-1,0},Length{0,-1,0}};
    p.common.name=name.empty()?encode_cp936(plane_family_name(family)):std::move(name);p.common.flags={0,1,0,0,0};p.common.color=color;p.common.opacity=100;p.common.opacity_mode=1;p.common.suffix.clear();
    if(family&0x10000000u){
        int depth=owner.common.depth,scale=1;
        if(family==0x11210001||family==0x11220001||family==0x11240001){depth=1;scale=8;}
        p.common.flags[4]=(family&0x00200000u)!=0;
        p.common.opacity_mode=stack_detail::plane_default_mode(family);
        set_raster(p,Raster(checked(std::int64_t(owner.common.width)*scale),checked(std::int64_t(owner.common.height)*scale),depth,owner.common.resolution*scale));
    }else if(family&0x20000000u){
        p.common.width=p.common.height=p.common.depth=0;p.common.resolution=owner.common.resolution*16;
        p.common.flags[4]=(family&0x00200000u)!=0;
        Writer w;
        if((family&0x01000000u)==0){w.i32(0);w.i32(0);w.i32(0);}
        else if(family&0x00200000u){Writer topology,list;topology.i32(0);topology.i32(0);list.i32(0);w.section(topology.data);w.section(list.data);}
        else w.i32(0);
        p.specific=std::move(w.data);
    }else{
        p.common.width=p.common.height=p.common.depth=0;
        if(family==0x40000001||family==0x40000002){
            //639A30/639AF0 start with empty topology and command lists.
            p.common.resolution=owner.common.resolution*16;
            Writer zero,topology,bundle,wire;zero.i32(0);topology.i32(0);topology.i32(0);bundle.section(topology.data);
            if(family==0x40000001)bundle.section(zero.data);else bundle.i32(0);
            wire.section(bundle.data);
            if(family==0x40000002){wire.section(zero.data);Writer paint;paint.i32(0);paint.i32(0);paint.i32(0);wire.section(paint.data);}
            wire.i32(0);p.specific=std::move(wire.data);
        }
        else if(family==0x40000010)p.specific=encode_adjust_parameters(adjust);
        else if(family==0x40001008||family==0x40002008)p.specific=encode_frame_parameters({});
        else if(family==0x40000080)p.specific=encode_vanishing_point_parameters({});
        else throw std::invalid_argument("Unsupported plane factory family");
    }
    return p;
}
Layer make_layer(const BaseInfo& base,const LayerCreateOptions& options){
    if(std::ranges::find(families,options.family,&ElementFamily::id)==std::end(families))throw std::invalid_argument("Layer family is not a New Layer choice");
    if(options.depth!=32&&options.depth!=64)throw std::invalid_argument("New layer depth must be 32 or 64");validate_name(options.name);
    Layer l;l.family=options.family;l.common.width=base.width();l.common.height=base.height();l.common.depth=options.depth;l.common.resolution=base.dpi;
    l.common.name=options.name.empty()?encode_cp936(layer_family_name(l.family)):options.name;l.common.color=options.color;
    l.common.position={Length{0,0,base.dpi},Length{0,0,base.dpi}};
    std::vector<std::uint32_t> initial;
    switch(l.family){
    case 0x11000800:initial={0x12100001,0x11110002};break;
    case 0x11001000:initial={0x12100001,0x11110001};break;
    case 0x21200000:{initial={0x22100001,0x21110001};VectorPaintArchive a;a.base.high_water.fill(-1);a.face_high_water.fill(-1);a.border.common=l.common;a.border.common.runtime_identity=next_element_identity();a.border.common.name=ascii("BorderPlane");store_vector_paint_archive(l,a);break;}
    case 0x40001000:initial={0x40001008};break;
    case 0x40020000:initial={0x40000080};break;
    case 0x40000200:{initial={0x40000010};Writer w;w.i32(0);l.specific=std::move(w.data);break;}
    default:initial={0x10000001};break;
    }
    if(l.family==0x10008000){l.common.depth=1;l.common.opacity=50;}
    for(auto f:initial)l.planes.push_back(make_plane(l,f,{},options.color,options.adjust));if(l.family==0x10008000){l.planes[0].common.color=0xffff00007fff0000ull;l.planes[0].common.flags[4]=1;}select_plane(l,0);return l;
}
bool can_set_current_plane(const DocumentData& d) noexcept{
    const auto* p=d.active_plane();if(!p||d.layers[d.current_layer].opaque||p->opaque)return false;
    if(p->family==0x40000010)return editable_adjust_plane(*p);
    return p->family==0x40000004||p->family==0x40001008||p->family==0x40002008||p->family==0x40000080;
}
namespace {
Raster plane_image(const DocumentData& data,const Layer& layer,std::size_t index,int depth){
    const auto& p=layer.planes.at(index);
    if(p.opaque)throw std::invalid_argument("Opaque plane conversion source");
    if(p.family==0x40000004&&p.runtime_text_cache&&p.runtime_text_cache->static_geometry)
        throw std::invalid_argument("Native-layout Text materialization requires its independent mask consumer");
    if(const auto* glyph=text_raster_cache(layer,p,data.base.dpi)){
        if(p.runtime_text_cache->static_geometry||p.runtime_text_cache->origin!=std::array<int,2>{})
            throw std::invalid_argument("Native-layout Text materialization requires its independent mask consumer");
        auto image=stack_detail::convert_raster(*glyph,depth);
        if(image.resolution!=layer.common.resolution)image=resolution_raster(image,{0,0,image.width,image.height},scaled(image.width*layer.common.resolution/image.resolution),scaled(image.height*layer.common.resolution/image.resolution),layer.common.resolution);
        return image;
    }
    if(p.raster&&p.raster->resolution==layer.common.resolution&&p.raster->width==layer.common.width&&p.raster->height==layer.common.height)return stack_detail::convert_raster(*p.raster,depth);
    const int render_depth=depth==64?64:32;auto single=make_document(layer.common.width,layer.common.height,render_depth,layer.common.resolution);single.layers={layer};single.layers[0].common.x=single.layers[0].common.y=0;single.layers[0].common.flags[1]=1;
    for(std::size_t i=0;i<single.layers[0].planes.size();++i)single.layers[0].planes[i].common.flags[1]=i==index;
    if(single.has_unrendered_content())throw std::invalid_argument("Plane conversion requires a complete source renderer");
    (void)data;return stack_detail::convert_raster(single.composite(render_depth),depth);
}
struct TextConversion {
    bool enabled{};
    std::optional<TextFontBackend> fonts;
    resolution_detail::Budget budget;
    std::uint64_t padded_pixels{};
    explicit TextConversion(ElementConvertTextFonts value){
        if(value!=ElementConvertTextFonts::None&&value!=ElementConvertTextFonts::Qt)throw std::invalid_argument("Invalid conversion Text font provider");
        enabled=value==ElementConvertTextFonts::Qt;
    }
    void apply(Layer& target,Plane& plane,const Layer& from,const Plane& source){
        if(from.opaque||(from.family!=0x40000800&&from.family!=0x48040800)||!plane.raster)
            throw std::invalid_argument("Text conversion requires an ordinary Text/Indication owner and raster target");
        if(!fonts)fonts.emplace();
        const auto masks=text_font_detail::export_masks(*fonts,from,source,plane.raster->resolution,budget,padded_pixels);
        //63C210 writes directly to the target at absolute mask coordinates,
        //then invokes its owner's position setter. It does not relocate pixels.
        text_font_detail::paint_conversion_masks(*plane.raster,masks,budget);
        target.common.x=target.common.y=0;
    }
};
void materialize_plane(const DocumentData& data,Layer& target,Plane& plane,const Layer& from,std::size_t index,TextConversion& text){
    const auto& source=from.planes.at(index);
    if(text.enabled&&source.family==0x40000004){text.apply(target,plane,from,source);return;}
    if(source.raster){stack_detail::transfer_raster(plane,target.common,source,from.common,true);return;}
    if(source.family&0x20000000u){
        //The native +60 materializer feeds one child through5ED450/5EB050.
        //Normal display admission (opacity and sibling caches) is unrelated.
        auto vector=from;std::size_t child{};
        for(std::size_t i=0;i<index;++i)if(!from.planes[i].raster)++child;
        std::erase_if(vector.planes,[](const Plane& p){return p.raster.has_value();});
        std::uint64_t bytes=512ull*1024*1024,work=128ull*1024*1024;
        auto image=selection_detail::clipboard_vector_raster(vector,child,target,plane,{},bytes,work,true);
        if(plane.raster->depth==1&&(plane.family&0x00f00000u)==0x00200000u){plane.raster=std::move(image);return;}
        stack_detail::merge_raster(*plane.raster,image,0,0,1,1,true);return;
    }
    auto rendered=source;rendered.raster=plane_image(data,from,index,plane.raster->depth);rendered.common.resolution=rendered.raster->resolution;
    stack_detail::transfer_raster(plane,target.common,rendered,from.common,true);
}
}
bool convert_planes(DocumentData& data,const ElementConvertOptions& options){
    TextConversion text(options.text_fonts);
    auto candidate=data;auto& l=current(candidate);if(!contains(convert_plane_families(l),options.family))throw std::invalid_argument("Plane conversion family is not admitted");
    std::vector<std::size_t> indices;for(std::size_t i=0;i<l.planes.size();++i){const auto& p=l.planes[i];if(p.common.flags[2]&&p.common.flags[1]&&p.common.flags[0]!=1)indices.push_back(i);}if(indices.empty())return false;
    const auto original=l.planes[indices.front()];const bool keep=plane_conversion_keep_source(l,options.family,options.keep_source);
    auto p=make_plane(l,options.family,options.name.empty()?original.common.name:options.name,original.common.color);
    if(p.raster){
        //63B280 receives owner depth for raster replacements; family constructors
        //may select their dedicated binary/gray line storage.
        for(auto index:indices)materialize_plane(data,l,p,l,index,text);
        p.pixels_changed=true;
    }else{
        // Same-owner vector conversion transfers lists through the same worker as MergeSame.
        const auto target=l.planes.size();l.planes.push_back(p);
        for(auto index:indices){if(!(l.planes[index].family&0x20000000u)||((l.planes[index].family^options.family)&0x01000000u))throw std::invalid_argument("Vector conversion payload categories differ");stack_detail::merge_vector_planes(l,target,index);}
        p=std::move(l.planes.back());l.planes.pop_back();
    }
    const auto insertion=l.planes.size();l.planes.insert(l.planes.begin()+insertion,std::move(p));std::size_t selected=insertion;
    if(!keep)for(auto i=indices.rbegin();i!=indices.rend();++i){const auto slot=*i>=insertion?*i+1:*i;l.planes.erase(l.planes.begin()+slot);if(slot<selected)--selected;}
    select_plane(l,selected);data=std::move(candidate);return true;
}
bool convert_layer(DocumentData& data,const ElementConvertOptions& options,bool batch){
    TextConversion text(options.text_fonts);
    if(!batch&&!contains(convert_layer_families(data),options.family))throw std::invalid_argument("Layer conversion target is not available");
    if(data.unrestored_external_content)throw std::invalid_argument("Unrestored external layer content");
    const bool keep=batch?options.keep_source:layer_conversion_keep_source(data,options.family,options.keep_source);auto candidate=data;std::vector<std::size_t> indices;
    for(std::size_t i=0;i<candidate.layers.size();++i){const auto& l=candidate.layers[i];if(l.common.flags[2]&&l.common.flags[1]&&(batch?l.common.flags[0]==0:l.common.flags[0]!=1)&&!convert_layer_families(l).empty())indices.push_back(i);}if(indices.empty())return false;
    if(options.family!=0x10000100&&options.family!=0x11000800&&options.family!=0x11001000&&options.family!=0x18004000)throw std::invalid_argument("Layer conversion target is not admitted");
    for(auto index:indices){const auto& l=candidate.layers[index];if((options.family==0x11000800||options.family==0x11001000)&&(l.family==0x12000200||l.family==0x14002000||l.family==0x22100000||l.family==0x24400000))throw std::invalid_argument("Draw layers cannot convert to Paint families");}
    const auto selected=std::ranges::find(indices,std::size_t(candidate.current_layer))!=indices.end()?std::size_t(candidate.current_layer):indices.back();const auto source=candidate.layers[selected];
    LayerCreateOptions settings;settings.family=options.family;settings.depth=options.family==0x10000100?options.depth:32;settings.name=options.name;
    if(settings.name.empty()){settings.name=source.common.name;const auto suffix=encode_cp936("的转换");settings.name.insert(settings.name.end(),suffix.begin(),suffix.end());}
    auto base=data.base;base.lengths[0]={double(source.common.width),0,source.common.resolution};base.lengths[1]={double(source.common.height),0,source.common.resolution};base.dpi=source.common.resolution;
    const bool native_text=text.enabled&&std::ranges::any_of(indices,[&](std::size_t index){
        return std::ranges::any_of(candidate.layers[index].planes,[](const Plane& p){return p.family==0x40000004;});});
    auto target=make_layer(base,settings);
    if(native_text)target.common.position={Length{0,-1,0},Length{0,-1,0}}; // native RCLength default constructors
    if(indices.size()==1){target.common.x=source.common.x;target.common.y=source.common.y;if(!native_text)target.common.position=source.common.position;}
    const auto sink_for=[&](const Layer& from,const Plane& plane){return target.planes.size()==2?(from.family==target.family&&plane.family==0x12100001?0u:1u):0u;};
    if(native_text){
        //47DA50 retains the constructor page and processes each target plane
        //before advancing to the next. Later children observe an origin reset
        //by Text immediately; already written backing is never relocated.
        for(std::size_t sink=0;sink<target.planes.size();++sink)for(auto index:indices){const auto& from=candidate.layers[index];
            for(std::size_t pi=0;pi<from.planes.size();++pi)if(sink_for(from,from.planes[pi])==sink){
                auto& p=target.planes[sink];materialize_plane(data,target,p,from,pi,text);p.pixels_changed=true;}}
    }else for(auto index:indices){const auto& from=candidate.layers[index];
        expand_raster_layer(target,stack_detail::merged_owner_bounds(target.common,from));
        for(std::size_t pi=0;pi<from.planes.size();++pi){auto& p=target.planes[sink_for(from,from.planes[pi])];materialize_plane(data,target,p,from,pi,text);p.pixels_changed=true;}
    }
    const auto insertion=selected+1;candidate.layers.insert(candidate.layers.begin()+insertion,std::move(target));std::size_t result=insertion;
    if(!keep)for(auto i=indices.rbegin();i!=indices.rend();++i){const auto slot=*i>=insertion?*i+1:*i;candidate.layers.erase(candidate.layers.begin()+slot);if(slot<result)--result;}
    select_layer(candidate,result);data=std::move(candidate);return true;
}
bool merge_same_planes(DocumentData& data){
    auto candidate=data;auto& l=current(candidate);if(l.family&0x40000000u)return false;bool changed=false;
    for(std::size_t i=0;i<l.planes.size();++i)for(std::size_t j=i+1;j<l.planes.size();){
        auto& a=l.planes[i];const auto& b=l.planes[j];
        if(a.family!=b.family||((a.family&0x00200000u)&&a.common.color!=b.common.color)){++j;continue;}
        if(a.opaque||b.opaque)throw std::invalid_argument("Opaque plane merge");
        if(a.family&0x20000000u)stack_detail::merge_vector_planes(l,i,j);
        else if(a.raster&&b.raster){stack_detail::transfer_raster(a,l.common,b,l.common,false);a.pixels_changed=true;}
        else throw std::invalid_argument("Unsupported plane merge payload");
        l.planes.erase(l.planes.begin()+j);if(l.current_plane==int(j))l.current_plane=int(i);else if(l.current_plane>int(j))--l.current_plane;changed=true;
    }
    if(changed){if(l.current_plane>=0)l.planes[l.current_plane].common.flags[2]=1;data=std::move(candidate);}return changed;
}
bool merge_same_layers(DocumentData& data){
    if(data.unrestored_external_content)throw std::invalid_argument("Unrestored external layer content");auto candidate=data;bool changed=false;
    for(std::size_t i=0;i<candidate.layers.size();++i){auto& a=candidate.layers[i];if(a.family&0x40000000u)continue;
        for(std::size_t j=i+1;j<candidate.layers.size();){const auto b=candidate.layers[j];if(a.family!=b.family){++j;continue;}if(a.opaque||b.opaque)throw std::invalid_argument("Opaque layer merge");
            if(a.family&0x20000000u)stack_detail::append_vector_layer(a,b);
            else{
                expand_raster_layer(a,stack_detail::merged_owner_bounds(a.common,b));
                for(const auto& p:b.planes){if(p.opaque||!p.raster)throw std::invalid_argument("Unsupported layer merge child");std::size_t sink=0;
                    if(a.family!=0x10000100&&a.family!=0x10008000){auto clone=p;clone.common.runtime_identity=next_element_identity();clone.common.opacity_mode=stack_detail::plane_default_mode(p.family);clone.common.flags[2]=0;const double scale=p.raster->resolution/a.common.resolution;const int width=scaled(a.common.width*scale),height=scaled(a.common.height*scale);clone.raster=Raster(width,height,p.raster->depth,p.raster->resolution);clone.common.width=width;clone.common.height=height;clone.specific.clear();a.planes.push_back(std::move(clone));sink=a.planes.size()-1;}
                    if(a.planes.empty()||!a.planes[sink].raster)throw std::invalid_argument("Missing layer merge sink");
                    auto& to=a.planes[sink];stack_detail::transfer_raster(to,a.common,p,b.common,false);to.pixels_changed=true;
                }
            }
            candidate.layers.erase(candidate.layers.begin()+j);if(candidate.current_layer==int(j))candidate.current_layer=int(i);else if(candidate.current_layer>int(j))--candidate.current_layer;changed=true;
        }
    }
    if(changed){if(candidate.current_layer>=0)candidate.layers[candidate.current_layer].common.flags[2]=1;data=std::move(candidate);}return changed;
}
}
