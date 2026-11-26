#include "retas/vector_geometry.hpp"
#include "retas/vector_color_metadata.hpp"
#include "vector_archive_budget.hpp"
#include "vector_line_projection.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>

namespace retas {
namespace {
constexpr std::size_t limit=100000;
[[noreturn]] void invalid(const char* message){throw FormatError(0,message);}
bool base_owner(std::uint32_t family){return family==0x22100000||family==0x24400000||family==0x20000200;}
std::vector<std::int32_t> indices(Reader& reader,std::size_t tail=0){
    const auto count=reader.i32();
    if(count<0||std::size_t(count)>limit||reader.remaining()<tail||std::size_t(count)>(reader.remaining()-tail)/4)
        throw FormatError(reader.offset()-4,"Invalid vector reference count");
    std::vector<std::int32_t> result;result.reserve(std::size_t(count));
    for(int i=0;i<count;++i)result.push_back(reader.i32());
    return result;
}
template<class T,class Decode> std::vector<VectorPoolRecord<T>> pool(Reader reader,std::int32_t high_water,Decode decode){
    std::vector<VectorPoolRecord<T>> records;std::set<std::int32_t> live;
    while(reader.remaining()){
        if(records.size()>=limit)invalid("Vector pool exceeds record limit");
        const auto index=reader.i32();auto payload=reader.section();
        if(index<0||index>high_water||!live.insert(index).second)invalid("Invalid or duplicate vector pool index");
        auto value=decode(payload);records.push_back({index,std::move(value),payload.rest()});
    }
    return records;
}
template<class T> auto lookup(const std::vector<VectorPoolRecord<T>>& records){
    std::map<std::int32_t,const T*> result;
    for(const auto& record:records)if(record.index<0||!result.emplace(record.index,&record.value).second)invalid("Invalid or duplicate typed vector identity");
    return result;
}
template<class Map> void nullable(const Map& records,std::int32_t index){
    if(index>=0&&!records.contains(index))invalid("Missing nullable vector pool target");
}
template<class Map> void member(const Map& records,std::int32_t index){
    if(index<0||!records.contains(index))invalid("Missing vector owner list member");
}
void validate_graph(const VectorLineArchive& archive){
    const auto vertices=lookup(archive.vertices);const auto scales=lookup(archive.scales);const auto subcurves=lookup(archive.subcurves);const auto models=lookup(archive.models);const auto crosspoints=lookup(archive.crosspoints);const auto lines=lookup(archive.lines);
    std::map<std::int32_t,std::int32_t> scale_owners,subcurve_owners,model_owners;
    for(const auto& [id,vertex]:vertices)for(auto ref:vertex->subcurves)nullable(subcurves,ref);
    for(const auto& [id,curve]:subcurves){
        nullable(vertices,curve->vertex_a);nullable(vertices,curve->vertex_b);nullable(models,curve->model);
        for(auto scale:curve->scales){member(scales,scale);if(!scale_owners.emplace(scale,id).second)invalid("Shared or duplicate intrusive width node");}
    }
    for(const auto& [id,model]:models){
        nullable(lines,model->line);
        for(auto target:model->targets)if(target>=0)invalid("Face target in base vector-line owner");
        for(auto cross:model->crosspoints)nullable(crosspoints,cross);
        for(auto sub:model->subcurves){member(subcurves,sub);
            if(!subcurve_owners.emplace(sub,id).second)invalid("Shared or duplicate intrusive subcurve");
            if(subcurves.at(sub)->model!=id)invalid("Subcurve parent disagrees with model list");}
    }
    for(const auto& [id,line]:lines)for(auto ref:line->members){member(models,ref.model);
        const auto [owner,inserted]=model_owners.emplace(ref.model,id);
        if(!inserted&&owner->second!=id)invalid("Model belongs to different line owners");
        if(models.at(ref.model)->line!=id)invalid("Model parent disagrees with line list");}
    for(const auto& [id,curve]:subcurves){
        if(curve->model>=0&&(!subcurve_owners.contains(id)||subcurve_owners.at(id)!=curve->model))invalid("Model parent lacks reciprocal subcurve member");
        if(curve->model<0&&subcurve_owners.contains(id))invalid("Nullable subcurve parent has owner");
    }
    for(const auto& [id,model]:models){
        if(model->line>=0&&(!model_owners.contains(id)||model_owners.at(id)!=model->line))invalid("Line parent lacks reciprocal model member");
        if(model->line<0&&model_owners.contains(id))invalid("Nullable model parent has owner");
    }
}
void write_indices(Writer& writer,const std::vector<std::int32_t>& values){
    if(values.size()>limit)invalid("Vector list exceeds reference limit");
    writer.i32(static_cast<std::int32_t>(values.size()));for(auto value:values)writer.i32(value);
}
template<class T,class Encode> void write_pool(Writer& bodies,const std::vector<VectorPoolRecord<T>>& records,std::int32_t high_water,Encode encode){
    Writer writer;
    if(records.size()>limit)invalid("Vector pool exceeds record limit");
    for(const auto& record:records){if(record.index>high_water)invalid("Vector identity exceeds high-water marker");
        Writer payload;encode(payload,record.value);payload.raw(record.suffix);writer.i32(record.index);writer.section(payload.data);}
    bodies.section(writer.data);
}
VectorBounds bounds(const VectorLineSpan& span){
    double maximum=span.fixed_width?1:0;
    if(!span.fixed_width)for(const auto& node:span.width_nodes)maximum=std::max(maximum,node.scale);
    const double width=span.half_width*maximum;
    if(!std::isfinite(width)||width>std::numeric_limits<std::int32_t>::max()-1.0)invalid("Vector stroke width exceeds coordinate range");
    const double radius=std::trunc(width)+1;
    double left=std::min(span.start.x,span.end.x),right=std::max(span.start.x,span.end.x);
    double top=std::min(span.start.y,span.end.y),bottom=std::max(span.start.y,span.end.y);
    if(!span.linear){left=std::min(left,span.control.x);right=std::max(right,span.control.x);top=std::min(top,span.control.y);bottom=std::max(bottom,span.control.y);}
    left-=radius;top-=radius;right+=radius+1;bottom+=radius+1;
    for(auto coordinate:{left,top,right,bottom})if(coordinate<std::numeric_limits<std::int32_t>::min()||coordinate>std::numeric_limits<std::int32_t>::max())invalid("Vector geometry bounds overflow");
    return {static_cast<std::int32_t>(left),static_cast<std::int32_t>(top),static_cast<std::int32_t>(right),static_cast<std::int32_t>(bottom)};
}
using Incidences=std::map<std::int32_t,std::vector<std::pair<std::int32_t,std::int32_t>>>;
Incidences endpoint_incidences(const VectorLineArchive& archive){
    Incidences result;
    for(const auto& record:archive.subcurves){const auto& sub=record.value;
        for(auto vertex:{sub.vertex_a,sub.vertex_b})if(vertex>=0)result[vertex].push_back({record.index,sub.model});}
    return result;
}
void admit_model_chain(std::int32_t model_id,const VectorModelCurve& model,
    const std::map<std::int32_t,const VectorSubcurve*>& subcurves,
    const std::map<std::int32_t,const VectorVertex*>& vertices,const Incidences& incidences,vector_archive_detail::LineClosurePolicy policy){
    // Shape/Ruler load saved descriptors directly. Retention bits0x10/0x20 are
    // edit-time state;59F4C0 passes only the linear/fixed bits to drawing.
    // Fresh projection retains its strict domain; NativeStored reads flags&3.
    const bool stored=policy==vector_archive_detail::LineClosurePolicy::StoredProfiles||policy==vector_archive_detail::LineClosurePolicy::NativeStored;
    const auto draw_flags=policy==vector_archive_detail::LineClosurePolicy::NativeStored?model.flags&3u:stored?model.flags&~0x30u:model.flags;
    if((model.flags&0x30)&&(!stored||draw_flags>3))throw std::invalid_argument("Unsupported retained vector endpoint model");
    if(model.subcurves.empty())return;
    const auto first=subcurves.at(model.subcurves.front())->vertex_a;
    if(first<0)throw std::invalid_argument("Nullable vector endpoints cannot be rendered");
    std::set<std::int32_t> path_vertices{first};std::int32_t previous=first;
    for(std::size_t i=0;i<model.subcurves.size();++i){const auto& sub=*subcurves.at(model.subcurves[i]);
        if(sub.vertex_a!=previous||sub.vertex_b<0)
            throw std::invalid_argument("Disconnected or reversed vector model chain is unsupported");
        if(!path_vertices.insert(sub.vertex_b).second&&!(policy!=vector_archive_detail::LineClosurePolicy::OpenOnly&&i+1==model.subcurves.size()&&sub.vertex_b==first&&model.subcurves.size()>1))
            throw std::invalid_argument("Closed or repeated-vertex vector topology is unsupported");
        previous=sub.vertex_b;}
    const auto last=previous;const bool closed=first==last;
    if(closed){if(draw_flags>3||(!stored&&draw_flags<2))throw std::invalid_argument("Unsupported closed stored Line class");for(auto id:model.subcurves){const auto& sub=*subcurves.at(id);const auto& a=*vertices.at(sub.vertex_a);const auto& b=*vertices.at(sub.vertex_b);
        for(auto value:{a.x,a.y,b.x,b.y})if(std::abs(std::int64_t(value))>134217727)throw std::invalid_argument("Closed stored chain exceeds certified raw domain");
        if(!(policy==vector_archive_detail::LineClosurePolicy::NativeStored&&(model.flags&1)))
            for(auto value:{sub.control_x,sub.control_y})if(std::abs(std::int64_t(value))>134217727)throw std::invalid_argument("Closed stored control exceeds certified raw domain");}
    }
    // Native archive loading retains the stored MC descriptor pointers; it
    // does not invoke5B6E90 editing reconstruction. NULL uses ordinary caps;
    // the Shape descriptor policy retains saved non-NULL overrides separately.
    for(auto vertex:path_vertices){const auto found=incidences.find(vertex);
        const auto expected=std::size_t(!closed&&(vertex==first||vertex==last)?1:2);
        if(found==incidences.end()||(policy!=vector_archive_detail::LineClosurePolicy::NativeStored&&found->second.size()!=expected))
            throw std::invalid_argument("Branching or shared vector attachment topology is unsupported");
        std::set<std::int32_t> physical;
        for(const auto& [sub_id,parent]:found->second){
            if(policy!=vector_archive_detail::LineClosurePolicy::NativeStored&&parent!=model_id)throw std::invalid_argument("Cross-model vector attachment topology is unsupported");
            physical.insert(sub_id);}
        std::set<std::int32_t> adjacent;
        for(auto sub_id:vertices.at(vertex)->subcurves){
            if(policy==vector_archive_detail::LineClosurePolicy::NativeStored){if(sub_id>=0)adjacent.insert(sub_id);continue;}
            if(sub_id<0||!adjacent.insert(sub_id).second)
                throw std::invalid_argument("Nullable or repeated vector render adjacency is unsupported");}
        if(adjacent!=physical)throw std::invalid_argument("Vector render adjacency disagrees with endpoint topology");
    }
}
VectorEndpointProfile stored_profile(const VectorCrosspoint& cross){
    const auto& w=cross.words;VectorEndpointProfile p;p.first_kind=w[0];p.second_kind=w[1];
    if(p.first_kind>4||p.second_kind>4)throw std::invalid_argument("Unsupported stored Shape endpoint kind");
    const auto xy=[&](std::size_t i){return VectorPoint{double(std::bit_cast<std::int32_t>(w[i])),double(std::bit_cast<std::int32_t>(w[i+1]))};};
    p.point0=xy(4);p.point2=xy(12);
    if(p.first_kind==2){p.control0=xy(6);p.middle=xy(8);}
    if(p.second_kind==2){p.control1=xy(10);p.middle=xy(8);}
    if(p.first_kind==1||p.first_kind==4||p.second_kind==1||p.second_kind==4)p.middle=xy(8);
    if(p.first_kind==1)p.first_trim=std::bit_cast<float>(w[2]);
    if(p.second_kind==1)p.second_trim=std::bit_cast<float>(w[3]);
    // Native5A7080 reads trims only for kind1;598040/597BF0 ignore inactive
    // controls/middle. Canonicalize only transient unused fields, not raw bytes.
    return p;
}
} // namespace

void vector_archive_detail::validate_stored_line_attachments(const VectorLineArchive& archive){
    const auto vertices=lookup(archive.vertices);
    const auto subcurves=lookup(archive.subcurves);
    const auto incidences=endpoint_incidences(archive);
    for(const auto& [id,sub]:subcurves){
        nullable(vertices,sub->vertex_a);nullable(vertices,sub->vertex_b);
    }
    for(const auto& [id,vertex]:vertices){
        std::set<std::int32_t> physical,adjacent;
        if(const auto found=incidences.find(id);found!=incidences.end())
            for(const auto& [sub,parent]:found->second)physical.insert(sub);
        for(auto sub:vertex->subcurves){
            nullable(subcurves,sub);if(sub>=0)adjacent.insert(sub);
        }
        if(adjacent!=physical)
            throw std::invalid_argument("Stored owner adjacency disagrees with endpoint topology");
    }
}

VectorLineArchive decode_vector_line_archive(const Layer& layer){
    if(layer.opaque||!base_owner(layer.family))throw std::invalid_argument("Unsupported vector line owner envelope");
    if(layer.specific.size()>64U*1024U*1024U)throw std::length_error("Vector line archive exceeds64MiB");
    VectorLineArchive result;Reader outer(layer.specific);auto header=outer.section();
    for(auto& marker:result.high_water){marker=header.i32();if(marker<-1)invalid("Invalid vector high-water marker");}
    result.header_suffix=header.rest();auto bodies=outer.section();
    result.vertices=pool<VectorVertex>(bodies.section(),result.high_water[0],[](Reader& r){VectorVertex v;v.x=r.i32();v.y=r.i32();v.subcurves=indices(r);return v;});
    result.scales=pool<VectorLineScale>(bodies.section(),result.high_water[1],[](Reader& r){return VectorLineScale{r.u32(),r.u32()};});
    result.subcurves=pool<VectorSubcurve>(bodies.section(),result.high_water[2],[](Reader& r){VectorSubcurve v;v.vertex_a=r.i32();v.vertex_b=r.i32();v.control_x=r.i32();v.control_y=r.i32();v.model=r.i32();v.scales=indices(r);return v;});
    result.models=pool<VectorModelCurve>(bodies.section(),result.high_water[3],[](Reader& r){
        VectorModelCurve v;v.subcurves=indices(r,64);v.flags=r.u32();v.line=r.i32();
        for(std::size_t i=0;i<2;++i){v.target_kinds[i]=r.i32();v.targets[i]=r.i32();}
        for(auto& scalar:v.scalars)scalar=r.i32();for(auto& cross:v.crosspoints)cross=r.i32();return v;});
    result.crosspoints=pool<VectorCrosspoint>(bodies.section(),result.high_water[4],[](Reader& r){VectorCrosspoint v;for(auto& word:v.words)word=r.u32();return v;});
    result.lines=pool<VectorLine>(bodies.section(),result.high_water[5],[](Reader& r){
        VectorLine v;const auto count=r.i32();
        if(count<0||std::size_t(count)>limit||r.remaining()<28||std::size_t(count)>(r.remaining()-28)/8)invalid("Invalid line model member count");
        v.members.reserve(std::size_t(count));for(int i=0;i<count;++i)v.members.push_back({r.i32(),r.i32()});
        v.half_width_bits=r.u32();v.color=r.u64();v.archived_bounds={r.i32(),r.i32(),r.i32(),r.i32()};return v;});
    result.body_suffix=bodies.rest();result.layer_suffix=outer.rest();validate_graph(result);return result;
}
Bytes encode_vector_line_archive(const VectorLineArchive& archive){
    vector_archive_detail::line_wire_size(archive);validate_graph(archive);Writer header,bodies,outer;
    for(auto marker:archive.high_water){if(marker<-1)invalid("Invalid vector high-water marker");header.i32(marker);}header.raw(archive.header_suffix);
    write_pool(bodies,archive.vertices,archive.high_water[0],[](Writer& w,const VectorVertex& v){w.i32(v.x);w.i32(v.y);write_indices(w,v.subcurves);});
    write_pool(bodies,archive.scales,archive.high_water[1],[](Writer& w,const VectorLineScale& v){w.u32(v.scale_bits);w.u32(v.parameter_bits);});
    write_pool(bodies,archive.subcurves,archive.high_water[2],[](Writer& w,const VectorSubcurve& v){w.i32(v.vertex_a);w.i32(v.vertex_b);w.i32(v.control_x);w.i32(v.control_y);w.i32(v.model);write_indices(w,v.scales);});
    write_pool(bodies,archive.models,archive.high_water[3],[](Writer& w,const VectorModelCurve& v){write_indices(w,v.subcurves);w.u32(v.flags);w.i32(v.line);for(std::size_t i=0;i<2;++i){w.i32(v.target_kinds[i]);w.i32(v.targets[i]);}for(auto scalar:v.scalars)w.i32(scalar);for(auto cross:v.crosspoints)w.i32(cross);});
    write_pool(bodies,archive.crosspoints,archive.high_water[4],[](Writer& w,const VectorCrosspoint& v){for(auto word:v.words)w.u32(word);});
    write_pool(bodies,archive.lines,archive.high_water[5],[](Writer& w,const VectorLine& v){
        if(v.members.size()>limit)invalid("Line model list exceeds reference limit");w.i32(static_cast<std::int32_t>(v.members.size()));for(auto member:v.members){w.i32(member.model);w.i32(member.selector);}w.u32(v.half_width_bits);w.u64(v.color);w.i32(v.archived_bounds.left);w.i32(v.archived_bounds.top);w.i32(v.archived_bounds.right);w.i32(v.archived_bounds.bottom);});
    bodies.raw(archive.body_suffix);outer.section(header.data);outer.section(bodies.data);outer.raw(archive.layer_suffix);return std::move(outer.data);
}
VectorLineGeometry decode_vector_line_geometry(const Layer& layer,std::size_t plane){
    if(plane>=layer.planes.size()||layer.planes[plane].opaque
       ||(layer.planes[plane].family!=0x21110001&&layer.planes[plane].family!=0x21120001
          &&layer.planes[plane].family!=0x21210001&&layer.planes[plane].family!=0x21220001))
        throw std::invalid_argument("Unsupported vector line plane");
    VectorLineGeometry result;result.archive=decode_vector_line_archive(layer);
    const auto references=validated_vector_plane_line_references(layer,plane);
    result.strokes=vector_archive_detail::project_line_strokes(result.archive,references);return result;
}
std::vector<VectorLineStroke> vector_archive_detail::project_line_strokes(const VectorLineArchive& archive,std::span<const std::int32_t> references,LineClosurePolicy policy,std::vector<VectorSpanProfiles>* profiles){
    if(references.size()>100000)throw std::length_error("Too many projected vector lines");
    std::vector<VectorLineStroke> result;
    const auto vertices=lookup(archive.vertices);const auto scales=lookup(archive.scales);const auto subcurves=lookup(archive.subcurves);const auto models=lookup(archive.models);const auto lines=lookup(archive.lines);const auto crosspoints=lookup(archive.crosspoints);
    std::vector<VectorSpanProfiles> projected_profiles;bool any_profile=false;
    const auto incidences=endpoint_incidences(archive);std::set<std::int32_t> admitted_models;
    std::size_t span_count{},width_node_count{};
    for(auto ref:references){const auto& line=*lines.at(ref);VectorLineStroke stroke;stroke.line_index=ref;stroke.color=line.color;
        const auto half_width=double(std::bit_cast<float>(line.half_width_bits));
        if(!std::isfinite(half_width)||half_width<0)invalid("Non-finite or negative vector half-width");
        for(std::size_t m=0;m<line.members.size();++m){const auto& member=line.members[m];const auto& model=*models.at(member.model);
            const bool stored=policy==LineClosurePolicy::StoredProfiles||policy==LineClosurePolicy::NativeStored;
            const bool has_profile=model.crosspoints[0]>=0||model.crosspoints[1]>=0;
            if(has_profile&&(!stored||!profiles))throw std::invalid_argument("Crosspoint-dependent vector profiles are unsupported");
            const auto draw_flags=policy==LineClosurePolicy::NativeStored?model.flags&3u:model.flags&~0x30u;
            if(has_profile&&draw_flags>3)throw std::invalid_argument("Unsupported stored Line profile model class");
            if(has_profile&&model.subcurves.empty())throw std::invalid_argument("Stored Line profile has no endpoint span");
            const bool closed=!model.subcurves.empty()&&subcurves.at(model.subcurves.front())->vertex_a==subcurves.at(model.subcurves.back())->vertex_b;
            const bool stored_variable=stored&&!(model.flags&2)&&(has_profile||(model.flags&0x30)||closed);
            VectorSpanProfiles endpoints;
            if(has_profile){if(model.crosspoints[0]>=0)endpoints.start=stored_profile(*crosspoints.at(model.crosspoints[0]));if(model.crosspoints[1]>=0)endpoints.end=stored_profile(*crosspoints.at(model.crosspoints[1]));if(!any_profile)projected_profiles.resize(span_count);any_profile=true;}
            if(admitted_models.insert(member.model).second)admit_model_chain(member.model,model,subcurves,vertices,incidences,policy);
            if(model.subcurves.size()>limit-span_count)invalid("Expanded vector geometry exceeds span limit");
            span_count+=model.subcurves.size();
            for(std::size_t s=0;s<model.subcurves.size();++s){const auto sub_id=model.subcurves[s];const auto& sub=*subcurves.at(sub_id);
                if(sub.vertex_a<0||sub.vertex_b<0)throw std::invalid_argument("Nullable vector endpoints cannot be rendered");
                const auto& a=*vertices.at(sub.vertex_a);const auto& b=*vertices.at(sub.vertex_b);
                VectorLineSpan span;span.model_index=member.model;span.subcurve_index=sub_id;span.start={double(a.x),double(a.y)};span.end={double(b.x),double(b.y)};span.control={double(sub.control_x),double(sub.control_y)};
                span.linear=(model.flags&1)||span.control==span.start||span.control==span.end;span.fixed_width=(model.flags&2)!=0;span.half_width=half_width;
                // The native linear vtable never reads the saved SC control.
                // Normalize only its transient admission/drawing carrier.
                if(policy==LineClosurePolicy::NativeStored&&(model.flags&1))span.control=span.start;
                span.round_start=s==0&&(member.selector!=0?m==0:m+1==line.members.size());
                span.round_end=s+1==model.subcurves.size()&&(member.selector!=0?m+1==line.members.size():m==0);
                // 5A6C70 overwrites the first/last effective parameters with
                // 0/1; raw archive bits are retained. New saved variable
                // profile/closed domains need a real interpolation pair.
                if(stored_variable&&sub.scales.size()<2)throw std::invalid_argument("Stored variable Line span needs two width nodes");
                if(sub.scales.size()>limit-width_node_count)invalid("Expanded vector geometry exceeds width-node limit");
                width_node_count+=sub.scales.size();span.width_nodes.reserve(sub.scales.size());
                for(std::size_t n=0;n<sub.scales.size();++n){const auto& raw=*scales.at(sub.scales[n]);const double scale=std::bit_cast<float>(raw.scale_bits);
                    double parameter=std::bit_cast<float>(raw.parameter_bits);if(n==0)parameter=0;if(n+1==sub.scales.size())parameter=1;
                    if(!std::isfinite(scale)||scale<0||!std::isfinite(parameter))invalid("Invalid vector width node");
                    if(!span.fixed_width&&(parameter<0||parameter>1||(!span.width_nodes.empty()&&(stored?parameter<span.width_nodes.back().parameter:parameter<=span.width_nodes.back().parameter))))
                        throw std::invalid_argument("Non-monotonic vector width interpolation is unsupported");
                    span.width_nodes.push_back({parameter,scale});}
                if(!span.fixed_width&&span.width_nodes.empty())throw std::invalid_argument("Variable-width vector span has no width nodes");
                span.bounds=bounds(span);stroke.spans.push_back(std::move(span));
                if(profiles&&any_profile){VectorSpanProfiles p;if(s==0)p.start=endpoints.start;if(s+1==model.subcurves.size())p.end=endpoints.end;projected_profiles.push_back(std::move(p));}}
        }
        result.push_back(std::move(stroke));
    }
    if(profiles)*profiles=std::move(projected_profiles);return result;
}
} // namespace retas

