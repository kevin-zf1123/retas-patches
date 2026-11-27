#include "retas/vector_ruler_render.hpp"
#include "retas/vector_extension_archive.hpp"
#include "retas/vector_color_metadata.hpp"
#include "vector_line_projection.hpp"
#include <algorithm>
#include <map>
#include <set>
namespace retas {
namespace {
template<class T> auto index(const std::vector<VectorPoolRecord<T>>& records){std::map<int,const T*> out;for(const auto& r:records)out.emplace(r.index,&r.value);return out;}
template<class Map> void claim(const std::vector<int>& refs,const Map& live,std::set<int>& owned){for(auto id:refs)if(id<0||!live.contains(id)||!owned.insert(id).second)throw FormatError(0,"Dead or competing Ruler plane intrusive member");}
int coordinate(std::int64_t v){if(v<std::numeric_limits<int>::min()||v>std::numeric_limits<int>::max())throw std::invalid_argument("Ruler circle bounds overflow");return int(v);}
}
std::vector<VectorRulerPlaneGeometry> decode_vector_ruler_geometries(const Layer& layer){
    if(layer.opaque||layer.family!=0x40004000||layer.planes.size()>1024)throw std::invalid_argument("Unsupported Ruler render owner");
    constexpr std::uint64_t max_bytes=512ull*1024*1024,max_work=128ull*1024*1024;
    std::uint64_t source=layer.specific.size();for(const auto& p:layer.planes){if(p.opaque||p.raster||p.specific.size()>64U*1024U*1024U)throw std::invalid_argument("Unsupported Ruler render child");source+=p.specific.size();}
    if(source>max_bytes/16||source>max_work/std::max<std::size_t>(layer.planes.size(),1))throw std::length_error("Ruler projection wire work/storage exceeds limits");
    const auto a=decode_vector_ruler_archive(layer);const auto vertices=index(a.base.vertices);const auto models=index(a.base.models);const auto lines=index(a.base.lines);const auto rulers=index(a.rulers);const auto subcurves=index(a.base.subcurves);
    std::set<int> owned_vertices,owned_models,owned_lines,owned_rulers;std::vector<VectorExtensionPlaneReferences> refs(layer.planes.size());
    for(std::size_t i=0;i<layer.planes.size();++i){const auto& p=layer.planes[i];auto& r=refs[i];
        if(p.family==0x40000001)r=vector_extension_plane_references(p);
        else if(p.family==0x21110001||p.family==0x21120001||p.family==0x21210001||p.family==0x21220001){auto t=vector_line_plane_topology(p);r.vertices=std::move(t.vertices);r.models=std::move(t.models);r.lines=std::move(t.lines);}
        else throw std::invalid_argument("Unknown Ruler companion family");
        claim(r.vertices,vertices,owned_vertices);claim(r.models,models,owned_models);claim(r.lines,lines,owned_lines);claim(r.extensions,rulers,owned_rulers);
    }
    std::uint64_t spans{},nodes{},commands{},storage=source*16+2*512ull*512*sizeof(Pixel);
    if(storage>max_bytes)throw std::length_error("Ruler projected storage exceeds limits");
    const auto charge=[&](std::uint64_t& used,std::uint64_t n,std::uint64_t limit){if(n>limit-used)throw std::length_error("Ruler projected graph exceeds aggregate limits");used+=n;};
    std::vector<VectorRulerPlaneGeometry> result(layer.planes.size());
    for(std::size_t i=0;i<layer.planes.size();++i){auto& p=result[i];p.color=layer.planes[i].common.color;if(layer.planes[i].family!=0x40000001)continue;
        charge(commands,refs[i].extensions.size(),100000);charge(storage,refs[i].extensions.size()*sizeof(VectorRulerCommand),max_bytes);
        std::set<int> needed;for(auto id:refs[i].extensions){const auto& r=*rulers.at(id);if(!(r.flags&4)){if(r.line<0)throw std::invalid_argument("Non-circle Ruler requires live Line");needed.insert(r.line);}}
        std::uint64_t plane_spans{};bool has_profiles=false;
        // Account for expanded copies before asking the strict Line projector
        // to allocate them. Repeated Ruler commands reuse one projected Line.
        for(auto id:needed)for(auto m:lines.at(id)->members){const auto& model=*models.at(m.model);const auto& b=model.scalars;
            if(p.model_bounds.emplace(m.model,VectorBounds{b[4],b[5],b[6],b[7]}).second)charge(storage,64+sizeof(std::pair<const int,VectorBounds>),max_bytes);
            charge(spans,model.subcurves.size(),100000);plane_spans+=model.subcurves.size();has_profiles=has_profiles||model.crosspoints[0]>=0||model.crosspoints[1]>=0;charge(storage,model.subcurves.size()*sizeof(VectorLineSpan),max_bytes);
            for(auto s:model.subcurves){const auto count=subcurves.at(s)->scales.size();charge(nodes,count,100000);charge(storage,count*sizeof(VectorWidthNode),max_bytes);}}
        if(has_profiles)charge(storage,plane_spans*sizeof(VectorSpanProfiles),max_bytes);
        const std::vector<int> selected(needed.begin(),needed.end());p.lines.strokes=vector_archive_detail::project_line_strokes(a.base,selected,vector_archive_detail::LineClosurePolicy::StoredProfiles,&p.line_profiles);std::map<int,int> stroke;
        for(std::size_t n=0;n<p.lines.strokes.size();++n)stroke.emplace(p.lines.strokes[n].line_index,int(n));
        for(auto id:refs[i].extensions){const auto& r=*rulers.at(id);VectorRulerCommand c;
            if(r.flags&4){c.center_x=r.x;c.center_y=r.y;c.radius=r.length_20;c.gate={coordinate(std::int64_t(r.x)-c.radius),coordinate(std::int64_t(r.y)-c.radius),coordinate(std::int64_t(r.x)+c.radius),coordinate(std::int64_t(r.y)+c.radius)};}
            else{c.stroke=stroke.at(r.line);c.gate=lines.at(r.line)->archived_bounds;}p.commands.push_back(c);
        }
    }
    if(!result.empty())result.front().reserved_storage=source*16;
    return result;
}
}
