#include "retas/vector_indication.hpp"
#include "vector_indication.hpp"
#include "retas/vector_color_metadata.hpp"
#include "retas/vector_extension_archive.hpp"
#include "vector_line_selection.hpp"
#include "vector_region_probe.hpp"
#include "modal_document_guard.hpp"
#include <algorithm>
#include <bit>
#include <map>
#include <limits>
#include <optional>

namespace retas {
namespace {
using vector_probe_detail::Budget;
constexpr std::uint64_t max_storage=512ull*1024*1024;
struct Resources {
    std::uint64_t storage{},work{};std::stop_token stop;
    void poll()const{modal_detail::poll(stop);}
    Budget budget(){return {work,stop};}
    void bytes(std::uint64_t n){modal_detail::charge(storage,n,max_storage,"Vector indication exceeds512MiB storage");}
    void source(const Layer& l){poll();if(l.opaque||l.planes.size()>1024)throw std::invalid_argument("Unsupported vector indication owner");
        std::uint64_t wire=l.specific.size();for(const auto& p:l.planes){poll();if(p.specific.size()>8ull*1024*1024||wire>8ull*1024*1024-p.specific.size())throw std::length_error("Vector indication source exceeds8MiB");wire+=p.specific.size();}
        if(wire>8ull*1024*1024)throw std::length_error("Vector indication source exceeds8MiB");bytes(wire*32);budget().charge(wire*8);
    }
};
template<class T> auto index(std::vector<VectorPoolRecord<T>>& pool){std::map<int,T*> out;for(auto& r:pool)out.emplace(r.index,&r.value);return out;}
template<class T> T& live(const std::map<int,T*>& pool,int id){const auto i=pool.find(id);if(i==pool.end())throw std::invalid_argument("Dead vector indication reference");return *i->second;}
bool ordinary(const Layer& l){return l.family==0x22100000||l.family==0x24400000||l.family==0x20000200||l.family==0x21200000||l.family==0x20800000;}
bool painting(const Layer& l){return l.family==0x21200000||l.family==0x20800000;}
bool extension(const Layer& l){return l.family==0x40004000||l.family==0x40002000;}
bool line_plane(const Plane& p){return p.family==0x21110001||p.family==0x21120001||p.family==0x21210001||p.family==0x21220001;}
bool face_plane(const Plane& p){return p.family==0x22100001||p.family==0x22100002;}
std::vector<int> face_refs(const Plane& p,Resources& r){
    //The marker walker consumes only the first, intrusive Face chain. Material
    //lists and their retained tail belong to other consumers.
    Reader in(p.specific);const int count=in.i32();in.require_count(count,4,100000);
    r.bytes(std::uint64_t(count)*sizeof(int));r.budget().charge(count);
    std::vector<int> ids;ids.reserve(std::size_t(count));for(int i=0;i<count;++i)ids.push_back(in.i32());return ids;
}
using TopologyDpis=std::map<int,double>;
void add_topology(TopologyDpis& owners,const std::vector<int>& ids,double dpi,Resources& r){
    r.bytes(std::uint64_t(ids.size())*64);
    for(int id:ids){r.budget().charge(2+std::bit_width(owners.size()));if(!owners.emplace(id,dpi).second)throw std::invalid_argument("Shared indication Region topology owner");}
}
double topology_dpi(const TopologyDpis& owners,int id,Resources& r){
    r.budget().charge(2+std::bit_width(owners.size()));const auto found=owners.find(id);
    return found==owners.end()?std::numeric_limits<double>::quiet_NaN():found->second;
}
struct OrdinaryArchive {
    std::optional<VectorPaintArchive> paint;VectorLineArchive line;
    explicit OrdinaryArchive(const Layer& l){if(painting(l))paint=decode_vector_paint_archive(l);else line=decode_vector_line_archive(l);}
    VectorLineArchive& base(){return paint?paint->base:line;}
    void publish(Layer& l){if(paint)store_vector_paint_archive(l,*paint);else l.specific=encode_vector_line_archive(line);}
};
void clear_owner(Layer& owner,Resources& r){
    r.source(owner);
    if(ordinary(owner)){
        OrdinaryArchive archive(owner);auto lines=index(archive.base().lines);auto models=index(archive.base().models);
        auto faces=archive.paint?index(archive.paint->faces):std::map<int,VectorArchiveFace*>{};
        for(auto& plane:owner.planes){r.budget().charge();if(!plane.common.flags[3])continue;
            if(line_plane(plane)){if(plane.opaque||plane.raster)throw std::invalid_argument("Unsupported indication Line plane");
                for(int id:vector_line_plane_references(plane)){r.budget().charge();for(auto member:live(lines,id).members){r.budget().charge();live(models,member.model).flags&=~0x10000u;}}
                plane.common.flags[3]=0;
            }else if(face_plane(plane)){if(!archive.paint||plane.opaque||plane.raster)throw std::invalid_argument("Unsupported indication Face plane");
                for(int id:face_refs(plane,r)){r.budget().charge();live(faces,id).flags&=~0x10000u;}plane.common.flags[3]=0;
            }
        }archive.publish(owner);
    }else if(owner.family==0x40004000){
        auto archive=decode_vector_ruler_archive(owner);auto rulers=index(archive.rulers);
        for(auto& plane:owner.planes){r.budget().charge();if(!plane.common.flags[3])continue;
            if(plane.opaque||plane.raster||plane.family!=0x40000001)throw std::invalid_argument("Unsupported indication Ruler plane");
            for(int id:vector_extension_plane_references(plane).extensions){r.budget().charge();live(rulers,id).flags&=~0x10000u;}
        }owner.specific=encode_vector_ruler_archive(archive);
    }else if(owner.family==0x40002000){
        auto archive=decode_vector_shape_archive(owner);auto shapes=index(archive.shapes);
        for(auto& plane:owner.planes){r.budget().charge();if(!plane.common.flags[3])continue;
            if(plane.opaque||plane.raster||plane.family!=0x40000002)throw std::invalid_argument("Unsupported indication Shape plane");
            for(int id:vector_extension_plane_references(plane).extensions){r.budget().charge();live(shapes,id).flags&=~0x10000u;}
        }store_vector_shape_archive(owner,archive);
    }else throw std::invalid_argument("Unsupported marked vector indication owner");
    owner.common.flags[3]=0;
}
void clear(DocumentData& data,bool active,Resources& r){
    r.poll();if(!active)return;
    for(auto& owner:data.layers){r.budget().charge();if(!owner.common.flags[3])continue;
        if((owner.family&0x20000000u)||extension(owner))clear_owner(owner,r);
    }
}
struct Scope {
    FrozenVectorLineSelection packet;
    vector_probe_detail::Scope value;
    Scope(const RegionSelection& selection,const DocumentData& data,const Layer& owner,double carrier_dpi,Resources& r)
        :packet(prepare_vector_line_selection(selection,data.base.dpi,carrier_dpi,r.stop)),
         value(vector_line_selection_detail::reserve_line_selection_scope(packet,{owner.common.x,owner.common.y},r.storage,r.work,r.stop)){}
};
bool eligible(const Plane& p,bool one){return p.common.flags[1]&&(!one||p.common.flags[2]);}
bool refresh_ordinary(Layer& owner,const RegionSelection& selection,const DocumentData& data,std::uint32_t flags,double carrier_dpi,bool one,Resources& r){
    r.source(owner);OrdinaryArchive archive(owner);auto& base=archive.base();Scope scoped(selection,data,owner,carrier_dpi,r);const auto& scope=scoped.value;
    vector_probe_detail::Graph graph(base,r.budget());auto lines=index(base.lines);auto models=index(base.models);
    const bool all=flags&4u;bool owner_hit{};
    std::map<int,Plane*> line_owners;
    for(auto& p:owner.planes)if(line_plane(p))for(int id:vector_line_plane_references(p))if(!line_owners.emplace(id,&p).second)throw std::invalid_argument("Shared indication LC plane owner");
    std::optional<vector_probe_detail::PaintGraph> paint_graph;std::map<int,VectorArchiveFace*> faces;TopologyDpis region_dpis;
    if(archive.paint){paint_graph.emplace(*archive.paint,r.budget());faces=index(archive.paint->faces);
        if(scope.mask)add_topology(region_dpis,archive.paint->border.regions,archive.paint->border.common.resolution,r);}
    const auto mark_boundary=[&](const VectorArchiveLoop& loop){
        for(auto member:loop.members){r.budget().charge();auto& model=live(models,member.model);if(model.line<0)continue;
            const auto found=line_owners.find(model.line);if(found==line_owners.end())continue;
            auto& plane=*found->second;if(plane.common.flags[2]){model.flags|=0x10000u;plane.common.flags[3]=1;}}
    };
    for(auto& plane:owner.planes){r.budget().charge();if(!eligible(plane,one))continue;bool hit{};
        if(line_plane(plane)&&(flags&0x100u)&&(flags&0x30u)){
            if(plane.opaque||plane.raster)throw std::invalid_argument("Unsupported indication Line plane");
            for(int id:vector_line_plane_references(plane)){r.budget().charge();auto& line=live(lines,id);
                if(flags&0x10u){if(!vector_probe_detail::overlap(line.archived_bounds,scope.bounds))continue;
                    for(auto member:line.members){r.budget().charge();auto& model=live(models,member.model);if(graph.model_matches(model,scope,all,r.budget())){model.flags|=0x10000u;hit=true;}}
                }else if(graph.line_matches(line,scope,all,r.budget())){for(auto member:line.members){r.budget().charge();live(models,member.model).flags|=0x10000u;}hit=true;}
            }
        }else if(face_plane(plane)&&((flags&0x200u)||((flags&0x140u)==0x140u))){
            if(!paint_graph||plane.opaque||plane.raster)throw std::invalid_argument("Unsupported indication Face plane");
            for(int id:face_refs(plane,r)){r.budget().charge();auto& face=live(faces,id);const auto region=paint_graph->regions.find(face.region);if(region==paint_graph->regions.end())throw std::invalid_argument("Indication Face lacks a Region");
                const double dpi=scope.mask?topology_dpi(region_dpis,face.region,r):0;
                if(!paint_graph->matches(*region->second,scope,all,dpi,r.storage,r.budget(),scope.mask?dpi/carrier_dpi:0))continue;
                if(flags&0x200u)face.flags|=0x10000u;
                if((flags&0x140u)==0x140u){mark_boundary(region->second->outer);for(int hole:region->second->holes){r.budget().charge();const auto loop=paint_graph->loops.find(hole);if(loop==paint_graph->loops.end())throw std::invalid_argument("Indication Region lacks a hole");mark_boundary(*loop->second);}}
                hit=true;
            }
        }
        if(hit){plane.common.flags[3]=1;owner.common.flags[3]=1;owner_hit=true;}
    }
    archive.publish(owner);return owner_hit;
}
bool refresh_extension(Layer& owner,const RegionSelection& selection,const DocumentData& data,std::uint32_t flags,double carrier_dpi,bool one,Resources& r){
    r.source(owner);Scope scoped(selection,data,owner,carrier_dpi,r);const auto& scope=scoped.value;const bool all=flags&4u;bool owner_hit{};
    if(owner.family==0x40004000){auto archive=decode_vector_ruler_archive(owner);auto rulers=index(archive.rulers);auto lines=index(archive.base.lines);vector_probe_detail::Graph graph(archive.base,r.budget());
        for(auto& plane:owner.planes){r.budget().charge();if(!eligible(plane,one))continue;if(plane.opaque||plane.raster||plane.family!=0x40000001)throw std::invalid_argument("Unsupported indication Ruler plane");bool hit{};
            for(int id:vector_extension_plane_references(plane).extensions){r.budget().charge();auto& ruler=live(rulers,id);
                if(!scope.configured||graph.line_matches(live(lines,ruler.line),scope,all,r.budget())){ruler.flags|=0x10000u;hit=true;}}
            if(hit){plane.common.flags[3]=1;owner.common.flags[3]=1;owner_hit=true;}}
        owner.specific=encode_vector_ruler_archive(archive);
    }else{auto archive=decode_vector_shape_archive(owner);auto shapes=index(archive.shapes);auto lines=index(archive.paint.base.lines);auto regions=index(archive.paint.regions);auto faces=index(archive.paint.faces);
        vector_probe_detail::PaintGraph graph(archive.paint,r.budget());TopologyDpis region_dpis;
        if(scope.mask){add_topology(region_dpis,archive.paint.border.regions,archive.paint.border.common.resolution,r);
            for(const auto& plane:owner.planes)if(plane.family==0x40000002)add_topology(region_dpis,vector_extension_plane_references(plane).regions,plane.common.resolution,r);}
        for(auto& plane:owner.planes){r.budget().charge();if(!eligible(plane,one))continue;if(plane.opaque||plane.raster||plane.family!=0x40000002)throw std::invalid_argument("Unsupported indication Shape plane");bool hit{};
            for(int id:vector_extension_plane_references(plane).extensions){r.budget().charge();auto& shape=live(shapes,id);int region=-1;
                if(shape.region>=0){const auto& value=live(regions,shape.region);if(!value.faces.empty()&&value.faces.front()>=0)region=live(faces,value.faces.front()).region;}
                const double raw_dpi=scope.mask&&region>=0?topology_dpi(region_dpis,region,r):0;
                const bool selected=region>=0?graph.matches(live(regions,region),scope,all,raw_dpi,r.storage,r.budget(),scope.mask?raw_dpi/carrier_dpi:0):shape.line>=0&&graph.lines.line_matches(live(lines,shape.line),scope,all,r.budget());
                if(selected){shape.flags|=0x10000u;hit=true;}}
            if(hit){plane.common.flags[3]=1;owner.common.flags[3]=1;owner_hit=true;}}
        store_vector_shape_archive(owner,archive);
    }
    return owner_hit;
}
bool refresh(DocumentData& data,bool active,const RegionSelection* selection,std::uint32_t flags,double carrier_dpi,Resources& resources){
    clear(data,active,resources);bool hit_any{};
    if(!selection||selection->state==SelectionLayerState::Cleared||(flags&1u)){resources.poll();return false;}
    const auto count=std::ranges::count_if(data.layers,[](const Layer& l){return l.common.flags[2]&&l.common.flags[1]&&!l.common.flags[0];});
    for(auto& owner:data.layers){resources.budget().charge();if(!owner.common.flags[2]||!owner.common.flags[1]||owner.common.flags[0]||!owner.common.runtime_operable)continue;
        bool hit{};
        if((owner.family&0x20000000u)&&(flags&0x300u)){if(!ordinary(owner))throw std::invalid_argument("Unsupported indication vector owner");hit=refresh_ordinary(owner,*selection,data,flags,carrier_dpi,count==1,resources);}
        else if(extension(owner)&&(flags&0x400u))hit=refresh_extension(owner,*selection,data,flags,carrier_dpi,count==1,resources);
        if(hit)hit_any=true;
    }
    resources.poll();return hit_any;
}
VectorIndicationUpdate prepare(const DocumentData& input,bool active,const RegionSelection* selection,std::uint32_t flags,double carrier_dpi,std::stop_token stop){
    Resources resources{0,0,stop};resources.poll();std::uint64_t metadata{};modal_detail::data_charge(input,metadata);resources.bytes(metadata*3);resources.budget().charge(metadata);
    VectorIndicationUpdate result{input,false,active};result.active=refresh(result.data,active,selection,flags,carrier_dpi,resources);
    result.redraw=result.redraw||result.active;return result;
}
}
void vector_indication_detail::clear_marks(DocumentData& data,bool active,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
    Resources resources{storage,work,stop};clear(data,active,resources);
    storage=resources.storage;work=resources.work;
}
bool vector_indication_detail::refresh_marks(DocumentData& data,bool active,const RegionSelection& selection,std::uint32_t flags,double carrier_dpi,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
    Resources resources{storage,work,stop};const bool result=refresh(data,active,&selection,flags,carrier_dpi,resources);
    storage=resources.storage;work=resources.work;return result;
}
VectorIndicationUpdate prepare_vector_indication_clear(const DocumentData& input,bool active,std::stop_token stop){return prepare(input,active,nullptr,0,0,stop);}
VectorIndicationUpdate prepare_vector_indication_refresh(const DocumentData& input,bool active,const RegionSelection& selection,std::uint32_t flags,double carrier_dpi,std::stop_token stop){return prepare(input,active,&selection,flags,carrier_dpi,stop);}
}
