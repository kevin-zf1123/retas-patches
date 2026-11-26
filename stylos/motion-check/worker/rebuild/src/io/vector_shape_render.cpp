#include "retas/vector_shape_render.hpp"
#include "retas/vector_extension_archive.hpp"
#include "retas/vector_color_metadata.hpp"
#include "vector_face_projection.hpp"
#include "vector_line_projection.hpp"
#include <algorithm>
#include <map>
#include <set>
namespace retas {
namespace {
template<class T> auto index(const std::vector<VectorPoolRecord<T>>& records){std::map<int,const T*> out;for(const auto& r:records)out.emplace(r.index,&r.value);return out;}
template<class Map> void claim(const std::vector<int>& refs,const Map& live,std::set<int>& owned){for(auto id:refs)if(id<0||!live.contains(id)||!owned.insert(id).second)throw FormatError(0,"Dead or competing Shape plane intrusive member");}
std::vector<int> refs(Reader& r){const auto n=r.i32();r.require_count(n,4,100000);std::vector<int> out;out.reserve(std::size_t(n));for(int i=0;i<n;++i)out.push_back(r.i32());return out;}
}
std::vector<VectorShapePlaneGeometry> decode_vector_shape_geometries(const Layer& layer){
    if(layer.opaque||layer.family!=0x40002000||layer.planes.size()>1024)throw std::invalid_argument("Unsupported Shape render owner");
    constexpr std::uint64_t max_bytes=512ull*1024*1024,max_work=128ull*1024*1024;
    std::uint64_t source=layer.specific.size();for(const auto& p:layer.planes){if(p.opaque||p.raster||p.specific.size()>64U*1024U*1024U)throw std::invalid_argument("Unsupported Shape render child");source+=p.specific.size();}
    if(source>max_bytes/16||source>max_work/std::max<std::size_t>(layer.planes.size(),1))throw std::length_error("Shape projection wire work/storage exceeds limits");
    auto a=decode_vector_shape_archive(layer);const auto& base=a.paint.base;
    const auto vertices=index(base.vertices);const auto models=index(base.models);const auto lines=index(base.lines);const auto subcurves=index(base.subcurves);
    const auto regions=index(a.paint.regions);const auto faces=index(a.paint.faces);const auto gradations=index(a.paint.gradations);const auto textures=index(a.paint.textures);const auto shapes=index(a.shapes);
    std::set<int> ov,om,ol,orr,of,og,ot,os;std::vector<VectorExtensionPlaneReferences> lists(layer.planes.size());
    claim(a.paint.border.vertices,vertices,ov);claim(a.paint.border.models,models,om);claim(a.paint.border.regions,regions,orr);
    for(std::size_t i=0;i<layer.planes.size();++i){const auto& p=layer.planes[i];auto& r=lists[i];
        if(p.family==0x40000002)r=vector_extension_plane_references(p);
        else if(p.family==0x21110001||p.family==0x21120001||p.family==0x21210001||p.family==0x21220001){auto t=vector_line_plane_topology(p);r.vertices=std::move(t.vertices);r.models=std::move(t.models);r.lines=std::move(t.lines);}
        else if(p.family==0x22100001||p.family==0x22100002){Reader reader(p.specific);r.faces=refs(reader);r.gradations=refs(reader);r.textures=refs(reader);}
        else throw std::invalid_argument("Unknown Shape companion family");
        claim(r.vertices,vertices,ov);claim(r.models,models,om);claim(r.regions,regions,orr);claim(r.lines,lines,ol);claim(r.faces,faces,of);claim(r.gradations,gradations,og);claim(r.textures,textures,ot);claim(r.extensions,shapes,os);
    }
    std::set<int> needed_faces;std::uint64_t commands{},spans{},nodes{},storage=source*16+2*512ull*512*sizeof(Pixel);
    const auto charge=[&](std::uint64_t& used,std::uint64_t n,std::uint64_t limit){if(used>limit||n>limit-used)throw std::length_error("Shape projected graph exceeds limits");used+=n;};
    std::vector<VectorShapePlaneGeometry> result(layer.planes.size());
    for(std::size_t i=0;i<layer.planes.size();++i){if(layer.planes[i].family!=0x40000002)continue;auto& out=result[i];std::set<int> needed_lines;
        charge(commands,lists[i].extensions.size(),100000);charge(storage,lists[i].extensions.size()*sizeof(VectorShapeCommand),max_bytes);
        for(auto id:lists[i].extensions){const auto& s=*shapes.at(id);if(s.line<0&&s.region<0)throw std::invalid_argument("Shape has no Line or Region gate");
            if(s.line>=0)needed_lines.insert(s.line);
            if(s.region>=0){const auto& f=regions.at(s.region)->faces;if(!f.empty()){if(f.front()<0)throw std::invalid_argument("Shape first Region Face is NULL");needed_faces.insert(f.front());}}
        }
        for(auto id:needed_lines)for(auto m:lines.at(id)->members){const auto& model=*models.at(m.model);const auto& b=model.scalars;
            if(out.model_bounds.emplace(m.model,VectorBounds{b[4],b[5],b[6],b[7]}).second)charge(storage,64+sizeof(std::pair<const int,VectorBounds>),max_bytes);
            charge(spans,model.subcurves.size(),100000);charge(storage,model.subcurves.size()*(sizeof(VectorLineSpan)+sizeof(VectorSpanProfiles)),max_bytes);
            for(auto sub:model.subcurves){const auto count=subcurves.at(sub)->scales.size();charge(nodes,count,100000);charge(storage,count*sizeof(VectorWidthNode),max_bytes);}}
        const std::vector<int> ids(needed_lines.begin(),needed_lines.end());out.lines.strokes=vector_archive_detail::project_line_strokes(base,ids,vector_archive_detail::LineClosurePolicy::StoredProfiles,&out.line_profiles);
    }
    const std::vector<int> ids(needed_faces.begin(),needed_faces.end());auto projected=vector_archive_detail::project_shape_faces(a.paint,ids,storage);
    std::map<int,std::shared_ptr<const VectorShapeFaceGeometry>> lowered;
    for(auto& f:projected){const auto& g=f.face.geometry;
        charge(storage,sizeof(g)+g.vertices.size()*sizeof(VectorFaceVertex)+g.models.size()*sizeof(VectorFaceModel)+g.loops.size()*sizeof(VectorFaceLoop)+g.holes.size()*sizeof(std::uint32_t),max_bytes);
        for(const auto& m:g.models)charge(storage,m.spans.size()*sizeof(VectorFaceSpan),max_bytes);for(const auto& l:g.loops)charge(storage,l.members.size()*sizeof(VectorFaceMember),max_bytes);
        charge(storage,(f.face.model_bounds.size()+f.face.loop_bounds.size())*sizeof(VectorBounds),max_bytes);lowered.emplace(f.face_index,std::make_shared<VectorShapeFaceGeometry>(std::move(f.face)));
    }
    for(std::size_t i=0;i<layer.planes.size();++i){auto& out=result[i];std::map<int,int> stroke,face;
        for(std::size_t n=0;n<out.lines.strokes.size();++n)stroke.emplace(out.lines.strokes[n].line_index,int(n));
        for(auto id:lists[i].extensions){const auto& s=*shapes.at(id);VectorShapeCommand command;
            if(s.line>=0){command.stroke=stroke.at(s.line);command.gate=lines.at(s.line)->archived_bounds;}
            else command.gate=regions.at(s.region)->outer.archived_bounds;
            if(s.region>=0&&!regions.at(s.region)->faces.empty()){const auto id=regions.at(s.region)->faces.front();const auto [f,inserted]=face.emplace(id,int(out.faces.size()));if(inserted)out.faces.push_back(lowered.at(id));command.face=f->second;}
            out.commands.push_back(command);
        }
    }
    if(!result.empty())result.front().reserved_storage=source*16;
    return result;
}
}
