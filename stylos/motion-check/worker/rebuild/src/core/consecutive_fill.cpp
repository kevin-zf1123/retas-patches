#include "retas/consecutive_fill.hpp"
#include "retas/numeric.hpp"
#include "retas/vector_face_archive.hpp"
#include "retas/vector_color_metadata.hpp"
#include "vector_edit_profile.hpp"
#include "vector_material_edit.hpp"
#include "vector_fill_boundary.hpp"
#include "vector_boundary_gap.hpp"
#include "vector_boundary_source_copy.hpp"
#include "vector_tool_selection.hpp"
#include "vector_tool_selection_binding.hpp"
#include "vector_region_probe.hpp"
#include "adjust_render.hpp"
#include <algorithm>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>

namespace retas {
namespace {
constexpr std::uint64_t max_bytes=512ull*1024*1024,max_work=128ull*1024*1024;
struct Cancelled {};
void poll(std::stop_token stop){if(stop.stop_requested())throw Cancelled{};}
void charge(std::uint64_t& count,std::uint64_t value,std::uint64_t limit,const char* message){
    if(value>limit||count>limit-value)throw std::length_error(message);count+=value;
}
void bytes(std::uint64_t& count,const Bytes& value){charge(count,value.size(),max_bytes,"BFIL source exceeds512MiB");}
bool same_double(double a,double b){return std::bit_cast<std::uint64_t>(a)==std::bit_cast<std::uint64_t>(b);}
template<std::size_t N> bool same_lengths(const std::array<Length,N>& a,const std::array<Length,N>& b){
    for(std::size_t i=0;i<N;++i)if(a[i].unit!=b[i].unit||!same_double(a[i].value,b[i].value)||!same_double(a[i].reference,b[i].reference))return false;
    return true;
}
bool same_common(const CommonElement& a,const CommonElement& b){return a.width==b.width&&a.height==b.height&&a.depth==b.depth&&
    same_double(a.resolution,b.resolution)&&a.name==b.name&&a.flags==b.flags&&a.color==b.color&&a.x==b.x&&a.y==b.y&&
    same_lengths(a.position,b.position)&&a.opacity==b.opacity&&a.opacity_mode==b.opacity_mode&&a.field_14==b.field_14&&a.suffix==b.suffix;}
bool same_raster(const std::optional<Raster>& a,const std::optional<Raster>& b){
    if(bool(a)!=bool(b))return false;if(!a)return true;
    return a->width==b->width&&a->height==b->height&&a->depth==b->depth&&same_double(a->resolution,b->resolution)&&a->color_order==b->color_order&&a->tiles==b->tiles;
}
bool same_data(const DocumentData& a,const DocumentData& b){
    if(a.version!=b.version||a.dga!=b.dga||a.current_layer!=b.current_layer||a.layers.size()!=b.layers.size()||
       !same_lengths(a.base.lengths,b.base.lengths)||a.base.frame_mode!=b.base.frame_mode||!same_double(a.base.scale,b.base.scale)||
       !same_double(a.base.dpi,b.base.dpi)||!same_double(a.base.dialog_dpi,b.base.dialog_dpi)||a.base.flags!=b.base.flags||a.base.suffix!=b.base.suffix||
       a.preview!=b.preview||a.layer_suffix!=b.layer_suffix||a.trailing!=b.trailing||a.original!=b.original||a.unrestored_external_content!=b.unrestored_external_content)return false;
    for(std::size_t i=0;i<a.layers.size();++i){const auto& x=a.layers[i];const auto& y=b.layers[i];
        if(x.opaque!=y.opaque||x.family!=y.family||!same_common(x.common,y.common)||x.specific!=y.specific||x.suffix!=y.suffix||
           x.plane_suffix!=y.plane_suffix||x.opaque_record!=y.opaque_record||x.current_plane!=y.current_plane||x.planes.size()!=y.planes.size()||x.visibility_modified!=y.visibility_modified||x.runtime_vector_high_water!=y.runtime_vector_high_water||!vector_runtime_detail::face_colors_equal(x,y))return false;
        for(std::size_t j=0;j<x.planes.size();++j){const auto& p=x.planes[j];const auto& q=y.planes[j];
            if(p.opaque!=q.opaque||p.family!=q.family||p.runtime_adjust_cache!=q.runtime_adjust_cache||!same_common(p.common,q.common)||p.specific!=q.specific||p.suffix!=q.suffix||
               p.opaque_record!=q.opaque_record||!same_raster(p.raster,q.raster)||p.pixels_changed!=q.pixels_changed||p.visibility_modified!=q.visibility_modified)return false;
        }
    }return true;
}
void common_bytes(std::uint64_t& count,const CommonElement& common){bytes(count,common.name);bytes(count,common.suffix);}
void raster_bytes(std::uint64_t& count,const Raster& raster,std::stop_token stop){
    charge(count,sizeof(Raster),max_bytes,"BFIL source exceeds512MiB");
    for(const auto& [coordinate,tile]:raster.tiles){(void)coordinate;poll(stop);if(!tile)throw std::invalid_argument("BFIL null raster tile");
        charge(count,sizeof(TileCoord)+sizeof(std::shared_ptr<Bytes>)+48,max_bytes,"BFIL source exceeds512MiB");bytes(count,*tile);}
}
std::uint64_t source_bytes(const DocumentData& data,const ConsecutiveFillItem& item,std::stop_token stop){
    if(item.rows.size()>consecutive_fill_max_rows)throw std::length_error("BFIL row count exceeds modern limit");
    std::uint64_t count=sizeof(DocumentData)+sizeof(ConsecutiveFillItem);
    charge(count,item.rows.size()*sizeof(ConsecutiveFillRow),max_bytes,"BFIL source exceeds512MiB");
    bytes(count,data.base.suffix);bytes(count,data.preview);bytes(count,data.layer_suffix);bytes(count,data.trailing);if(data.original)bytes(count,*data.original);
    for(const auto& layer:data.layers){poll(stop);charge(count,sizeof(Layer)+vector_runtime_detail::face_colors_storage(layer),max_bytes,"BFIL source exceeds512MiB");common_bytes(count,layer.common);
        bytes(count,layer.specific);bytes(count,layer.suffix);bytes(count,layer.plane_suffix);bytes(count,layer.opaque_record);
        for(const auto& plane:layer.planes){poll(stop);charge(count,sizeof(Plane),max_bytes,"BFIL source exceeds512MiB");common_bytes(count,plane.common);
            bytes(count,plane.specific);bytes(count,plane.suffix);bytes(count,plane.opaque_record);charge(count,adjust_render_detail::cache_storage(plane),max_bytes,"BFIL Adjust cache exceeds512MiB");if(plane.raster)raster_bytes(count,*plane.raster,stop);}
    }return count;
}
bool eligible(const Layer& layer,const ConsecutiveFillItem& item){
    if(!layer.common.flags[1])return false;
    for(std::size_t i=0;i<consecutive_fill_families.size();++i)
        if(layer.family==consecutive_fill_families[i]&&item.layers[i].select&&item.layers[i].check)return true;
    return false;
}
std::optional<std::size_t> target_plane(const Layer& layer){
    const std::uint32_t mask=(layer.family&0x01000000)?0x02000000:0x10000001;
    std::optional<std::size_t> result;
    for(std::size_t i=0;i<layer.planes.size();++i)if(layer.planes[i].family&mask)result=i;
    return result;
}
const Plane* companion(const Layer& layer,const Plane& target){
    const std::uint32_t family=(target.family==0x12100001||target.family==0x12400001)
        ?(layer.family==0x11000800?0x11110002:layer.family==0x10040000?0x11410001:0):0;
    if(family)for(const auto& plane:layer.planes)if(plane.family==family)return plane.common.flags[1]?&plane:nullptr;
    return nullptr;
}
void validate_raster(const Raster& r,std::stop_token stop,bool read_only=false){
    if((!r.can_edit()&&!(read_only&&r.depth==1&&r.color_order==0))||r.width<=0||r.height<=0||r.width>100000||r.height>100000||!std::isfinite(r.resolution)||r.resolution<=0)
        throw std::invalid_argument("BFIL requires bounded normal32/64 raster");
    for(const auto& [c,t]:r.tiles){poll(stop);if(c.x<0||c.y<0||c.x>(r.width-1)/256||c.y>(r.height-1)/256||!t||t->size()!=r.tile_bytes())
        throw std::invalid_argument("BFIL malformed physical raster storage");}
}
void validate_target(const Layer& layer,const Plane& plane,std::stop_token stop){
    if(layer.opaque||(layer.family&0x60000000)||plane.opaque||(plane.family&0xf0000000)!=0x10000000||!plane.raster||plane.common.opacity_mode!=1)
        throw std::invalid_argument("BFIL vector, opaque or nonnormal target is unsupported");
    validate_raster(*plane.raster,stop);
    if(!std::isfinite(layer.common.resolution)||layer.common.resolution<=0||plane.common.resolution!=layer.common.resolution||
       plane.raster->resolution!=plane.common.resolution||plane.common.width!=plane.raster->width||plane.common.height!=plane.raster->height||plane.common.depth!=plane.raster->depth)
        throw std::invalid_argument("BFIL requires matching physical metadata and same-DPI target");
    if(const auto* p=companion(layer,plane)){
        if(p->opaque||!p->raster)throw std::invalid_argument("BFIL opaque line companion is unsupported");validate_raster(*p->raster,stop,true);
        if(p->common.resolution!=layer.common.resolution||p->raster->resolution!=layer.common.resolution||p->raster->width!=plane.raster->width||p->raster->height!=plane.raster->height)
            throw std::invalid_argument("BFIL companion requires same-DPI physical size");
    }
}
int coordinate(std::int32_t document,int origin){const auto value=std::int64_t(document)-origin;
    if(value<std::numeric_limits<int>::min()||value>std::numeric_limits<int>::max())throw std::invalid_argument("BFIL coordinate subtraction exceeds modern integer domain");return int(value);}
Rect document_rect(Rect rect,const CommonElement& owner){const auto x=std::int64_t(rect.x)+owner.x,y=std::int64_t(rect.y)+owner.y;
    if(x<std::numeric_limits<int>::min()||y<std::numeric_limits<int>::min()||x+rect.width>std::numeric_limits<int>::max()||y+rect.height>std::numeric_limits<int>::max())
        throw std::invalid_argument("BFIL document damage exceeds modern integer domain");
    return {int(x),int(y),rect.width,rect.height};}
Pixel packed(std::uint64_t color){return {std::uint16_t(color),std::uint16_t(color>>16),std::uint16_t(color>>32),std::uint16_t(color>>48)};}
void validate_reference(const FillReference& ref,double dpi,std::uint64_t& count,std::stop_token stop){
    if(!ref.raster||!std::ranges::all_of(ref.target_to_reference,[](double v){return std::isfinite(v);}))throw std::invalid_argument("BFIL malformed prepared Light reference");
    validate_raster(*ref.raster,stop,true);if(ref.raster->resolution!=dpi)throw std::invalid_argument("BFIL mixed-DPI Light reference is unsupported");raster_bytes(count,*ref.raster,stop);
}
std::optional<Pixel> sample(const FillReference& ref,int x,int y){const auto& t=ref.target_to_reference;
    const auto px=t[0]*x+t[1]*y+t[2],py=t[3]*x+t[4]*y+t[5];
    if(!std::isfinite(px)||!std::isfinite(py)||px<0||py<0||px>=ref.raster->width||py>=ref.raster->height)return {};
    const auto p=ref.raster->pixel(int(std::floor(px)),int(std::floor(py)));return p.a?std::optional<Pixel>{p}:std::nullopt;
}
void work_budget(const Raster& raster,const BucketFillSettings& settings,std::size_t lines,std::uint64_t& work){
    const auto p=std::uint64_t(raster.width)*raster.height;
    std::uint64_t scratch{};charge(scratch,p*128,max_bytes,"BFIL scratch bound exceeds512MiB");
    std::uint64_t units=128+16*lines;
    if(settings.close_gap_on&&settings.close_gap_distance_px){const auto gap=std::uint64_t(settings.close_gap_distance_px);
        // Existing closure traces nearest endpoint lists. Quadratic accounting
        // bounds that path, rather than assuming every flood is linear.
        units+=64*p+16*gap*gap+16*gap;
    }
    if(units>max_work||p>max_work/units)throw std::length_error("BFIL work exceeds128Mi units");
    charge(work,p*units,max_work,"BFIL total batch work exceeds128Mi units");
}
void merge_damage(ConsecutiveFillResult& result,std::size_t layer,std::size_t plane,Rect dirty,const CommonElement& owner){
    auto found=std::ranges::find_if(result.damage,[&](const auto& d){return d.layer==layer&&d.plane==plane;});
    if(found==result.damage.end())result.damage.push_back({layer,plane,dirty,document_rect(dirty,owner)});
    else{const int left=std::min(found->local.x,dirty.x),top=std::min(found->local.y,dirty.y);
        const int right=std::max(found->local.x+found->local.width,dirty.x+dirty.width),bottom=std::max(found->local.y+found->local.height,dirty.y+dirty.height);
        found->local={left,top,right-left,bottom-top};found->document=document_rect(found->local,owner);}
}
template<class T> auto pool_index(std::vector<VectorPoolRecord<T>>& pool){
    std::map<int,T*> result;for(auto& record:pool)result.emplace(record.index,&record.value);return result;
}
Rect vector_dirty(VectorBounds raw){
    // Native 61F9A0 truncates each raw edge independently, before width/height.
    const auto left=raw.left/16,top=raw.top/16,right=raw.right/16,bottom=raw.bottom/16;
    const auto width=std::int64_t(right)-left,height=std::int64_t(bottom)-top;
    if(width>std::numeric_limits<int>::max()||height>std::numeric_limits<int>::max())throw std::invalid_argument("BFIL vector damage exceeds bounded domain");
    return {left,top,int(width),int(height)};
}
void union_bounds(VectorBounds& dst,VectorBounds src){
    if(src.left>=src.right||src.top>=src.bottom)return;
    if(dst.left>=dst.right||dst.top>=dst.bottom){dst=src;return;}
    dst={std::min(dst.left,src.left),std::min(dst.top,src.top),std::max(dst.right,src.right),std::max(dst.bottom,src.bottom)};
}
std::map<int,std::uint32_t> support_owners(const Layer& layer,std::uint64_t wire,
    std::uint64_t& references,std::uint64_t& work,std::stop_token stop){
    charge(work,4*wire,max_work,"BFIL vector support ownership work exceeds128Mi units");
    std::optional<std::size_t> first;std::map<int,std::uint32_t> owners;
    for(std::size_t i=0;i<layer.planes.size();++i){poll(stop);const auto& p=layer.planes[i];
        if(p.opaque)throw std::invalid_argument("BFIL vector support requires decoded sibling planes");
        if(p.family==0x22100001||p.family==0x22100002)continue;
        if(p.family!=0x21110001&&p.family!=0x21120001&&p.family!=0x21210001&&p.family!=0x21220001)
            throw std::invalid_argument("BFIL vector support requires pure/vertex line or paint siblings");
        Reader raw(p.specific);const auto list=[&](Reader& r){const auto count=r.i32();r.require_count(count,4,8192);
            charge(references,std::uint64_t(count),8192,"BFIL vector support references exceed8192");r.take(std::size_t(count)*4);};
        if(p.family==0x21210001||p.family==0x21220001){auto topology=raw.section();list(topology);list(topology);auto lines=raw.section();list(lines);}
        else list(raw);
        if(!first)first=i;
        for(auto id:vector_line_plane_references(p)){poll(stop);if(!owners.emplace(id,p.family).second)throw std::invalid_argument("BFIL vector support has competing line planes");}
    }
    // One complete ownership validation proves both MC/Line reciprocity and
    // exclusive plane ownership, including hidden/nonmatching line siblings.
    if(first){poll(stop);(void)validated_vector_plane_line_references(layer,*first);poll(stop);}
    return owners;
}
using vector_material_detail::VectorSlots;
using vector_material_detail::PaintLists;
using vector_material_detail::MaterialDetach;
struct UniformLinePartition {
    struct PlaneList {std::size_t index;std::vector<int> ids;Bytes tail;bool visible{},changed{};};
    std::map<int,VectorPoolRecord<VectorLine>> records;
    std::vector<PlaneList> planes;
    int cursor{},high{-1};bool changed{};
    int lowest_free(std::uint64_t& work,std::stop_token stop)const{
        for(int id=0;;++id){poll(stop);charge(work,32,max_work,"BFIL vector allocator scan work exceeds128Mi units");if(!records.contains(id))return id;}
    }
    UniformLinePartition(const VectorPaintArchive& archive,const Layer& layer,std::uint64_t& work,std::stop_token stop){
        for(const auto& record:archive.base.lines){poll(stop);records.emplace(record.index,record);high=std::max(high,record.index);}
        if(const auto previous=layer.runtime_vector_high_water[5]){if(*previous<high||*previous<-1)throw std::invalid_argument("Invalid live BFIL Line pool H");high=*previous;}cursor=lowest_free(work,stop);
        for(std::size_t i=0;i<layer.planes.size();++i)if(layer.planes[i].family==0x21120001){
            Reader r(layer.planes[i].specific);const auto n=r.i32();r.require_count(n,4,8192);PlaneList p{i,{}, {},layer.planes[i].common.flags[1]!=0};
            p.ids.reserve(std::size_t(n));for(int j=0;j<n;++j)p.ids.push_back(r.i32());p.tail=r.rest();planes.push_back(std::move(p));
        }
    }
    bool partition(VectorLineArchive& archive,Layer& layer,int region_id,VectorBounds region,const std::map<int,VectorModelCurve*>& models,
        const std::map<int,VectorArchiveLoop*>& loops,std::map<int,std::uint32_t>& owners,
        std::uint64_t& wire,std::uint64_t& nodes,std::uint64_t& references,std::uint64_t& retained,std::uint64_t& work,std::stop_token stop){
        bool rebuilt{};
        const auto create=[&](const VectorLine& source){charge(wire,44,8ull*1024*1024,"BFIL vector partition wire exceeds8MiB");charge(references,1,8192,"BFIL vector partition references exceed8192");charge(nodes,1,4096,"BFIL vector partition exceeds4096 records");charge(retained,512,max_bytes,"BFIL vector partition storage exceeds512MiB");const int id=cursor;
            records.emplace(id,VectorPoolRecord<VectorLine>{id,{{},source.half_width_bits,source.color,{}},{}});if(std::int64_t(cursor)==std::int64_t(high)+1){high=cursor;++cursor;}else cursor=lowest_free(work,stop);owners.emplace(id,0x21120001);return id;};
        const auto bounds=[&](int id){auto& line=records.at(id).value;line.archived_bounds={};for(auto member:line.members){const auto& m=*models.at(member.model);union_bounds(line.archived_bounds,{m.scalars[4],m.scalars[5],m.scalars[6],m.scalars[7]});}};
        for(auto& plane:planes){poll(stop);if(!plane.visible)continue;const auto original=plane.ids;std::vector<int> updated;updated.reserve(original.size());
            for(auto id:original){poll(stop);const auto& record=records.at(id);const auto source=record.value;
                if(region.left>=source.archived_bounds.right||region.right<=source.archived_bounds.left||region.top>=source.archived_bounds.bottom||region.bottom<=source.archived_bounds.top){updated.push_back(id);continue;}
                if(!record.suffix.empty())throw std::invalid_argument("BFIL vector partition of extended Line records is unsupported");
                charge(work,32*source.members.size()+32,max_work,"BFIL vector partition work exceeds128Mi units");charge(retained,8*source.members.size(),max_bytes,"BFIL vector partition storage exceeds512MiB");
                int clone=create(source);updated.push_back(clone);std::optional<bool> selected;std::optional<VectorLineMember> previous;
                for(auto member:source.members){poll(stop);auto& model=*models.at(member.model);bool inside{};
                    for(std::size_t side=0;side<2;++side){if(model.targets[side]<0)throw std::invalid_argument("BFIL vector partition requires both Loop lanes");const auto owner=model.target_kinds[side]?model.targets[side]:loops.at(model.targets[side])->region;inside=inside||owner==region_id;}
                    if(selected&&*selected!=inside){
                        //5D7160 intentionally uses the incoming selector for
                        //both split sides, even when the outgoing node differs.
                        vector_edit_detail::partition_endpoint(archive,layer,previous->model,member.selector==0,previous->selector==0,source.half_width_bits,wire,nodes,references,retained,work,stop);
                        bounds(clone);clone=create(source);updated.push_back(clone);rebuilt=true;
                    }
                    records.at(clone).value.members.push_back(member);model.line=clone;
                    if(selected&&*selected!=inside)vector_edit_detail::partition_endpoint(archive,layer,member.model,member.selector!=0,member.selector!=0,source.half_width_bits,wire,nodes,references,retained,work,stop);
                    selected=inside;previous=member;
                }
                bounds(clone);records.erase(id);--nodes;--references;wire-=44;if(cursor>id){cursor=id;if(high==id)--high;}owners.erase(id);plane.changed=changed=true;
            }
            plane.ids=std::move(updated);
        }
        return rebuilt;
    }
    void publish(VectorPaintArchive& archive,Layer& layer){
        // Preserve the existing color-only lossless envelope (raw H/order).
        // Structural clones publish the reconstructed native allocator state.
        if(!changed){for(auto& record:archive.base.lines)record.value.color=records.at(record.index).value.color;return;}
        archive.base.lines.clear();archive.base.lines.reserve(records.size());for(auto& [id,record]:records){(void)id;archive.base.lines.push_back(record);}
        archive.base.high_water[5]=high;
        layer.runtime_vector_high_water[5]=high;
        for(auto& p:planes)if(p.changed){Writer r;r.i32(int(p.ids.size()));for(auto id:p.ids)r.i32(id);r.raw(p.tail);layer.planes[p.index].specific=std::move(r.data);}
    }
};
struct OrdinaryVectorBucket {VectorFaceVertex seed;const RegionSelection* selection;CommonElement document;};
struct VectorCloseFillRequest {const BinaryMask* mask;Rect bounds;bool rectangle{},only_transparent{};int spread_face{-1},spread_threshold{};};
void fill_vector(Layer& layer,std::size_t layer_index,std::size_t plane_index,const ConsecutiveFillItem& item,
    const ConsecutiveFillRuntime& runtime,const FillSettings& settings,ConsecutiveFillResult& result,
    double dpi,std::uint64_t& retained,std::uint64_t& work,std::stop_token stop,const OrdinaryVectorBucket* ordinary=nullptr,const VectorCloseFillRequest* close=nullptr){
    auto& plane=layer.planes[plane_index];
    if((layer.family!=0x21200000&&(!(ordinary||close)||layer.family!=0x20800000))||plane.opaque||(plane.family!=0x22100001&&plane.family!=0x22100002)||plane.raster||
       (!(ordinary||close)&&layer.common.depth!=32&&layer.common.depth!=64)||layer.common.width<=0||layer.common.height<=0||
       layer.common.width>100000||layer.common.height>100000||!std::isfinite(dpi)||dpi<=0||!std::isfinite(layer.common.resolution)||layer.common.resolution<=0||
       plane.common.width||plane.common.height||plane.common.depth||!std::isfinite(plane.common.resolution)||plane.common.resolution<=0||plane.common.resolution!=numeric_detail::binary64(layer.common.resolution*16))
        throw std::invalid_argument("BFIL vector requires Paint32/64 and native16x plane metadata");
    const auto& bucket=settings.active_bucket();
    auto prepared=runtime.references_provider?runtime.references_provider(layer_index):ConsecutiveFillReferences{};poll(stop);
    if(ordinary&&bucket.fill_separated){prepared.light_line.reset();prepared.vector_light_lines.clear();}
    if(bucket.use_light_color){
        if(prepared.light_colors.size()>100000)throw std::length_error("BFIL vector LightColor references exceed modern limit");
        std::uint64_t reference_bytes{};charge(reference_bytes,prepared.light_colors.size()*sizeof(FillReference),max_bytes,"BFIL vector LightColor metadata exceeds512MiB");
        for(const auto& reference:prepared.light_colors)validate_reference(reference,dpi,reference_bytes,stop);
        charge(retained,reference_bytes,max_bytes,"BFIL vector source and LightColor references exceed512MiB");
        const auto rows=std::uint64_t(std::ranges::count_if(item.rows,[](const auto& row){return row.done!=0;}));
        charge(work,rows*prepared.light_colors.size(),max_work,"BFIL vector ordered LightColor sampling exceeds128Mi work units");
    }
    int raw_gap{};
    if(bucket.close_gap_on&&(!ordinary||!bucket.fill_separated)){
        const auto gap=settings.use_mm_for_tools?legacy_rt_double_to_int(numeric_detail::binary64(numeric_detail::binary64(bucket.close_gap_distance_mm*plane.common.resolution)*(1.0/25.4))):std::optional<int>{};
        if(settings.use_mm_for_tools){if(!gap)throw std::invalid_argument("BFIL vector metric gap exceeds integer domain");raw_gap=*gap;}
        else{const auto scaled=std::trunc(numeric_detail::binary64(numeric_detail::binary64(double(bucket.close_gap_distance_px)/layer.common.resolution)*plane.common.resolution));if(!std::isfinite(scaled)||scaled>INT32_MAX||scaled<INT32_MIN)throw std::invalid_argument("BFIL vector gap exceeds integer domain");raw_gap=int(scaled);}
    }
    if(layer.planes.size()>1024)throw std::length_error("BFIL vector plane count exceeds1024");
    std::uint64_t wire=layer.specific.size(),plane_references{};
    for(const auto& p:layer.planes){poll(stop);charge(wire,p.specific.size(),8ull*1024*1024,"BFIL vector query wire exceeds8MiB");
        if(p.family!=0x22100001&&p.family!=0x22100002)continue;
        Reader refs(p.specific);for(int list=0;list<3;++list){const auto n=refs.i32();refs.require_count(n,4,8192);
            charge(plane_references,std::uint64_t(n),8192,"BFIL vector plane references exceed8192");refs.take(std::size_t(n)*4);}
    }
    if(wire>8ull*1024*1024)throw std::length_error("BFIL vector query wire exceeds8MiB");
    charge(retained,16*wire,max_bytes,"BFIL vector retained storage exceeds512MiB");
    charge(work,4*wire+8ull*1024*1024,max_work,"BFIL vector parse/certification work exceeds128Mi units");
    poll(stop);auto archive=decode_vector_paint_archive(layer);poll(stop);
    std::uint64_t nodes{},references=plane_references;
    const auto count=[&](const auto& pool){nodes+=pool.size();};
    count(archive.base.vertices);count(archive.base.scales);count(archive.base.subcurves);count(archive.base.models);count(archive.base.crosspoints);count(archive.base.lines);
    count(archive.loops);count(archive.regions);count(archive.faces);count(archive.gradnodes);count(archive.gradations);count(archive.textures);
    for(const auto& v:archive.base.vertices)references+=v.value.subcurves.size();for(const auto& s:archive.base.subcurves)references+=s.value.scales.size();
    for(const auto& m:archive.base.models){references+=m.value.subcurves.size()+std::ranges::count_if(m.value.crosspoints,[](int id){return id>=0;});}
    for(const auto& l:archive.base.lines)references+=l.value.members.size();for(const auto& l:archive.loops)references+=l.value.members.size();
    for(const auto& r:archive.regions)references+=r.value.outer.members.size()+r.value.holes.size()+r.value.faces.size();
    references+=archive.border.vertices.size()+archive.border.models.size()+archive.border.regions.size();
    if(nodes>4096||references>8192)throw std::length_error("BFIL vector query graph exceeds bounded profile");
    std::vector<int> selection_candidates;bool selection_materialized{};
    if(ordinary&&ordinary->selection){
        const auto packet=prepare_gradation_vector_tool_selection(*ordinary->selection,ordinary->document,selection_carrier_dpi(*ordinary->selection,dpi),stop);
        const auto& native=vector_selection_detail::reserve_vector_tool_selection(packet,retained,work,stop);
        if(!native.bypass()&&native.available()){
            vector_graph_detail::Editor editor(archive.base,&archive,layer,vector_graph_detail::BorderOwnerTag{},wire,nodes,references,retained,work,stop);
            vector_boundary_detail::Integrator context(editor);
            auto transferred=vector_selection_detail::gradation_transfer(context,native,{layer.common.x,layer.common.y});context.publish();
            selection_candidates=std::move(transferred.selected_regions);selection_materialized=editor.topology_changed;
            result.storage_changed|=selection_materialized;
            const auto dirty=vector_dirty(transferred.converter_dirty);if(dirty.width>0&&dirty.height>0)merge_damage(result,layer_index,plane_index,dirty,layer.common);
        }
    }
    auto regions=pool_index(archive.regions);auto models=pool_index(archive.base.models);auto loops=pool_index(archive.loops);
    // Record indices remain valid when creation reallocates the Face vector.
    std::map<int,std::size_t> face_positions;for(std::size_t i=0;i<archive.faces.size();++i)face_positions.emplace(archive.faces[i].index,i);
    PaintLists plane_lists(plane);auto& plane_faces=plane_lists.ids[0];std::set<int> target_faces(plane_faces.begin(),plane_faces.end());
    MaterialDetach materials{archive,plane_lists,layer,nodes,references,retained,work,stop,{}, {},false};std::optional<VectorSlots> face_slots;
    // BFIL5A1CF0 consumes saved gates directly. Rendering certification would
    // reject exterior/empty and unrelated contours before native seed lookup.
    vector_fill_detail::validate_paint_owners(archive,layer,{work,stop});
    const auto active_rows=std::uint64_t(std::ranges::count_if(item.rows,[](const auto& row){return row.done!=0;}));
    charge(work,active_rows*(nodes+references+archive.border.regions.size()+1),max_work,"BFIL vector point-query work exceeds128Mi units");
    charge(work,active_rows*std::min<std::uint64_t>(active_rows,archive.border.regions.size())*8,max_work,"BFIL vector creation lookup work exceeds128Mi units");
    auto lines=pool_index(archive.base.lines);std::optional<std::map<int,std::uint32_t>> line_owners;std::optional<UniformLinePartition> partition;
    bool wrote{},created{},materialized{selection_materialized};
    if(prepared.vector_light_lines.size()>100000)throw std::length_error("BFIL vector LightLine references exceed modern limit");
    charge(retained,prepared.vector_light_lines.size()*(sizeof(ConsecutiveFillVectorReference)+sizeof(std::optional<VectorPaintArchive>)),max_bytes,"BFIL vector LightLine metadata exceeds512MiB");
    if(bucket.use_light_line)charge(work,active_rows*prepared.vector_light_lines.size(),max_work,"BFIL vector ordered LightLine sources exceed128Mi work units");
    std::vector<std::optional<VectorPaintArchive>> source_graphs(prepared.vector_light_lines.size());
    const auto refresh_graph=[&]{
        regions=pool_index(archive.regions);models=pool_index(archive.base.models);loops=pool_index(archive.loops);lines=pool_index(archive.base.lines);
        partition.reset();line_owners.reset();face_slots.reset();materials.slots={};materials.inspected.clear();
        plane_lists=PaintLists(plane);target_faces={plane_faces.begin(),plane_faces.end()};face_positions.clear();
        for(std::size_t i=0;i<archive.faces.size();++i)face_positions.emplace(archive.faces[i].index,i);
    };
    const auto source_graph=[&](std::size_t index)->const VectorPaintArchive&{
        auto& cached=source_graphs[index];if(cached)return *cached;
        const auto* source=prepared.vector_light_lines[index].owner;
        if(!source||source->opaque||source->family!=0x21200000||(source->common.depth!=32&&source->common.depth!=64)||source->common.width<=0||source->common.height<=0||source->common.width>100000||source->common.height>100000||!std::isfinite(source->common.resolution)||source->common.resolution<=0||source->planes.size()>1024)
            throw std::invalid_argument("BFIL vector LightLine requires a prepared Paint32/64 owner");
        std::uint64_t source_wire=source->specific.size();
        for(const auto& child:source->planes){poll(stop);if(child.opaque)throw std::invalid_argument("BFIL vector LightLine has an opaque companion");
            //5EFD60 consumes this exact Paint owner's Border. Raster and
            //unmapped Vert siblings are not converted into source geometry.
            const bool raster=child.raster&&(child.family&0xf0000000u)==0x10000000u;
            if(!raster&&(child.raster||(child.family!=0x22100001&&child.family!=0x22100002&&child.family!=0x21110001&&child.family!=0x21120001&&child.family!=0x21210001&&child.family!=0x21220001)||child.common.width||child.common.height||child.common.depth||child.common.resolution!=numeric_detail::binary64(source->common.resolution*16)))throw std::invalid_argument("BFIL vector LightLine requires typed Paint/Line or raster companions");charge(source_wire,child.specific.size(),8ull*1024*1024,"BFIL vector LightLine wire exceeds8MiB");}
        if(source_wire>8ull*1024*1024)throw std::length_error("BFIL vector LightLine wire exceeds8MiB");
        charge(retained,16*source_wire,max_bytes,"BFIL vector LightLine retained storage exceeds512MiB");charge(work,4*source_wire+8ull*1024*1024,max_work,"BFIL vector LightLine decode work exceeds128Mi units");
        cached=decode_vector_paint_archive(*source);vector_fill_detail::validate_paint_owners(*cached,*source,{work,stop});poll(stop);return *cached;
    };
    const auto write_region=[&](int region_id,IncludeFillMode include,std::uint64_t fill_color,bool& refresh_query,bool admit_empty=false){
            auto& region=*regions.at(region_id);
            //5F0790 Region+28 is the embedded outer MEMBER COUNT. Exterior
            //membership and the direct color/damage write gate are independent.
            if(admit_empty||!region.outer.members.empty()){VectorArchiveFace* face{};int face_id{-1};
            for(auto id:region.faces)if(id>=0&&target_faces.contains(id)){face_id=id;face=&archive.faces[face_positions.at(id)].value;break;}
            const auto previous=face&&(face->flags&1)?face->color:0;
            if(!face){
                if(!face_slots)face_slots.emplace(archive.faces,work,stop,layer.runtime_vector_high_water[8]);
                const int id=face_slots->allocate(work,stop);
                charge(nodes,1,4096,"BFIL vector creation exceeds4096 records");charge(references,2,8192,"BFIL vector creation exceeds8192 references");
                charge(wire,32,8ull*1024*1024,"BFIL vector creation wire exceeds8MiB");charge(retained,512,max_bytes,"BFIL vector creation storage exceeds512MiB");
                poll(stop);face_positions.emplace(id,archive.faces.size());archive.faces.push_back({id,{region_id,0,0,0},{}});
                region.faces.push_back(id);plane_faces.push_back(id);target_faces.insert(id);archive.face_high_water[2]=face_slots->high;
                layer.runtime_vector_high_water[8]=face_slots->high;
                face_id=id;face=&archive.faces.back().value;created=true;result.storage_changed=true;
            }
            //Direct Face mutation precedes5BE4F0 full-chain equivalence; the
            //new material may expose a previously distinct common boundary.
            materials.detach(face_id,*face);
            const auto flags=(face->flags&0xfffffff1u)|1u;result.storage_changed=result.storage_changed||face->flags!=flags||face->color!=fill_color;
            face->flags=flags;face->color=fill_color;wrote=true;result.performed=true;
            auto bounds=region.outer.archived_bounds;
            if(include==IncludeFillMode::SpecifiedColors){
                if(!line_owners){line_owners=support_owners(layer,wire,references,work,stop);
                    charge(work,active_rows*(nodes+references)*16,max_work,"BFIL vector support traversal work exceeds128Mi units");}
                if(!bucket.include.whole_vector){
                    if(!partition){charge(retained,4*wire,max_bytes,"BFIL vector partition workspace exceeds512MiB");poll(stop);partition.emplace(archive,layer,work,stop);poll(stop);}
                    refresh_query=partition->partition(archive.base,layer,region_id,region.outer.archived_bounds,models,loops,*line_owners,wire,nodes,references,retained,work,stop);
                    result.storage_changed=result.storage_changed||partition->changed;
                }
                // Native first three keys are fixed, independent of the
                // rebuild's editable primary_colors raster extension.
                const std::array<std::uint64_t,3> fixed{0xffff00000000ffffull,0xffff0000ffff0000ull,0xffffffff00000000ull};
                const auto matches=[&](std::uint64_t color){if(color==previous)return true;
                    for(std::size_t i=0;i<6;++i){if(!bucket.include.enabled[i])continue;
                        const auto p=i<3?Pixel{}:bucket.include.custom_colors[i-3];
                        const auto key=i<3?fixed[i]:std::uint64_t(p.r)|(std::uint64_t(p.g)<<16)|(std::uint64_t(p.b)<<32)|(std::uint64_t(p.a)<<48);
                        if(color==key)return true;}
                    return false;};
                const auto propagate=[&](const VectorArchiveLoop& loop){for(auto member:loop.members){poll(stop);
                    const auto id=models.at(member.model)->line;if(id<0)continue;
                    const auto owner=line_owners->find(id);if(owner==line_owners->end())throw std::invalid_argument("BFIL vector linked support has no plane owner");
                    if(owner->second!=0x21120001)continue;auto& line=partition?partition->records.at(id).value:*lines.at(id);if(!matches(line.color))continue;
                    result.storage_changed=result.storage_changed||line.color!=fill_color;line.color=fill_color;union_bounds(bounds,line.archived_bounds);
                }};
                // RVLoop nodes carry MC pointers; MC+28 is its persistent Line.
                // Visit embedded, then saved secondary loops; never dedupe.
                propagate(region.outer);for(auto hole:region.holes)propagate(*loops.at(hole));
            }
            const auto dirty=vector_dirty(bounds);if(dirty.width>0&&dirty.height>0)merge_damage(result,layer_index,plane_index,dirty,layer.common);
            }
    };
    if(close){
        const auto edge=[](std::int64_t v){v*=16;if(std::abs(v)>1073741824)throw std::invalid_argument("Vector Close Fill raw bounds exceed domain");return int(v);};
        const auto& b=close->bounds;
        const auto scope=close->rectangle?vector_probe_detail::Scope::rectangle({edge(b.x),edge(b.y),edge(std::int64_t(b.x)+b.width),edge(std::int64_t(b.y)+b.height)}):
            vector_probe_detail::Scope::masked(*close->mask,16,{},b,{work,stop});
        if(close->spread_threshold){
            if(!target_faces.contains(close->spread_face))throw std::invalid_argument("Vector Spread Fill sample no longer belongs to the active Paint plane");
            const auto sample=archive.faces[face_positions.at(close->spread_face)].value;
            vector_probe_detail::PaintGraph graph(archive,{work,stop});
            vector_graph_detail::Editor editor(archive.base,&archive,layer,vector_graph_detail::BorderOwnerTag{},wire,nodes,references,retained,work,stop);
            vector_boundary_detail::Integrator context(editor);
            for(int id:archive.border.regions){poll(stop);const auto& region=editor.value(archive.regions,id);const auto box=region.outer.archived_bounds;
                const auto width=std::int64_t(box.right)-box.left,height=std::int64_t(box.bottom)-box.top;
                if(!region.faces.empty()||width<=0||height<=0||(width>=close->spread_threshold&&height>=close->spread_threshold))continue;
                if(!graph.matches(region,scope,true,plane.common.resolution,retained,{work,stop}))continue;
                (void)context.create_face(plane_index,id,sample);result.performed=true;
                const auto dirty=vector_dirty(box);if(dirty.width>0&&dirty.height>0)merge_damage(result,layer_index,plane_index,dirty,layer.common);
            }
            context.publish();result.storage_changed|=editor.topology_changed;
            if(editor.topology_changed)store_vector_paint_archive(layer,archive);
            return;
        }
        std::vector<int> pending;const auto candidates=archive.border.regions;
        charge(work,candidates.size()*(nodes+references+1),max_work,"Vector Close Fill query exceeds work limit");
        for(int id:candidates){poll(stop);const auto& region=*regions.at(id);const auto bounds=region.outer.archived_bounds;
            if(bounds.left>=bounds.right||bounds.top>=bounds.bottom||(close->only_transparent&&!region.faces.empty()))continue;
            vector_probe_detail::PaintGraph graph(archive,{work,stop});
            if(!graph.matches(region,scope,true,plane.common.resolution,retained,{work,stop}))continue;
            bool refresh_query{};write_region(id,bucket.include.mode,item.rows.front().color,refresh_query,true);pending.push_back(id);
            if(partition)partition->publish(archive,layer);if(created||materials.plane_changed)plane_lists.publish(plane);
            if(refresh_query)vector_fill_detail::validate_paint_owners(archive,layer,{work,stop});
        }
        if(!pending.empty()){
            vector_graph_detail::Editor editor(archive.base,&archive,layer,vector_graph_detail::BorderOwnerTag{},wire,nodes,references,retained,work,stop);
            vector_boundary_detail::Integrator context(editor);(void)context.materialize_regions(pending);context.publish();materialized|=editor.topology_changed;result.storage_changed|=editor.topology_changed;
        }
        if(created)std::ranges::sort(archive.faces,{},&VectorPoolRecord<VectorArchiveFace>::index);
        if(wrote||materialized)store_vector_paint_archive(layer,archive);
        return;
    }
    for(std::size_t row_index=0;row_index<item.rows.size();++row_index){const auto& row=item.rows[row_index];if(!row.done)continue;poll(stop);
        auto include=bucket.include.mode;if(runtime.shift_provider&&runtime.shift_provider(layer_index,row_index))include=IncludeFillMode::None;poll(stop);
        if((!ordinary||!bucket.fill_separated)&&include!=IncludeFillMode::None&&include!=IncludeFillMode::ExceptSpecifiedColors&&include!=IncludeFillMode::SpecifiedColors)throw std::invalid_argument("BFIL vector IncludeFill mode is invalid");
        const auto x=ordinary?std::int64_t(ordinary->seed.x):std::int64_t(coordinate(row.x,layer.common.x))*16,
                   y=ordinary?std::int64_t(ordinary->seed.y):std::int64_t(coordinate(row.y,layer.common.y))*16;
        if(std::abs(x)>1073741824||std::abs(y)>1073741824)throw std::invalid_argument("BFIL vector seed exceeds bounded raw domain");
        auto fill_color=row.color;
        //56E830 accepts the first prepared reference with nonzero alphaWORD;
        //its actual Color64 is retained before any target-depth conversion.
        if(bucket.use_light_color)for(const auto& reference:prepared.light_colors){poll(stop);if(const auto color=sample(reference,int(x/16),int(y/16))){fill_color=std::uint64_t(color->r)|(std::uint64_t(color->g)<<16)|(std::uint64_t(color->b)<<32)|(std::uint64_t(color->a)<<48);break;}}
        ++result.attempted_rows;bool refresh_query{};
        int seed=-1;const auto& candidates=selection_candidates.empty()?archive.border.regions:selection_candidates;
        for(int id:candidates)if(vector_fill_detail::region_contains(archive,id,{int(x),int(y)},{work,stop})){seed=id;break;}poll(stop);
        if(seed<0){
            if(!selection_candidates.empty()){
                vector_graph_detail::Editor editor(archive.base,&archive,layer,vector_graph_detail::BorderOwnerTag{},wire,nodes,references,retained,work,stop);
                vector_boundary_detail::Integrator context(editor);(void)context.materialize_regions(selection_candidates);context.publish();
                materialized|=editor.topology_changed;result.storage_changed|=editor.topology_changed;
            }
            continue;
        }
        std::vector<int> pending;for(int id:selection_candidates)if(id!=seed)pending.push_back(id);
        bool prepass=!selection_candidates.empty();
        if(raw_gap!=0||(bucket.use_light_line&&regions.at(seed)->faces.empty()&&(!prepared.vector_light_lines.empty()||prepared.light_line))){
            if(partition)partition->publish(archive,layer);if(created||materials.plane_changed)plane_lists.publish(plane);
            vector_graph_detail::Editor editor(archive.base,&archive,layer,vector_graph_detail::BorderOwnerTag{},wire,nodes,references,retained,work,stop);
            vector_boundary_detail::Integrator context(editor);VectorBounds context_dirty;union_bounds(context_dirty,regions.at(seed)->outer.archived_bounds);bool generated{};
            if(bucket.use_light_line&&regions.at(seed)->faces.empty()){
                if(prepared.light_line)throw std::invalid_argument("BFIL vector raster LightLine geometry copying is unsupported");
                for(std::size_t index=0;index<prepared.vector_light_lines.size();++index){const auto& reference=prepared.vector_light_lines[index];if(!reference.enabled)continue;poll(stop);const auto& source=source_graph(index);
                    const auto sx=std::int64_t(x)-reference.source_to_target_translation[0],sy=std::int64_t(y)-reference.source_to_target_translation[1];if(std::abs(sx)>1073741824||std::abs(sy)>1073741824)throw std::invalid_argument("BFIL vector LightLine seed translation exceeds raw domain");
                    int selected=-1;for(int id:source.border.regions)if(vector_fill_detail::region_contains(source,id,{int(sx),int(sy)},{work,stop})){selected=id;break;}if(selected<0)continue;
                    const auto source_region=std::ranges::find(source.regions,selected,&VectorPoolRecord<VectorArchiveRegion>::index);if(source_region==source.regions.end())throw std::invalid_argument("BFIL vector LightLine selected dead Region");VectorBounds box;union_bounds(box,source_region->value.outer.archived_bounds);
                    const auto translate=[&](int value,int delta){const auto v=std::int64_t(value)+delta;if(v<INT32_MIN||v>INT32_MAX)throw std::invalid_argument("BFIL vector LightLine bounds translation overflow");return int(v);};
                    box={translate(box.left,reference.source_to_target_translation[0]),translate(box.top,reference.source_to_target_translation[1]),translate(box.right,reference.source_to_target_translation[0]),translate(box.bottom,reference.source_to_target_translation[1])};
                    auto affected=vector_boundary_detail::query_candidates(context,box);VectorBounds converter_dirty=box;
                    vector_boundary_detail::SourceCopySettings copy;copy.translation=reference.source_to_target_translation;copy.regions=std::vector<int>{selected};
                    auto copied=vector_boundary_detail::copy_source(context,source,*reference.owner,copy);union_bounds(converter_dirty,copied.line_dirty);
                    const auto ordered=vector_boundary_detail::source_pointer_ordered_models(copied);std::vector<int> missing;for(int id:ordered){const auto m=editor.model(id);if(m.targets[0]<0||m.targets[1]<0)missing.push_back(id);}
                    std::vector<int> completed;(void)context.complete_missing_sides(missing,completed);copied.new_regions.insert(copied.new_regions.end(),completed.begin(),completed.end());
                    if(!affected.regions.empty()){std::vector<int> live;for(int id:ordered)if(!editor.model(id).subcurves.empty())live.push_back(id);auto repair=vector_boundary_detail::run(editor,live,affected.models,affected.regions,context.line_planes(),context.face_planes());union_bounds(converter_dirty,context.commit_regions(repair,copied.new_regions,affected.regions,false));}
                    context.merge_exteriors_once(converter_dirty);context.publish();union_bounds(context_dirty,converter_dirty);generated=true;
                }
            }
            if(generated){seed=-1;for(int id:archive.border.regions){const auto& r=editor.value(archive.regions,id);const auto b=r.outer.archived_bounds;if(b.left<b.right&&b.top<b.bottom&&!(b.left<context_dirty.right&&context_dirty.left<b.right&&b.top<context_dirty.bottom&&context_dirty.top<b.bottom))continue;pending.push_back(id);if(vector_fill_detail::region_contains(archive,id,{int(x),int(y)},{work,stop}))seed=id;}if(seed<0)throw std::invalid_argument("BFIL vector LightLine generated no target Region for seed");}
            if(raw_gap!=0){auto closed=vector_boundary_detail::close_gaps(context,seed,raw_gap);union_bounds(context_dirty,closed.context_dirty);pending.insert(pending.end(),closed.regions.begin(),closed.regions.end());seed=-1;for(int id:closed.regions)if(vector_fill_detail::region_contains(archive,id,{int(x),int(y)},{work,stop})){seed=id;break;}if(seed<0)throw std::invalid_argument("BFIL vector gap returned no target Region for seed");}
            else pending.push_back(seed);
            context.publish();prepass=prepass||generated||raw_gap!=0;materialized|=editor.topology_changed;result.storage_changed|=editor.topology_changed;
            refresh_graph();
        }else pending.push_back(seed);
        if(ordinary&&bucket.fill_separated){
            if(bucket.color_margin<0||bucket.color_margin>255)throw std::invalid_argument("Vector Bucket margin exceeds0..255");
            const auto face_for=[&](int region)->int{for(int id:regions.at(region)->faces)if(id>=0&&target_faces.contains(id))return id;return -1;};
            const int descriptor_id=face_for(seed);
            const auto descriptor=descriptor_id<0?std::optional<VectorArchiveFace>{}:std::optional<VectorArchiveFace>{archive.faces[face_positions.at(descriptor_id)].value};
            const auto matches=[&](const VectorArchiveFace& face){
                if(descriptor->flags&1){
                    if(!(face.flags&1))return false;
                    if(!(descriptor->color>>48)&&!(face.color>>48))return true;
                    const auto margin=bucket.color_margin*257;
                    for(int lane=0;lane<4;++lane)if(std::abs(int(std::uint16_t(descriptor->color>>(lane*16)))-int(std::uint16_t(face.color>>(lane*16))))>margin)return false;
                    return true;
                }
                //5EFCE2 intentionally tests solid bit1 for a gradient descriptor.
                // A persisted solid Face has a null gradient pointer; its
                // dormant material scalar is not a gradient pool identity.
                if(descriptor->flags&2)return (face.flags&1)&&descriptor->material<0;
                return (descriptor->flags&4)&&(face.flags&4)&&face.material==descriptor->material;
            };
            const auto fanout=candidates;auto repair=pending;
            if(std::ranges::find(repair,seed)==repair.end())repair.push_back(seed);
            vector_graph_detail::Editor editor(archive.base,&archive,layer,vector_graph_detail::BorderOwnerTag{},wire,nodes,references,retained,work,stop);
            vector_boundary_detail::Integrator context(editor);
            for(int region_id:fanout){poll(stop);const auto box=editor.value(archive.regions,region_id).outer.archived_bounds;if(box.left>=box.right||box.top>=box.bottom)continue;
                int face_id=face_for(region_id);if(descriptor?(face_id<0||!matches(editor.value(archive.faces,face_id))):face_id>=0)continue;
                if(face_id<0)face_id=context.create_face(plane_index,region_id,{region_id,1,fill_color,-1});
                else (void)context.replace_face_solid(face_id,fill_color,false);
                if(selection_candidates.empty()&&std::ranges::find(repair,region_id)==repair.end())repair.push_back(region_id);
                wrote=true;result.performed=true;const auto dirty=vector_dirty(box);if(dirty.width>0&&dirty.height>0)merge_damage(result,layer_index,plane_index,dirty,layer.common);
            }
            (void)context.materialize_regions(repair);context.publish();materialized|=editor.topology_changed;result.storage_changed|=editor.topology_changed;
            if(wrote||materialized)store_vector_paint_archive(layer,archive);
            return;
        }
        {
            const int region_id=seed;write_region(region_id,include,fill_color,refresh_query);
            if(partition)partition->publish(archive,layer);
            if(created||materials.plane_changed)plane_lists.publish(plane);
            bool repaired{};
            if(prepass){vector_graph_detail::Editor editor(archive.base,&archive,layer,vector_graph_detail::BorderOwnerTag{},wire,nodes,references,retained,work,stop);vector_boundary_detail::Integrator context(editor);(void)context.materialize_regions(pending);repaired=editor.topology_changed;}
            else repaired=vector_fill_detail::materialize_region(archive,layer,region_id,wire,nodes,references,retained,{work,stop}).changed;
            if(repaired){materialized=true;result.storage_changed=true;refresh_graph();refresh_query=false;}
        }
        if(refresh_query){charge(work,8ull*1024*1024+4*wire,max_work,"BFIL updated query certification work exceeds128Mi units");partition->publish(archive,layer);if(created||materials.plane_changed)plane_lists.publish(plane);poll(stop);vector_fill_detail::validate_paint_owners(archive,layer,{work,stop});poll(stop);}
    }
    poll(stop);if(partition)partition->publish(archive,layer);poll(stop);
    if(created)std::ranges::sort(archive.faces,{},&VectorPoolRecord<VectorArchiveFace>::index);
    if(wrote||materialized)store_vector_paint_archive(layer,archive);poll(stop);
    if(created||materials.plane_changed)plane_lists.publish(plane);poll(stop);
}
}

FillResult fill_vector_document(DocumentData& data,Point seed,Pixel color,const FillSettings& settings,
    const RegionSelection& selection,std::stop_token stop,const FillReferences& references){
    FillResult result;
    try{
        poll(stop);if(settings.current_set<0||settings.current_set>1||data.current_layer<0||std::size_t(data.current_layer)>=data.layers.size())throw std::invalid_argument("Vector Bucket has no valid active owner/bank");
        const auto& source=data.layers[std::size_t(data.current_layer)];if(source.opaque||source.current_plane<0||std::size_t(source.current_plane)>=source.planes.size())throw std::invalid_argument("Vector Bucket has no editable active plane");
        const auto raw=[](double v){v=std::trunc(numeric_detail::binary64(v*16));if(!std::isfinite(v)||std::abs(v)>1073741824)throw std::invalid_argument("Vector Bucket seed exceeds raw16 domain");return int(v);};
        OrdinaryVectorBucket ordinary{{raw(seed.x),raw(seed.y)},&selection,source.common};ordinary.document.resolution=data.base.dpi;ordinary.document.width=data.base.width();ordinary.document.height=data.base.height();
        if(selection.state!=SelectionLayerState::Cleared&&selection.state!=SelectionLayerState::FullPlane){
            const auto dx=std::int64_t(ordinary.seed.x/16)+source.common.x,dy=std::int64_t(ordinary.seed.y/16)+source.common.y;
            if(dx<INT32_MIN||dx>INT32_MAX||dy<INT32_MIN||dy>INT32_MAX)throw std::invalid_argument("Vector Bucket selection origin overflow");
            if(!selection.contains_document(int(dx),int(dy)))return result;
        }
        ConsecutiveFillItem item;item.rows={{std::uint64_t(color.r)|(std::uint64_t(color.g)<<16)|(std::uint64_t(color.b)<<32)|(std::uint64_t(color.a)<<48),0,0,1,0}};
        auto retained=source_bytes(data,item,stop);std::uint64_t work{};Layer candidate=source;
        ConsecutiveFillRuntime runtime;runtime.references_provider=[&](std::size_t){ConsecutiveFillReferences refs;refs.light_line=references.light_line;if(references.light_color)refs.light_colors.push_back(*references.light_color);refs.vector_light_lines=references.vector_light_lines;return refs;};
        ConsecutiveFillResult native;fill_vector(candidate,std::size_t(data.current_layer),std::size_t(source.current_plane),item,runtime,settings,native,data.base.dpi,retained,work,stop,&ordinary);
        poll(stop);result.storage_changed=native.storage_changed;result.performed=native.performed;
        for(const auto& damage:native.damage){if(!result.dirty.width)result.dirty=damage.local;else{const auto left=std::min(result.dirty.x,damage.local.x),top=std::min(result.dirty.y,damage.local.y),right=std::max(result.dirty.x+result.dirty.width,damage.local.x+damage.local.width),bottom=std::max(result.dirty.y+result.dirty.height,damage.local.y+damage.local.height);result.dirty={left,top,right-left,bottom-top};}}
        if(result.storage_changed){candidate.planes[std::size_t(candidate.current_plane)].pixels_changed=true;data.layers[std::size_t(data.current_layer)]=std::move(candidate);}
    }catch(const std::exception& e){result={};if(stop.stop_requested())result.cancelled=true;else{result.handled=false;result.error=e.what();}}
    catch(const Cancelled&){result={};result.cancelled=true;}
    return result;
}

namespace {
FillResult vector_range_fill(DocumentData& data,const BinaryMask& request,Rect bounds,Pixel color,
    const CloseFillSettings& settings,const std::optional<RasterSelection>& selection,std::stop_token stop,bool rectangle,int face=-1,int threshold=0){
    FillResult result;
    try{
        poll(stop);if(data.current_layer<0||std::size_t(data.current_layer)>=data.layers.size())throw std::invalid_argument("Vector Close Fill has no active owner");
        const auto& source=data.layers[std::size_t(data.current_layer)];if(source.opaque||source.current_plane<0||std::size_t(source.current_plane)>=source.planes.size())throw std::invalid_argument("Vector Close Fill has no active plane");
        if(request.width()!=source.common.width||request.height()!=source.common.height||request.width()<=0||request.height()<=0||std::uint64_t(request.width())*request.height()>128ull*1024*1024)throw std::invalid_argument("Vector Close Fill requires a bounded owner-sized mask");
        if(selection&&!selection->has_materialized_mask())throw std::invalid_argument("Vector Close Fill selection lacks its mask carrier");
        const int left=int(std::clamp<std::int64_t>(bounds.x,0,request.width())),top=int(std::clamp<std::int64_t>(bounds.y,0,request.height())),
            right=int(std::clamp<std::int64_t>(std::int64_t(bounds.x)+std::max(0,bounds.width),0,request.width())),bottom=int(std::clamp<std::int64_t>(std::int64_t(bounds.y)+std::max(0,bounds.height),0,request.height()));
        if(left>=right||top>=bottom)return result;
        ConsecutiveFillItem item;item.rows={{std::uint64_t(color.r)|(std::uint64_t(color.g)<<16)|(std::uint64_t(color.b)<<32)|(std::uint64_t(color.a)<<48),0,0,1,0}};
        auto retained=source_bytes(data,item,stop);std::uint64_t work{};
        const auto mask_bytes=((std::uint64_t(request.width())+31)/32)*request.height()*4+((std::uint64_t(request.width())+255)/256)*((std::uint64_t(request.height())+255)/256);
        charge(retained,2*mask_bytes,max_bytes,"Vector Close Fill mask storage exceeds512MiB");
        std::optional<BinaryMask> clipped;const BinaryMask* mask=&request;
        if(selection){clipped=request;for(int y=top;y<bottom;++y){poll(stop);charge(work,right-left,max_work,"Vector Close Fill selection work limit");for(int x=left;x<right;++x)if(!selection->contains(x,y))clipped->set(x,y,false);}mask=&*clipped;rectangle=false;}
        VectorCloseFillRequest range{mask,{left,top,right-left,bottom-top},rectangle,settings.only_transparent!=0,face,threshold};
        FillSettings lowered;lowered.active_bucket().include=settings.include;
        Layer candidate=source;ConsecutiveFillResult native;fill_vector(candidate,std::size_t(data.current_layer),std::size_t(source.current_plane),item,{},lowered,native,data.base.dpi,retained,work,stop,nullptr,&range);
        poll(stop);result.storage_changed=native.storage_changed;result.performed=native.performed;
        if(!native.damage.empty())result.dirty=native.damage.front().local;
        if(result.storage_changed){candidate.planes[std::size_t(candidate.current_plane)].pixels_changed=true;data.layers[std::size_t(data.current_layer)]=std::move(candidate);}
    }catch(const std::exception& e){result={};if(stop.stop_requested())result.cancelled=true;else{result.handled=false;result.error=e.what();}}
    catch(const Cancelled&){result={};result.cancelled=true;}
    return result;
}
}
FillResult close_fill_vector_document(DocumentData& data,const BinaryMask& request,Rect bounds,Pixel color,
    const CloseFillSettings& settings,const std::optional<RasterSelection>& selection,std::stop_token stop,bool rectangle){
    return vector_range_fill(data,request,bounds,color,settings,selection,stop,rectangle);
}
FillResult spread_fill_vector_document(DocumentData& data,const BinaryMask& request,Rect bounds,int face,
    const SpreadFillSettings& settings,const std::optional<RasterSelection>& selection,std::stop_token stop){
    const int threshold=settings.effect_choice==1?32:settings.effect_choice==2?80:settings.effect_choice==3?128:16;
    CloseFillSettings lowered;lowered.include.mode=IncludeFillMode::None;
    return vector_range_fill(data,request,bounds,{},lowered,selection,stop,false,face,threshold);
}
std::optional<int> capture_vector_spread_fill_face(const DocumentData& data,Point point,std::stop_token stop){
    if(stop.stop_requested())return {};
    if(data.current_layer<0||std::size_t(data.current_layer)>=data.layers.size())throw std::invalid_argument("Vector Spread Fill has no owner");
    const auto& owner=data.layers[std::size_t(data.current_layer)];if(owner.opaque||(owner.family!=0x21200000&&owner.family!=0x20800000)||owner.current_plane<0||std::size_t(owner.current_plane)>=owner.planes.size())throw std::invalid_argument("Vector Spread Fill target is not a Paint owner");
    const auto& plane=owner.planes[std::size_t(owner.current_plane)];if(plane.opaque||plane.raster||(plane.family!=0x22100001&&plane.family!=0x22100002))throw std::invalid_argument("Vector Spread Fill target is not a Paint plane");
    std::uint64_t wire=owner.specific.size(),work{};for(const auto& p:owner.planes)charge(wire,p.specific.size(),8ull*1024*1024,"Vector Spread Fill capture wire limit");
    charge(wire,0,8ull*1024*1024,"Vector Spread Fill capture wire limit");charge(work,4*wire+8ull*1024*1024,max_work,"Vector Spread Fill capture work limit");
    auto archive=decode_vector_paint_archive(owner);vector_fill_detail::validate_paint_owners(archive,owner,{work,stop});
    const auto raw=[](double v){v=std::trunc(numeric_detail::binary64(v*16));if(!std::isfinite(v)||std::abs(v)>1073741824)throw std::invalid_argument("Vector Spread Fill capture point exceeds raw16 domain");return int(v);};
    const VectorFaceVertex seed{raw(point.x),raw(point.y)};PaintLists lists(plane);auto faces=pool_index(archive.faces);
    for(int id:lists.ids[0]){if(stop.stop_requested())return {};const auto found=faces.find(id);if(found==faces.end())throw std::invalid_argument("Vector Spread Fill capture references a dead Face");if(vector_fill_detail::region_contains(archive,found->second->region,seed,{work,stop}))return id;}
    return {};
}

void record_consecutive_fill_point(ConsecutiveFillItem& item,std::int32_t x,std::int32_t y,std::uint64_t color){
    if(item.rows.size()>consecutive_fill_max_rows||item.current_row<0||std::size_t(item.current_row)>=item.rows.size())throw std::out_of_range("BFIL current row is outside existing list");
    auto& row=item.rows[std::size_t(item.current_row)];row.x=x;row.y=y;row.done=1;row.color=color;
}
ConsecutiveFillResult apply_consecutive_fill(DocumentData& data,const ConsecutiveFillItem& input,const ConsecutiveFillRuntime& runtime,std::stop_token stop){
    ConsecutiveFillResult result;
    try{poll(stop);const auto base_bytes=source_bytes(data,input,stop);const auto item=input;const auto before=data;auto output=data;
        std::uint64_t retained=base_bytes,work{};
        for(std::size_t layer_index=0;layer_index<output.layers.size();++layer_index){poll(stop);auto& layer=output.layers[layer_index];if(!eligible(layer,item))continue;
            if(layer.opaque)throw std::invalid_argument("BFIL opaque batch owner is unsupported");
            ++result.targets;result.performed=true;
            auto settings=runtime.bucket_provider?runtime.bucket_provider(layer_index):runtime.bucket;poll(stop);
            if(settings.current_set<0||settings.current_set>1)throw std::invalid_argument("BFIL CurrentSet is outside two native banks");
            const auto target_index=target_plane(layer);
            // Native reads the bank even for an empty row list or no matching
            // plane, including vector owners, without resolving row fields.
            // Active matching targets still require their supported worker.
            if(!target_index||std::ranges::none_of(item.rows,[](const auto& row){return row.done!=0;}))continue;
            auto& plane=layer.planes[*target_index];
            if(layer.family&0x60000000){fill_vector(layer,layer_index,*target_index,item,runtime,settings,result,data.base.dpi,retained,work,stop);continue;}
            validate_target(layer,plane,stop);
            auto refs=runtime.references_provider?runtime.references_provider(layer_index):ConsecutiveFillReferences{};poll(stop);
            if(refs.light_colors.size()>100000)throw std::length_error("BFIL Light reference count exceeds modern limit");
            std::uint64_t ref_bytes{};
            charge(ref_bytes,refs.light_colors.size()*sizeof(FillReference),max_bytes,"BFIL Light metadata exceeds512MiB");
            if(settings.active_bucket().use_light_line&&refs.light_line)validate_reference(*refs.light_line,plane.common.resolution,ref_bytes,stop);
            if(settings.active_bucket().use_light_color)for(const auto& ref:refs.light_colors)validate_reference(ref,plane.common.resolution,ref_bytes,stop);
            charge(ref_bytes,base_bytes,max_bytes,"BFIL source with Light references exceeds512MiB");
            const auto active_rows=std::uint64_t(std::ranges::count_if(item.rows,[](const auto& row){return row.done!=0;}));
            if(settings.active_bucket().use_light_color){
                charge(work,active_rows*refs.light_colors.size(),max_work,"BFIL ordered LightColor sample work exceeds128Mi units");
            }
            const auto columns=(std::uint64_t(plane.raster->width)+255)/256,rows=(std::uint64_t(plane.raster->height)+255)/256;
            charge(retained,columns*rows*(plane.raster->tile_bytes()+80),max_bytes,"BFIL retained-plus-COW storage exceeds512MiB");
            // Same-valued native writes still clone/check complete physical
            // tiles. Charge two64-bit memory visits per byte span, for ALL
            // Done rows, before allocating any candidate tile for this target.
            charge(work,active_rows*columns*rows*(plane.raster->tile_bytes()/4),max_work,"BFIL physical copy/compare work exceeds128Mi units");
            const int old_layer=output.current_layer,old_plane=layer.current_plane;output.current_layer=int(layer_index);layer.current_plane=int(*target_index);
            for(std::size_t row_index=0;row_index<item.rows.size();++row_index){const auto& row=item.rows[row_index];if(!row.done)continue;poll(stop);
                const int x=coordinate(row.x,layer.common.x),y=coordinate(row.y,layer.common.y);
                auto effective=settings;auto& bucket=effective.active_bucket();bucket.stop_if_leaked=0;bucket.fill_separated=0;
                if(runtime.shift_provider&&runtime.shift_provider(layer_index,row_index))bucket.include.mode=IncludeFillMode::None;poll(stop);
                if(bucket.color_margin<0||bucket.color_margin>255)throw std::invalid_argument("BFIL ColorMargin is outside0..255");
                // fill_document lowers gap units. Resolve independently for
                // conservative preflight, with the same caller branch order.
                auto budget_settings=bucket;
                if(bucket.close_gap_on){const auto store=numeric_detail::binary64;
                    double distance=std::trunc(store(store(double(bucket.close_gap_distance_px)/layer.common.resolution)*plane.common.resolution));
                    if(effective.use_mm_for_tools){const auto pixels=store(store(bucket.close_gap_distance_mm*plane.common.resolution)*(1.0/25.4));
                        const auto rounded=legacy_rt_double_to_int(pixels);if(!rounded)throw std::invalid_argument("BFIL metric gap exceeds integer domain");distance=*rounded;}
                    if(distance<0||distance>200)throw std::invalid_argument("BFIL gap is outside bounded0..200 pixels");
                    budget_settings.close_gap_distance_px=int(distance);
                }
                Pixel color=packed(row.color);if(bucket.use_light_color)for(const auto& ref:refs.light_colors)if(const auto p=sample(ref,x,y)){color=*p;break;}
                bucket.use_light_color=0;
                FillReferences references;if(bucket.use_light_line)references.light_line=refs.light_line;
                const std::size_t lines=(references.light_line?1:0)+(companion(layer,plane)?1:0);
                work_budget(*plane.raster,budget_settings,lines,work);++result.attempted_rows;
                const auto filled=fill_consecutive_document(output,{double(x),double(y)},color,effective,stop,references);
                if(filled.pixels.cancelled)throw Cancelled{};if(!filled.pixels.handled)throw std::invalid_argument(filled.pixels.error);
                result.changed_pixels+=filled.pixels.changed_pixels;if(filled.performed)merge_damage(result,layer_index,*target_index,filled.scan,layer.common);
                result.storage_changed=result.storage_changed||filled.storage_changed;
            }
            layer.current_plane=old_plane;output.current_layer=old_layer;
        }
        poll(stop);if(!same_data(data,before)){result={};result.handled=true;result.stale=true;result.error="BFIL source changed during provider execution";return result;}
        poll(stop);
        result.handled=true;result.completed=true;if(result.performed)data=std::move(output);return result;
    }catch(const Cancelled&){result={};result.handled=true;result.cancelled=true;}
    catch(const std::exception& e){result={};if(stop.stop_requested()){result.handled=true;result.cancelled=true;}else result.error=e.what();}
    return result;
}
ConsecutiveFillResult execute_consecutive_fill(Document& document,const ConsecutiveFillItem& item,const ConsecutiveFillRuntime& runtime,std::stop_token stop){
    ConsecutiveFillResult result;
    try{poll(stop);if(document.has_floating_selection())throw std::invalid_argument("BFIL requires settled floating content");
        // Charge the raw source graph before either metadata snapshot copy.
        (void)source_bytes(document.data,item,stop);
        const auto state=document.state_generation(),selection=document.region_selection_generation(),floating=document.floating_selection_generation(),grid=document.grid_generation(),guide=document.guide_generation();
        auto before=document.data;auto candidate=before;result=apply_consecutive_fill(candidate,item,runtime,stop);
        if(!result.handled||result.cancelled||result.stale||!result.performed)return result;poll(stop);
        if(document.state_generation()!=state||document.region_selection_generation()!=selection||document.floating_selection_generation()!=floating||document.grid_generation()!=grid||document.guide_generation()!=guide||!same_data(document.data,before)){
            result={};result.handled=true;result.stale=true;result.error="BFIL document changed during provider execution";return result;}
        // Document::commit allocates before consuming its rollback snapshot.
        // Avoid duplicating unrelated private history to publish this batch.
        poll(stop);
        document.data=std::move(candidate);
        try{document.commit(std::move(before),"Consecutive Fill",HistorySelection::PreserveCurrent);}
        catch(...){document.data=std::move(before);throw;}
        result.history_recorded=true;
    }catch(const Cancelled&){result={};result.handled=true;result.cancelled=true;}
    catch(const std::exception& e){result={};if(stop.stop_requested()){result.handled=true;result.cancelled=true;}else result.error=e.what();}
    return result;
}
}
