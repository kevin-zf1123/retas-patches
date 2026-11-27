#include "vector_document_geometry.hpp"
#include "vector_material_geometry.hpp"
#include "modal_document_guard.hpp"
#include "retas/vector_extension_archive.hpp"
#include "retas/vector_color_metadata.hpp"
#include "retas/numeric.hpp"
#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace retas::vector_geometry_detail {
namespace {
using modal_detail::poll;
using numeric_detail::binary64;
int checked(std::int64_t value){
    if(value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())throw std::overflow_error("Vector geometry coordinate overflow");
    return int(value);
}
int truncated(double value){
    const auto integer=std::trunc(value);
    if(!std::isfinite(integer)||integer<std::numeric_limits<int>::min()||integer>std::numeric_limits<int>::max())throw std::overflow_error("Vector geometry numeric overflow");
    return int(integer);
}
bool paint(std::uint32_t family){return family==0x21200000||family==0x20800000||family==0x40002000;}
bool line_plane(std::uint32_t family){return family==0x21110001||family==0x21120001||family==0x21210001||family==0x21220001;}
void bounded(const Layer& layer){
    if(layer.opaque||!owner(layer.family)||layer.specific.size()>64U*1024U*1024U||layer.planes.size()>100000)
        throw std::invalid_argument("Unsupported vector geometry owner envelope");
    for(const auto& p:layer.planes){
        if(p.opaque||p.raster||!std::isfinite(p.common.resolution)||p.common.resolution<=0||p.specific.size()>64U*1024U*1024U
           ||(!line_plane(p.family)&&!(paint(layer.family)&&(p.family==0x22100001||p.family==0x22100002))
              &&!(layer.family==0x40004000&&p.family==0x40000001)&&!(layer.family==0x40002000&&p.family==0x40000002)))
            throw std::invalid_argument("Unsupported vector geometry child");
    }
}
template<class T> auto index(std::vector<VectorPoolRecord<T>>& records){
    std::map<int,T*> result;for(auto& record:records)result.emplace(record.index,&record.value);return result;
}
template<class T> std::set<int> identities(const std::vector<VectorPoolRecord<T>>& records){
    std::set<int> result;for(const auto& record:records)result.insert(record.index);return result;
}
void claim(const std::vector<int>& refs,const std::set<int>& live,std::set<int>& owned){
    for(auto id:refs)if(id<0||!live.contains(id)||!owned.insert(id).second)throw FormatError(0,"Dead or competing vector plane owner reference");
}
std::vector<int> refs(Reader& reader){
    const auto n=reader.i32();reader.require_count(n,4);if(n>100000)throw FormatError(0,"Vector plane reference limit exceeded");
    std::vector<int> out;out.reserve(std::size_t(n));for(int i=0;i<n;++i)out.push_back(reader.i32());return out;
}
void material_references(const VectorPaintArchive& face){
    const auto nodes=identities(face.gradnodes);std::map<int,const VectorArchiveFace*> faces;
    for(const auto& record:face.faces)faces.emplace(record.index,&record.value);
    std::set<int> owned_nodes,gradient_faces,texture_faces;
    const auto reverse=[&](const std::vector<int>& refs,int id,unsigned tag,std::set<int>& owned){
        for(auto ref:refs){if(ref<0)continue;const auto f=faces.find(ref);
            if(f==faces.end()||(f->second->flags&15)!=tag||f->second->material!=id)throw FormatError(0,"Vector material reverse face disagrees with direct material reference");
            owned.insert(ref); // Fresh pointer nodes retain same-owner duplicates and nullable holes.
        }
    };
    for(const auto& record:face.gradations){const auto g=decode_vector_gradation(record.value);
        for(auto id:g.stops)if(id<0||!nodes.contains(id)||!owned_nodes.insert(id).second)throw FormatError(0,"Dead or competing intrusive vector gradient stop");
        reverse(g.faces,record.index,2,gradient_faces);
    }
    for(const auto& record:face.textures){const auto t=decode_vector_texture(record.value);reverse(t.faces,record.index,4,texture_faces);}
    for(const auto& [id,f]:faces)if(f->material>=0){
        if(((f->flags&15)==2&&!gradient_faces.contains(id))||((f->flags&15)==4&&!texture_faces.contains(id)))throw FormatError(0,"Vector face has no reciprocal material reverse reference");
    }
}
void planes(const Layer& layer,const VectorLineArchive& base,const VectorPaintArchive* face,
            const std::vector<VectorPoolRecord<VectorArchiveRuler>>* rulers=nullptr,
            const std::vector<VectorPoolRecord<VectorArchiveShape>>* shapes=nullptr){
    const auto vertices=identities(base.vertices),models=identities(base.models),lines=identities(base.lines);
    std::set<int> owned_vertices,owned_models,owned_lines,owned_faces,owned_grads,owned_textures,owned_regions,owned_extensions;
    std::set<int> faces,grads,textures,regions,extensions;
    if(rulers)extensions=identities(*rulers);if(shapes)extensions=identities(*shapes);
    if(face){material_references(*face);faces=identities(face->faces);grads=identities(face->gradations);textures=identities(face->textures);regions=identities(face->regions);
        claim(face->border.vertices,vertices,owned_vertices);claim(face->border.models,models,owned_models);claim(face->border.regions,regions,owned_regions);}
    for(const auto& p:layer.planes){
        if(line_plane(p.family)){const auto topology=vector_line_plane_topology(p);
            claim(topology.vertices,vertices,owned_vertices);claim(topology.models,models,owned_models);claim(topology.lines,lines,owned_lines);
        }else if(p.family==0x40000001||p.family==0x40000002){
            const auto lists=vector_extension_plane_references(p);
            claim(lists.vertices,vertices,owned_vertices);claim(lists.models,models,owned_models);claim(lists.regions,regions,owned_regions);
            claim(lists.lines,lines,owned_lines);claim(lists.faces,faces,owned_faces);claim(lists.gradations,grads,owned_grads);claim(lists.textures,textures,owned_textures);
            claim(lists.extensions,extensions,owned_extensions);
        }else{
            if(!face)throw std::invalid_argument("Paint plane in base line owner");
            Reader r(p.specific);claim(refs(r),faces,owned_faces);claim(refs(r),grads,owned_grads);claim(refs(r),textures,owned_textures);
        }
    }
}
void resources(const Layer& layer,std::uint64_t& allocation,std::uint64_t& work){
    std::uint64_t bytes=layer.specific.size();for(const auto& p:layer.planes)bytes+=p.specific.size();
    // Wire copies, decoded vectors and map/set nodes are simultaneously live.
    // Charge before parsing; high-water markers never materialize holes.
    modal_detail::charge(allocation,bytes*16,modal_detail::max_bytes,"Vector geometry staging exceeds512MiB");
    modal_detail::charge(work,bytes*2,modal_detail::max_work,"Vector geometry parsing exceeds128Mi work units");
}
void offset(VectorBounds& b,int x,int y){
    b={checked(std::int64_t(b.left)+x),checked(std::int64_t(b.top)+y),checked(std::int64_t(b.right)+x),checked(std::int64_t(b.bottom)+y)};
}
void translate(VectorLineArchive& a,int x,int y,std::stop_token stop){
    for(auto& r:a.vertices){poll(stop);r.value.x=checked(std::int64_t(r.value.x)+x);r.value.y=checked(std::int64_t(r.value.y)+y);}
    for(auto& r:a.subcurves){poll(stop);r.value.control_x=checked(std::int64_t(r.value.control_x)+x);r.value.control_y=checked(std::int64_t(r.value.control_y)+y);}
    for(auto& r:a.models){poll(stop);for(std::size_t i=0;i<8;i+=2){r.value.scalars[i]=checked(std::int64_t(r.value.scalars[i])+x);r.value.scalars[i+1]=checked(std::int64_t(r.value.scalars[i+1])+y);}}
    for(auto& r:a.crosspoints){poll(stop);for(std::size_t i=4;i<14;i+=2){r.value.words[i]=std::bit_cast<std::uint32_t>(checked(std::int64_t(std::bit_cast<std::int32_t>(r.value.words[i]))+x));r.value.words[i+1]=std::bit_cast<std::uint32_t>(checked(std::int64_t(std::bit_cast<std::int32_t>(r.value.words[i+1]))+y));}}
    for(auto& r:a.lines){poll(stop);offset(r.value.archived_bounds,x,y);}
}
struct Matrix {
    double x,y,tx,ty,xy{},yx{};
    double scale()const{return yx==0?std::abs(x):binary64(std::sqrt(binary64(binary64(x*x)+binary64(yx*yx))));}
    bool reflected()const{return binary64(binary64(x*y)-binary64(xy*yx))<0;}
    void point(int& px,int& py)const{
        const double ox=px,oy=py;
        px=truncated(binary64((xy==0?binary64(ox*x):binary64(binary64(ox*x)+binary64(oy*xy)))+tx));
        py=truncated(binary64((yx==0?binary64(oy*y):binary64(binary64(ox*yx)+binary64(oy*y)))+ty));
    }
};
bool empty(VectorBounds b){return b.left>=b.right||b.top>=b.bottom;}
void unite(VectorBounds& a,VectorBounds b){
    if(empty(b))return;if(empty(a)){a=b;return;}
    a={std::min(a.left,b.left),std::min(a.top,b.top),std::max(a.right,b.right),std::max(a.bottom,b.bottom)};
}
void point(VectorBounds& bounds,int x,int y){unite(bounds,{x,y,checked(std::int64_t(x)+1),checked(std::int64_t(y)+1)});}
void crosspoint(VectorBounds& bounds,const VectorCrosspoint& c){
    const auto add=[&](std::size_t n){point(bounds,std::bit_cast<std::int32_t>(c.words[4+2*n]),std::bit_cast<std::int32_t>(c.words[5+2*n]));};
    add(0);add(4);
    if(c.words[0]==1||c.words[0]==4)add(2);else if(c.words[0]==2){add(1);add(2);}
    if(c.words[1]==1||c.words[1]==4)add(2);else if(c.words[1]==2){add(3);add(2);}
}
struct Bounds {
    std::map<int,VectorVertex*> vertices;
    std::map<int,VectorLineScale*> scales;
    std::map<int,VectorSubcurve*> subcurves;
    std::map<int,VectorModelCurve*> models;
    std::map<int,VectorCrosspoint*> crosspoints;
    std::map<int,VectorLine*> lines;
    std::uint64_t& work;std::stop_token stop;
    void model(int id){
        poll(stop);auto& m=*models.at(id);if(m.subcurves.empty())return; // Native retains the old bounds.
        modal_detail::charge(work,m.subcurves.size(),modal_detail::max_work,"Vector model bounds exceed128Mi work units");
        const auto* first=subcurves.at(m.subcurves.front());
        if(first->vertex_a<0)throw std::invalid_argument("Nullable vector bounds endpoint");
        const auto* a=vertices.at(first->vertex_a);VectorBounds center{};point(center,a->x,a->y);
        double scale{};
        for(auto sub:m.subcurves){poll(stop);const auto* s=subcurves.at(sub);
            if(s->vertex_b<0)throw std::invalid_argument("Nullable vector bounds endpoint");
            if(!(m.flags&1))point(center,s->control_x,s->control_y);
            const auto* b=vertices.at(s->vertex_b);point(center,b->x,b->y);
            if(m.line>=0&&!(m.flags&2)){
                modal_detail::charge(work,s->scales.size(),modal_detail::max_work,"Vector width bounds exceed128Mi work units");
                for(auto node:s->scales){const double v=std::bit_cast<float>(scales.at(node)->scale_bits);
                    if(!std::isfinite(v))throw std::invalid_argument("Non-finite active vector width scale");scale=std::max(scale,v);}
            }
        }
        VectorBounds stroke{};
        if(m.line>=0){const double half=std::bit_cast<float>(lines.at(m.line)->half_width_bits);
            const int inflate=truncated(binary64(half*((m.flags&2)?1:scale)));
            stroke={checked(std::int64_t(center.left)-inflate),checked(std::int64_t(center.top)-inflate),checked(std::int64_t(center.right)+inflate),checked(std::int64_t(center.bottom)+inflate)};
            for(auto id:m.crosspoints)if(id>=0)crosspoint(stroke,*crosspoints.at(id));
        }
        m.scalars={center.left,center.top,center.right,center.bottom,stroke.left,stroke.top,stroke.right,stroke.bottom};
    }
    void line(VectorLine& l,bool refresh_models=true){
        l.archived_bounds={};modal_detail::charge(work,l.members.size(),modal_detail::max_work,"Vector line bounds exceed128Mi work units");
        for(auto member:l.members){if(refresh_models)model(member.model);const auto& v=models.at(member.model)->scalars;unite(l.archived_bounds,{v[4],v[5],v[6],v[7]});}
    }
    void loop(VectorArchiveLoop& l){
        l.archived_bounds={};modal_detail::charge(work,l.members.size(),modal_detail::max_work,"Vector loop bounds exceed128Mi work units");
        for(auto member:l.members){model(member.model);const auto& v=models.at(member.model)->scalars;unite(l.archived_bounds,{v[0],v[1],v[2],v[3]});}
    }
};
Bounds transform(VectorLineArchive& a,const Matrix& matrix,std::uint64_t& work,std::stop_token stop){
    for(auto& r:a.vertices){poll(stop);matrix.point(r.value.x,r.value.y);}
    for(auto& r:a.subcurves){poll(stop);matrix.point(r.value.control_x,r.value.control_y);}
    if(matrix.reflected())for(auto& r:a.models){poll(stop);std::swap(r.value.targets[0],r.value.targets[1]);std::swap(r.value.target_kinds[0],r.value.target_kinds[1]);}
    for(auto& r:a.crosspoints){poll(stop);for(std::size_t i=4;i<14;i+=2){int x=std::bit_cast<std::int32_t>(r.value.words[i]),y=std::bit_cast<std::int32_t>(r.value.words[i+1]);matrix.point(x,y);r.value.words[i]=std::bit_cast<std::uint32_t>(x);r.value.words[i+1]=std::bit_cast<std::uint32_t>(y);}
        if(matrix.reflected()){auto& w=r.value.words;std::swap(w[0],w[1]);std::swap(w[2],w[3]);std::swap(w[4],w[12]);std::swap(w[5],w[13]);std::swap(w[6],w[10]);std::swap(w[7],w[11]);}}
    // All live lines are visited, including those not owned by any plane.
    // Each line scales its own width immediately before recalculating members.
    Bounds b{index(a.vertices),index(a.scales),index(a.subcurves),index(a.models),index(a.crosspoints),index(a.lines),work,stop};
    for(auto& r:a.lines){poll(stop);const double half=std::bit_cast<float>(r.value.half_width_bits);
        if(!std::isfinite(half)||half<0)throw std::invalid_argument("Invalid vector half-width");
        const double product=binary64(half*matrix.scale());
        if(!std::isfinite(product)||product>std::numeric_limits<float>::max())throw std::invalid_argument("Invalid transformed vector half-width");
        const float value=static_cast<float>(product);
        r.value.half_width_bits=std::bit_cast<std::uint32_t>(value);b.line(r.value);}
    return b;
}
void border_crop(VectorPaintArchive& a,Rect target,double dpi){
    auto& c=a.border.common;const double r=binary64(c.resolution/dpi);
    if(!std::isfinite(r)||r<=0)throw std::invalid_argument("Invalid geometry border DPI ratio");
    c.width=checked(std::int64_t(truncated(binary64((double(target.x)+target.width)*r)))-truncated(binary64(double(target.x)*r)));
    c.height=checked(std::int64_t(truncated(binary64((double(target.y)+target.height)*r)))-truncated(binary64(double(target.y)*r)));
    if(c.width<0||c.height<0)throw std::invalid_argument("Invalid geometry border extent");
}
// Decode the entire extension graph before invoking an edit. Ordinary nullable
// Line/Region pointers may be shared; only plane owner lists are exclusive.
using Rulers=std::vector<VectorPoolRecord<VectorArchiveRuler>>;
using Shapes=std::vector<VectorPoolRecord<VectorArchiveShape>>;
template<class Edit> Bytes archive(const Layer& layer,bool write,Edit edit){
    if(layer.family==0x40004000){auto a=decode_vector_ruler_archive(layer);edit(a.base,nullptr,&a.rulers,nullptr);return write?encode_vector_ruler_archive(a):Bytes{};}
    if(layer.family==0x40002000){auto a=decode_vector_shape_archive(layer);edit(a.paint.base,&a.paint,nullptr,&a.shapes);return write?encode_vector_shape_archive(a):Bytes{};}
    if(paint(layer.family)){auto a=decode_vector_paint_archive(layer);edit(a.base,&a,nullptr,nullptr);return write?encode_vector_paint_archive(a):Bytes{};}
    auto a=decode_vector_line_archive(layer);edit(a,nullptr,nullptr,nullptr);return write?encode_vector_line_archive(a):Bytes{};
}
void extra_point(int& x,int& y,int dx,int dy){x=checked(std::int64_t(x)+dx);y=checked(std::int64_t(y)+dy);}
void extra_transform(int& x,int& y,int& l1,int& l2,std::uint32_t& angle,const Matrix& m){
    //5CDEC0 stores GetScale and angle as F32 before use. Both lengths use SX.
    const auto scale=m.scale();if(!std::isfinite(scale)||scale>std::numeric_limits<float>::max())throw std::invalid_argument("Invalid vector extension scale");
    const float s=static_cast<float>(scale);float a=std::bit_cast<float>(angle);
    if(!std::isfinite(a))throw std::invalid_argument("Non-finite vector extension angle");
    const double deg=double(std::bit_cast<float>(0x42652ee0u));
    double theta=m.x>0?binary64(std::atan(binary64(m.yx/m.x))*deg):m.x<0?binary64(binary64(std::atan(binary64(m.yx/m.x))*deg)+180):m.yx<0?270:90;
    if(theta<0)theta=binary64(theta+360);const float delta=static_cast<float>(theta);
    a=m.reflected()?delta-a:a+delta;if(a<0)a=a+360.0f;else if(a>=360)a=a-360.0f; // One wrap, not modulo.
    m.point(x,y);l1=truncated(binary64(double(l1)*s));l2=truncated(binary64(double(l2)*s));angle=std::bit_cast<std::uint32_t>(a);
}
void translate_paint(VectorPaintArchive& a,int x,int y,std::stop_token stop){
    for(auto& r:a.loops){poll(stop);offset(r.value.archived_bounds,x,y);}for(auto& r:a.regions){poll(stop);offset(r.value.outer.archived_bounds,x,y);}
    for(auto& r:a.gradations){poll(stop);auto g=decode_vector_gradation(r.value);extra_point(g.start_x,g.start_y,x,y);extra_point(g.end_x,g.end_y,x,y);r.value=encode_vector_gradation(g);}
    for(auto& r:a.textures){poll(stop);auto t=decode_vector_texture(r.value);extra_point(t.origin_x,t.origin_y,x,y);r.value=encode_vector_texture(t);}
}
void resolution_paint(VectorPaintArchive& a,Bounds& bounds,const Matrix& matrix,int width,int height,double dpi,std::uint64_t& work,std::stop_token stop){
    for(auto& r:a.loops){poll(stop);bounds.loop(r.value);}for(auto& r:a.regions){poll(stop);bounds.loop(r.value.outer);}
    const vector_material_detail::Matrix material_matrix{matrix.x,matrix.y,matrix.tx,matrix.ty,matrix.xy,matrix.yx};
    for(auto& r:a.gradations){poll(stop);modal_detail::charge(work,36,modal_detail::max_work,"Vector material transform exceeds128Mi work units");auto g=decode_vector_gradation(r.value);vector_material_detail::transform(g,material_matrix);r.value=encode_vector_gradation(g);}
    for(auto& r:a.textures){poll(stop);modal_detail::charge(work,1,modal_detail::max_work,"Vector material transform exceeds128Mi work units");auto t=decode_vector_texture(r.value);vector_material_detail::transform(t,material_matrix);r.value=encode_vector_texture(t);}
    auto& c=a.border.common;const double ratio=binary64(c.resolution/dpi); // Host630CB0 after root DPI publication.
    const double border_dpi=binary64(dpi*ratio);
    if(!std::isfinite(ratio)||ratio<=0||!std::isfinite(border_dpi)||border_dpi<=0)throw std::invalid_argument("Invalid resolution border ratio");
    c.width=truncated(binary64(width*ratio));c.height=truncated(binary64(height*ratio));c.resolution=border_dpi;
}
}
bool owner(std::uint32_t family){return paint(family)||family==0x40004000||family==0x22100000||family==0x24400000||family==0x20000200;}
void rebuild_lines(VectorLineArchive& archive,const std::vector<int>& lines,std::uint64_t& work,std::stop_token stop,bool refresh_models){
    Bounds b{index(archive.vertices),index(archive.scales),index(archive.subcurves),index(archive.models),index(archive.crosspoints),index(archive.lines),work,stop};
    for(auto id:lines){poll(stop);b.line(*b.lines.at(id),refresh_models);}
}
void rebuild_models(VectorLineArchive& archive,const std::vector<int>& models,std::uint64_t& work,std::stop_token stop){
    Bounds b{index(archive.vertices),index(archive.scales),index(archive.subcurves),index(archive.models),index(archive.crosspoints),index(archive.lines),work,stop};
    for(auto id:models){poll(stop);b.model(id);}
}
void validate(const Layer& layer,std::uint64_t& allocation,std::uint64_t& work,std::stop_token stop){
    bounded(layer);resources(layer,allocation,work);poll(stop);
    (void)archive(layer,false,[&](VectorLineArchive& a,VectorPaintArchive* f,Rulers* r,Shapes* s){planes(layer,a,f,r,s);});poll(stop);
}
void translate_cache(Layer& layer,int x,int y,std::uint64_t& allocation,std::uint64_t& work,std::stop_token stop){
    validate(layer,allocation,work,stop);if(!x&&!y)return;
    layer.specific=archive(layer,true,[&](VectorLineArchive& a,VectorPaintArchive* f,Rulers* rulers,Shapes* shapes){
        planes(layer,a,f,rulers,shapes);translate(a,x,y,stop);if(f)translate_paint(*f,x,y,stop);
        if(rulers)for(auto& r:*rulers){poll(stop);extra_point(r.value.x,r.value.y,x,y);}
        if(shapes)for(auto& r:*shapes){poll(stop);extra_point(r.value.x,r.value.y,x,y);}
    });
}
void crop(Layer& layer,Rect target,std::uint64_t& allocation,std::uint64_t& work,std::stop_token stop){
    (void)allocation;(void)work;bounded(layer);poll(stop); // Shared admission precedes first decode.
    const int x=checked(-16*std::int64_t(target.x)),y=checked(-16*std::int64_t(target.y));
    layer.specific=archive(layer,true,[&](VectorLineArchive& a,VectorPaintArchive* f,Rulers* rulers,Shapes* shapes){
        planes(layer,a,f,rulers,shapes);translate(a,x,y,stop);if(f){translate_paint(*f,x,y,stop);border_crop(*f,target,layer.common.resolution);}
        if(rulers)for(auto& r:*rulers){poll(stop);extra_point(r.value.x,r.value.y,x,y);}
        if(shapes)for(auto& r:*shapes){poll(stop);extra_point(r.value.x,r.value.y,x,y);}
    });
}
void resolution(Layer& layer,int old_width,int old_height,int width,int height,double dpi,std::uint64_t& allocation,std::uint64_t& work,std::stop_token stop){
    (void)allocation;bounded(layer);poll(stop);
    const double sx=binary64(double(width)/old_width),sy=binary64(double(height)/old_height);
    const auto translation=[](int point,double scale){const double v=binary64(double(checked(std::int64_t(point)*16))*scale);return truncated(binary64(v+(v>=0?0.5:-0.5)));};
    const Matrix matrix{sx,sy,double(translation(layer.common.x,sx)),double(translation(layer.common.y,sy))};
    layer.specific=archive(layer,true,[&](VectorLineArchive& a,VectorPaintArchive* f,Rulers* rulers,Shapes* shapes){
        planes(layer,a,f,rulers,shapes);auto bounds=transform(a,matrix,work,stop);if(f)resolution_paint(*f,bounds,matrix,width,height,dpi,work,stop);
        if(rulers)for(auto& r:*rulers){poll(stop);auto& v=r.value;extra_transform(v.x,v.y,v.length_20,v.length_24,v.angle_bits,matrix);v.length_30=truncated(binary64(double(v.length_30)*sx));v.length_34=truncated(binary64(double(v.length_34)*sx));}
        if(shapes)for(auto& r:*shapes){poll(stop);auto& v=r.value;extra_transform(v.x,v.y,v.length_24,v.length_28,v.angle_bits,matrix);}
    });
}
void page_transform(Layer& layer,std::int32_t kind,std::uint64_t& allocation,std::uint64_t& work,std::stop_token stop){
    // Raster companions take their own rotate/mirror slots in the caller;
    // they do not participate in the typed primitive/material projection.
    auto envelope=layer;std::erase_if(envelope.planes,[](const Plane& p){return p.raster.has_value();});
    validate(envelope,allocation,work,stop);const int width=layer.common.width,height=layer.common.height;
    if(width<0||height<0)throw std::invalid_argument("Negative vector page dimensions");
    const double cx=checked(std::int64_t(width/2)*16),cy=checked(std::int64_t(height/2)*16);Matrix matrix{1,1,0,0};
    if(kind==28){matrix.x=-1;matrix.tx=2*cx;}
    else if(kind==29){matrix.y=-1;matrix.ty=2*cy;}
    else if(kind==30||kind==31){
        const double theta=binary64((kind==30?270.0:90.0)*0x1.1df46a2529d39p-6);const double cosine=binary64(std::cos(theta)),sine=binary64(std::sin(theta));
        matrix={cosine,cosine,binary64(cy-binary64(binary64(cx*cosine)-binary64(cy*sine))),binary64(cx-binary64(binary64(cx*sine)+binary64(cy*cosine))),-sine,sine};
    }else throw std::invalid_argument("Invalid vector page transform kind");
    layer.specific=archive(envelope,true,[&](VectorLineArchive& a,VectorPaintArchive* f,Rulers* rulers,Shapes* shapes){
        planes(envelope,a,f,rulers,shapes);auto bounds=transform(a,matrix,work,stop);
        if(f){
            const auto loop=[&](VectorArchiveLoop& l){if(matrix.reflected()){std::ranges::reverse(l.members);for(auto& member:l.members)member.selector=!member.selector;}bounds.loop(l);};
            for(auto& r:f->loops){poll(stop);loop(r.value);}for(auto& r:f->regions){poll(stop);loop(r.value.outer);}
            const vector_material_detail::Matrix material{matrix.x,matrix.y,matrix.tx,matrix.ty,matrix.xy,matrix.yx};
            for(auto& r:f->gradations){poll(stop);modal_detail::charge(work,36,modal_detail::max_work,"Page gradient work limit");auto g=decode_vector_gradation(r.value);vector_material_detail::transform(g,material);r.value=encode_vector_gradation(g);}
            for(auto& r:f->textures){poll(stop);auto t=decode_vector_texture(r.value);vector_material_detail::transform(t,material);r.value=encode_vector_texture(t);}
        }
        if(rulers)for(auto& r:*rulers){poll(stop);auto& v=r.value;extra_transform(v.x,v.y,v.length_20,v.length_24,v.angle_bits,matrix);v.length_30=truncated(binary64(double(v.length_30)*matrix.scale()));v.length_34=truncated(binary64(double(v.length_34)*matrix.scale()));}
        if(shapes)for(auto& r:*shapes){poll(stop);auto& v=r.value;extra_transform(v.x,v.y,v.length_24,v.length_28,v.angle_bits,matrix);}
    });
}
}