namespace retas::vector_archive_detail {
std::size_t line_wire_size(const VectorLineArchive& a){
    constexpr std::size_t byte_limit=64U*1024U*1024U,count_limit=100000;
    std::size_t total=8+24+24;
    const auto add=[&](std::size_t n){if(n>byte_limit-total)throw std::length_error("Vector line archive exceeds64MiB");total+=n;};
    const auto list=[&](std::size_t n,std::size_t stride){if(n>count_limit)throw FormatError(0,"Vector line collection limit exceeded");add(n*stride);};
    const auto pool=[&](const auto& records,auto payload){if(records.size()>count_limit)throw FormatError(0,"Vector line pool record limit exceeded");for(const auto& r:records){add(8);add(r.suffix.size());payload(r.value);}};
    for(const auto* suffix:{&a.header_suffix,&a.body_suffix,&a.layer_suffix})add(suffix->size());
    pool(a.vertices,[&](const VectorVertex& v){add(12);list(v.subcurves.size(),4);});
    pool(a.scales,[&](const auto&){add(8);});
    pool(a.subcurves,[&](const VectorSubcurve& v){add(24);list(v.scales.size(),4);});
    pool(a.models,[&](const VectorModelCurve& v){add(68);list(v.subcurves.size(),4);});
    pool(a.crosspoints,[&](const auto&){add(56);});
    pool(a.lines,[&](const VectorLine& v){add(32);list(v.members.size(),8);});
    return total;
}
}
