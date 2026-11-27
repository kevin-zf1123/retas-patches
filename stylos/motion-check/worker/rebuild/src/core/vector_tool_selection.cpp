#include "vector_native_mask_scope.hpp"
#include "vector_tool_selection.hpp"
#include "vector_mask_region.hpp"
#include "vector_mask_contour.hpp"
#include "vector_fill_boundary.hpp"
#include "retas/fill.hpp"
#include "retas/vector_color_metadata.hpp"
#include <set>

namespace retas::vector_selection_detail {
using vector_boundary_detail::Integrator;
using vector_graph_detail::Editor;
using vector_probe_detail::Budget;
using vector_material_detail::charge;
namespace {
constexpr std::uint64_t black=0xffff000000000000ull;
bool paint(const Plane& p){return !p.opaque&&(p.family==0x22100001||p.family==0x22100002);}
template<class T>const T& get(const std::vector<VectorPoolRecord<T>>& pool,int id,Budget b){b.charge(pool.size()+1);const auto i=std::ranges::find(pool,id,&VectorPoolRecord<T>::index);if(i==pool.end())throw std::invalid_argument("Selection bridge dead identity");return i->value;}
int raw(std::int64_t n){if(std::abs(n)>134217727)throw std::invalid_argument("Selection bridge exceeds bounded raw domain");return int(n);}
VectorBounds source_bounds(const VectorPaintArchive& a,Budget b){
    //643E60/5A1B70: FIRST empty embedded outer, then saved hole union.
    for(int id:a.border.regions){const auto& r=get(a.regions,id,b);if(!r.outer.members.empty())continue;VectorBounds out;for(int hole:r.holes)vector_graph_detail::unite(out,get(a.loops,hole,b).archived_bounds);return out;}return {};
}
VectorBounds selected_bounds(const VectorPaintArchive& a,const std::vector<int>& selected,Budget b){VectorBounds out;for(int id:selected)vector_graph_detail::unite(out,get(a.regions,id,b).outer.archived_bounds);return out;}
struct Counts {std::uint64_t wire{},nodes{},refs{};};
Counts counts(const VectorPaintArchive& a,const Layer& owner,std::uint64_t& storage,Budget b){
    Counts out;const auto& g=a.base;out.nodes=g.vertices.size()+g.scales.size()+g.subcurves.size()+g.models.size()+g.crosspoints.size()+g.lines.size()+a.loops.size()+a.regions.size()+a.faces.size()+a.gradnodes.size()+a.gradations.size()+a.textures.size();
    if(out.nodes>4096||owner.planes.size()>1024)throw std::length_error("Selection snapshot record/plane limit");
    out.refs=a.border.vertices.size()+a.border.models.size()+a.border.regions.size();for(const auto& v:g.vertices)out.refs+=v.value.subcurves.size();for(const auto& s:g.subcurves)out.refs+=s.value.scales.size();for(const auto& m:g.models){out.refs+=m.value.subcurves.size();for(int id:m.value.crosspoints)out.refs+=id>=0;}for(const auto& l:g.lines)out.refs+=l.value.members.size();for(const auto& l:a.loops)out.refs+=l.value.members.size();for(const auto& r:a.regions)out.refs+=r.value.outer.members.size()+r.value.holes.size()+r.value.faces.size();
    for(const auto& p:owner.planes){if(p.specific.size()>8ull*1024*1024)throw std::length_error("Selection child wire limit");
        // Header-only counting: PaintLists::tail/rest would allocate opaque
        // suffix bytes before the aggregate snapshot budget had been checked.
        if(paint(p)||p.family==0x21110001||p.family==0x21120001){Reader r(p.specific);for(int lane=0;lane<(paint(p)?3:1);++lane){const auto n=r.i32();r.require_count(n,4,8192);out.refs+=std::uint64_t(n);r.take(std::size_t(n)*4);}}
    }
    if(out.refs>8192)throw std::length_error("Selection snapshot reference limit");
    // Bound all variable source data BEFORE encoding/snapshot allocation.
    // Fixed record/collection envelopes deliberately use an upper bound.
    std::uint64_t estimated=8192+256*out.nodes+16*out.refs;
    const auto add=[&](std::uint64_t n){if(n>8ull*1024*1024-estimated)throw std::length_error("Selection snapshot wire limit");estimated+=n;};
    const auto suffixes=[&](const auto& pool){for(const auto& r:pool){b.charge();add(r.suffix.size());}};
    suffixes(g.vertices);suffixes(g.scales);suffixes(g.subcurves);suffixes(g.models);suffixes(g.crosspoints);suffixes(g.lines);suffixes(a.loops);suffixes(a.regions);suffixes(a.faces);suffixes(a.gradnodes);suffixes(a.gradations);suffixes(a.textures);
    for(const auto& r:a.loops)add(r.value.suffix.size());for(const auto& r:a.regions)add(r.value.outer.suffix.size());
    for(const auto* pool:{&a.gradnodes,&a.gradations,&a.textures})for(const auto& r:*pool)add(r.value.size());
    for(const auto* bytes:{&g.header_suffix,&g.body_suffix,&g.layer_suffix,&a.face_header_suffix,&a.face_body_suffix,&a.core_suffix,&a.outer_suffix,&a.border.topology_suffix,&a.border.specific_suffix,&a.border.record_suffix,&a.border.common.name,&a.border.common.suffix,&owner.common.name,&owner.common.suffix,&owner.specific,&owner.suffix,&owner.plane_suffix,&owner.opaque_record})add(bytes->size());
    for(const auto& p:owner.planes){add(p.specific.size());add(p.suffix.size());add(p.opaque_record.size());add(p.common.name.size());add(p.common.suffix.size());}
    b.charge(4*estimated+32);charge(storage,4*estimated+4096,512ull*1024*1024,"Selection snapshot storage exceeds512MiB");
    const auto encoded=encode_vector_paint_archive(a);out.wire=encoded.size();for(const auto& p:owner.planes)out.wire+=p.specific.size();
    if(out.wire>8ull*1024*1024)throw std::length_error("Selection snapshot wire limit");return out;
}
void paint_map(const Layer& source,const std::map<std::size_t,std::size_t>& planes,Integrator& target){for(auto [from,to]:planes){target.geometry().budget.charge();if(from>=source.planes.size()||to>=target.geometry().layer.planes.size()||!paint(source.planes[from])||!paint(target.geometry().layer.planes[to]))throw std::invalid_argument("Selection fill bridge requires actual Paint child mappings");}}
VectorBounds all_copy(Integrator& target,const VectorPaintArchive& a,const Layer& owner,
    const std::map<std::size_t,std::size_t>& planes,std::optional<vector_copy_detail::Matrix> matrix,std::vector<int>* selected){
    auto& e=target.geometry();VectorBounds dirty=vector_copy_detail::bounds(source_bounds(a,e.budget),matrix);auto affected=vector_boundary_detail::query_candidates(target,dirty);
    vector_boundary_detail::SourceCopySettings settings;settings.planes=planes;settings.matrix=matrix;if(selected)settings.selected=*selected;
    const auto copied=vector_boundary_detail::copy_source(target,a,owner,settings);if(selected)*selected=copied.selected_regions;
    if(!affected.regions.empty()){auto repair=vector_boundary_detail::run(e,vector_boundary_detail::source_pointer_ordered_models(copied),affected.models,affected.regions,target.line_planes(),target.face_planes());vector_graph_detail::unite(dirty,target.commit_regions(repair,copied.new_regions,affected.regions,selected==nullptr,selected));}
    target.merge_exteriors_once(dirty);target.publish();return dirty;
}
void rect_regions(Integrator& c){auto& e=c.geometry();const auto models=e.owner.models;if(models.size()!=4)throw std::invalid_argument("Selection rectangle lacks four native edges");const int id=c.create_region();std::vector<VectorLineMember> main,back;for(int model:models)main.push_back({model,1});for(auto i=models.rbegin();i!=models.rend();++i)back.push_back({*i,0});c.populate_loop(true,id,main);c.populate_loop(false,c.create_hole(0),back);c.create_face(0,id,{id,1,black,-1});c.publish();}
}
struct FrozenNativeSelection::Data {
    bool bypass{},available{true};VectorPaintArchive archive;Layer owner;std::array<int,2> origin{};
    std::vector<int> selected;std::optional<vector_mask_detail::TemporaryLineCleanup> temporary_lines;
};
FrozenNativeSelection::FrozenNativeSelection(std::shared_ptr<const Data> p):data_(std::move(p)){}
bool FrozenNativeSelection::bypass()const{return data_->bypass;}
bool FrozenNativeSelection::available()const{return data_->available;}
const VectorPaintArchive& FrozenNativeSelection::archive()const{return data_->archive;}
const Layer& FrozenNativeSelection::owner()const{return data_->owner;}
std::array<int,2> FrozenNativeSelection::origin()const{return data_->origin;}
const std::vector<int>& FrozenNativeSelection::selected_regions()const{return data_->selected;}
FrozenNativeSelection freeze_cache(const VectorPaintArchive& a,const Layer& owner,std::array<int,2> origin,std::uint64_t& storage,Budget b){
    b.charge();if(owner.opaque||owner.family!=0x20800000||owner.planes.size()!=1||owner.planes[0].family!=0x22100001||!paint(owner.planes[0])||!a.base.lines.empty())throw std::invalid_argument("Selection cache requires native208 Paint0-only owner");
    if(!std::isfinite(owner.common.resolution)||owner.common.resolution<=0||!std::isfinite(a.border.common.resolution)||a.border.common.resolution<=0||a.border.common.resolution!=numeric_detail::binary64(owner.common.resolution*16)||owner.planes[0].common.resolution!=a.border.common.resolution)throw std::invalid_argument("Selection cache density differs from native factory");
    (void)counts(a,owner,storage,b);vector_fill_detail::validate_paint_owners(a,owner,b);vector_material_detail::PaintLists lists(owner.planes[0]);if(lists.ids[0].size()!=a.faces.size())throw std::invalid_argument("Selection cache has unowned Face identities");if(!lists.ids[1].empty()||!lists.ids[2].empty()||!a.gradnodes.empty()||!a.gradations.empty()||!a.textures.empty())throw std::invalid_argument("Selection cache has non-indication materials");
    auto out=std::make_shared<FrozenNativeSelection::Data>();out->origin=origin;
    for(int id:lists.ids[0]){const auto& face=get(a.faces,id,b);if((face.flags&15)!=1||face.color!=black)throw std::invalid_argument("Selection cache Face differs from black indication");(void)get(a.regions,face.region,b);out->selected.push_back(face.region);}
    for(const auto& mc:a.base.models){b.charge();if(mc.value.line>=0)throw std::invalid_argument("Selection cache retained a borrowed LC backlink");}
    // Full typed graph grammar and side reciprocity, without requiring the
    // renderer's stricter simple-curve certificate for transient native loops.
    Layer decoded=owner;store_vector_paint_archive(decoded,a);out->archive=decode_vector_paint_archive(decoded);out->owner=owner;out->owner.specific.clear();return FrozenNativeSelection(std::move(out));
}
FrozenNativeSelection prepare(const RegionSelection& selection,const CommonElement& document,double mask_dpi,std::uint64_t& storage,Budget b){
    b.charge();if(selection.state==SelectionLayerState::Cleared||selection.state==SelectionLayerState::FullPlane){auto out=std::make_shared<FrozenNativeSelection::Data>();out->bypass=true;return FrozenNativeSelection(std::move(out));}
    if(selection.state==SelectionLayerState::VectorIndicated){
        if(!selection.vector_cache)throw std::invalid_argument("State4 requires its actual cached vector selection packet");
        const auto& cache=*selection.vector_cache;
        std::uint64_t bytes=cache.specific.size();if(cache.planes.size()>1024)throw std::length_error("State4 cache child limit");
        for(const auto& p:cache.planes)charge(bytes,p.specific.size(),8ull*1024*1024,"State4 cache wire limit");
        charge(bytes,0,8ull*1024*1024,"State4 cache wire limit");charge(storage,bytes*16+4096,512ull*1024*1024,"State4 cache decode storage limit");b.charge(bytes*8);
        return freeze_cache(decode_vector_paint_archive(cache),cache,selection.vector_cache_origin,storage,b);
    }
    const double geometry_dpi=numeric_detail::binary64(document.resolution*16);if(!std::isfinite(document.resolution)||document.resolution<=0||!std::isfinite(geometry_dpi)||geometry_dpi<=0)throw std::invalid_argument("Selection document/native geometry DPI is invalid");
    vector_probe_detail::Scope scope;
    if(selection.state==SelectionLayerState::Rectangle){const auto& r=selection.bounds;if(r.width<0||r.height<0)throw std::invalid_argument("Selection rectangle has negative extent");scope=vector_probe_detail::Scope::rectangle({raw(std::int64_t(r.x)*16),raw(std::int64_t(r.y)*16),raw((std::int64_t(r.x)+r.width)*16),raw((std::int64_t(r.y)+r.height)*16)});}
    else if(selection.state==SelectionLayerState::Mask){if(!selection.mask||!std::isfinite(mask_dpi)||mask_dpi<=0)throw std::invalid_argument("Selection mask requires actual backing and DPI");if(selection.carrier_size!=std::array<int,2>{}&&selection.carrier_size!=std::array<int,2>{selection.mask->width(),selection.mask->height()})throw std::invalid_argument("Selection mask carrier metadata differs from actual backing");const double scale=numeric_detail::binary64(numeric_detail::binary64(document.resolution/mask_dpi)*16);scope=vector_menu_scope_detail::mask(*selection.mask,{0,0},selection.bounds,b,scale);}
    else throw std::invalid_argument("Unknown native selection state");
    auto traced=vector_mask_detail::trace(scope,storage,b);auto out=std::make_shared<FrozenNativeSelection::Data>();out->origin=selection.origin;auto& a=out->archive;auto& owner=out->owner;
    owner.family=0x20800000;owner.common=document;owner.common.depth=1;owner.current_plane=0;
    a.base=std::move(traced.graph);a.face_high_water.fill(-1);a.border.common=owner.common;a.border.common.resolution=geometry_dpi;a.border.common.width=a.border.common.height=a.border.common.depth=0;a.border.vertices=std::move(traced.vertices);a.border.models=std::move(traced.models);
    Plane p,l;p.family=0x22100001;p.common=a.border.common;Writer w;for(int i=0;i<3;++i)w.i32(0);p.specific=w.data;l.family=0x21110001;l.common=p.common;Writer lines;lines.i32(int(traced.lines.size()));for(int id:traced.lines)lines.i32(id);l.specific=lines.data;owner.planes={p,l};
    auto n=counts(a,owner,storage,b);Editor e(a.base,&a,owner,vector_graph_detail::BorderOwnerTag{},n.wire,n.nodes,n.refs,storage,b.work,b.stop);e.owner.lines=traced.lines;Integrator c(e);if(c.create_region()!=0)throw std::invalid_argument("Selection factory exterior is not zero");
    if(!a.base.models.empty()){if(traced.rectangle)rect_regions(c);else{std::map<int,std::size_t> planes;for(int id:traced.lines)planes.emplace(id,1);auto converted=vector_mask_detail::materialize_samples(c,traced.lines,planes,0,black);out->temporary_lines=std::move(converted.temporary_lines);}}
    c.publish();if(!a.base.lines.empty()||!vector_line_plane_references(owner.planes[1]).empty())throw std::invalid_argument("Selection conversion retained temporary Line ownership");owner.planes.pop_back();vector_material_detail::PaintLists lists(owner.planes[0]);for(int id:lists.ids[0])out->selected.push_back(get(a.faces,id,b).region);out->available=!out->selected.empty();b.charge();return FrozenNativeSelection(std::move(out));
}
GradationTransferResult gradation_transfer(Integrator& target,const FrozenNativeSelection& selection,std::array<int,2> target_origin){
    auto& e=target.geometry();e.budget.charge();if(selection.bypass())throw std::invalid_argument("GRAD selected converter requires actual native208 packet");
    GradationTransferResult result;if(!selection.available())return result;
    auto from=selection.owner().common,to=e.layer.common;from.x=selection.origin()[0];from.y=selection.origin()[1];to.x=target_origin[0];to.y=target_origin[1];
    const auto matrix=vector_copy_detail::between(from,selection.archive().border.common.resolution,to,target.paint().border.common.resolution);
    result.selected_regions=selection.selected_regions();result.converter_dirty=all_copy(target,selection.archive(),selection.owner(),{},matrix,&result.selected_regions);
    return result;
}
GradationTransferResult transfer_regions(Integrator& target,const VectorPaintArchive& a,const Layer& owner,std::vector<int> selected){
    GradationTransferResult result;result.selected_regions=std::move(selected);
    //5DBBCA supplies RCPoint(0,0);5DB81D consequently passes a NULL
    //matrix. The mask tracer already produced this caller's raw coordinates.
    result.converter_dirty=all_copy(target,a,owner,{},std::nullopt,&result.selected_regions);return result;
}
VectorBounds transfer_all(Integrator& target,const VectorPaintArchive& a,const Layer& owner,const std::map<std::size_t,std::size_t>& planes){return all_copy(target,a,owner,planes,planes.empty()?vector_copy_detail::between(owner.common,a.border.common.resolution,target.geometry().layer.common,target.paint().border.common.resolution):vector_copy_detail::between_planes(owner,target.geometry().layer,planes),nullptr);}
TransferResult transfer(Integrator& target,const VectorPaintArchive& generated,const Layer& owner,const FrozenNativeSelection& selection,std::array<int,2> generated_origin,const std::map<std::size_t,std::size_t>& planes){
    auto& e=target.geometry();e.budget.charge();if(!selection.available())return {};paint_map(owner,planes,target);if(&generated==&target.paint())throw std::invalid_argument("Selection transfer requires a distinct immutable generated graph");TransferResult result;const bool changed=e.topology_changed;
    if(selection.bypass()){result.converter_dirty=transfer_all(target,generated,owner,planes);result.storage_changed=e.topology_changed!=changed||!generated.border.regions.empty();return result;}
    // Private immutable generated snapshot: selection promotion never aliases
    // or clears the caller's WorkLayer. This is explicitly a modern boundary.
    auto n=counts(generated,owner,e.storage,e.budget);VectorPaintArchive scratch=generated;Layer scratch_owner=owner;Editor edit(scratch.base,&scratch,scratch_owner,vector_graph_detail::BorderOwnerTag{},n.wire,n.nodes,n.refs,e.storage,e.budget.work,e.budget.stop);
    {
        Integrator c(edit);result.selected_regions=selection.selected_regions();
        auto from=selection.owner().common,to=owner.common;from.x=selection.origin()[0];from.y=selection.origin()[1];to.x=generated_origin[0];to.y=generated_origin[1];
        const auto matrix=vector_copy_detail::between(from,selection.archive().border.common.resolution,to,generated.border.common.resolution);
        result.first_copy_dirty=all_copy(c,selection.archive(),selection.owner(),{},matrix,&result.selected_regions);
    }
    const auto matrix=planes.empty()?vector_copy_detail::between(scratch_owner.common,scratch.border.common.resolution,e.layer.common,target.paint().border.common.resolution):vector_copy_detail::between_planes(scratch_owner,e.layer,planes);
    result.converter_dirty=vector_copy_detail::bounds(selected_bounds(scratch,result.selected_regions,e.budget),matrix);auto affected=vector_boundary_detail::query_candidates(target,result.converter_dirty);
    vector_boundary_detail::SourceCopySettings settings;settings.regions=result.selected_regions;settings.planes=planes;settings.matrix=matrix;auto copied=vector_boundary_detail::copy_source(target,scratch,scratch_owner,settings);
    auto models=vector_boundary_detail::source_pointer_ordered_models(copied);std::vector<int> missing;for(int id:models){const auto& mc=e.model(id);if(mc.targets[0]<0||mc.targets[1]<0)missing.push_back(id);}const auto missing_dirty=target.complete_missing_sides(missing,copied.new_regions);
    //5D4770 rerun1: NONNULL collector, before affected gate. Returned Regions
    // can differ from copied seeds; related identities suppress stale entries.
    result.prepass_dirty=missing_dirty;vector_graph_detail::unite(result.prepass_dirty,target.materialize_regions(copied.new_regions,&result.prepass_regions));
    if(!affected.regions.empty()){
        std::vector<int> live;for(int id:models){e.budget.charge();const auto i=std::ranges::find(e.a.models,id,&VectorPoolRecord<VectorModelCurve>::index);if(i!=e.a.models.end()&&!i->value.subcurves.empty())live.push_back(id);}
        auto repair=vector_boundary_detail::run(e,live,affected.models,affected.regions,target.line_planes(),target.face_planes());vector_graph_detail::unite(repair.context_dirty,result.prepass_dirty);vector_graph_detail::unite(result.converter_dirty,target.commit_regions(repair,result.prepass_regions,affected.regions,true));
    }
    //No unconditional prepass dirty union when affected is empty (5D4770).
    target.merge_exteriors_once(result.converter_dirty);target.publish();result.storage_changed=e.topology_changed!=changed||!copied.new_regions.empty();e.budget.charge();return result;
}
}
