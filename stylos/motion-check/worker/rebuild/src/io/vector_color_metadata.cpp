#include "retas/vector_color_metadata.hpp"

#include <array>
#include <algorithm>
#include <map>
#include <set>

namespace retas {
namespace {

constexpr std::size_t vector_pool_count = 6;
constexpr std::size_t rvlcurve_pool_index = 5;
constexpr std::size_t rvface_pool_index = 2;
constexpr std::uint64_t word_empty_white = 0x0000ffffffffffffull;

struct SectionGroup {
    std::array<Bytes, vector_pool_count> children;
    Bytes suffix;
};

struct RewriteCounts {
    std::uint64_t lines{};
    std::uint64_t faces{};
};

std::uint64_t native_color(Pixel color) {
    // RTCOLORREF64 is persisted as AAAABBBBGGGGRRRR. This intentionally
    // differs from the project's internal BGRA raster byte layout.
    return (std::uint64_t(color.a) << 48) |
           (std::uint64_t(color.b) << 32) |
           (std::uint64_t(color.g) << 16) | color.r;
}

std::map<std::uint64_t, std::uint64_t> replacement_map(
    std::span<const ColorReplaceRule> rules) {
    std::map<std::uint64_t, std::uint64_t> result;
    for (auto rule = rules.rbegin(); rule != rules.rend(); ++rule) {
        if (!rule->done) continue;
        auto source = native_color(rule->source);
        if (!rule->source.a) source = word_empty_white;
        // Native insert/find returns the existing value slot, so reverse
        // traversal makes the lowest enabled row win for duplicate keys.
        result[source] = native_color(rule->destination);
    }
    return result;
}

Bytes bounded_section(Reader& reader) {
    return reader.section().rest();
}

SectionGroup split_pool_group(std::span<const std::uint8_t> payload) {
    Reader reader(payload);
    SectionGroup group;
    for (auto& child : group.children) child = bounded_section(reader);
    group.suffix = reader.rest();
    return group;
}

Bytes join_pool_group(const SectionGroup& group) {
    Writer writer;
    for (const auto& child : group.children) writer.section(child);
    writer.raw(group.suffix);
    return std::move(writer.data);
}

std::array<std::int32_t, vector_pool_count> read_high_water(
    std::span<const std::uint8_t> payload) {
    Reader reader(payload);
    std::array<std::int32_t, vector_pool_count> result;
    for (auto& value : result) value = reader.i32();
    // A derived build may retain a bounded header suffix. It is deliberately
    // ignored here and replayed unchanged by the outer archive writer.
    return result;
}

void put_u64(Bytes& bytes, std::size_t offset, std::uint64_t value) {
    if (offset > bytes.size() || bytes.size() - offset < 8)
        throw FormatError(offset, "Truncated vector color metadata field");
    for (unsigned index = 0; index < 8; ++index)
        bytes[offset + index] =
            static_cast<std::uint8_t>(value >> ((7 - index) * 8));
}

std::size_t rvlcurve_color_offset(std::span<const std::uint8_t> payload) {
    Reader reader(payload);
    const auto member_count=reader.i32();
    if(member_count<0)throw FormatError(reader.offset()-4,"Negative RVLCurve member count");
    constexpr std::size_t fixed_tail=28;
    const auto count=static_cast<std::size_t>(member_count);
    if(reader.remaining()<fixed_tail||count>(reader.remaining()-fixed_tail)/8)
        throw FormatError(reader.offset(),"Truncated RVLCurve member list");
    reader.take(count*8);reader.u32();
    return reader.offset();
}
bool rewrite_rvlcurve_payload(
    Bytes& payload,const std::map<std::uint64_t,std::uint64_t>& replacements) {
    const auto color_offset=rvlcurve_color_offset(payload);
    Reader reader(payload);reader.take(color_offset);const auto color=reader.u64();
    const auto found = replacements.find(color);
    if (found == replacements.end()) return false;
    // sub_5E95F0 stores the mapped low word and ORs the high dword with
    // 0xFFFF0000, making every replaced line color fully opaque.
    const auto replacement = found->second | 0xffff000000000000ull;
    if (replacement == color) return false;
    put_u64(payload, color_offset, replacement);
    return true;
}

bool rewrite_rvface_payload(
    Bytes& payload, const std::map<std::uint64_t, std::uint64_t>& replacements) {
    Reader reader(payload);
    const auto region = reader.i32();
    const auto flags = reader.u32();
    const auto variant = flags & 0x0fu;
    std::uint64_t color{};
    std::size_t suffix_offset = reader.offset();
    if (variant == 1) {
        color = reader.u64();
        suffix_offset = reader.offset();
    } else if (variant == 2 || variant == 4) {
        reader.i32();
        suffix_offset = reader.offset();
    }

    // The runtime worker visits records whose low discriminator has bit zero
    // set. The loader zeroes the union for other odd discriminators, so their
    // effective color is the all-zero no-color value.
    if (!(flags & 1u)) return false;

    std::uint64_t replacement{};
    if ((color >> 48) == 0) {
        const auto empty = replacements.find(word_empty_white);
        if (empty != replacements.end()) replacement = empty->second;
    } else {
        const auto found = replacements.find(color);
        if (found == replacements.end()) return false;
        replacement = found->second;
    }

    // The live worker uses (flags & 0xFFFFFFF1) | 1. The immediately
    // following native serializer persists only flags & 0xFFFF, so this is
    // the exact canonical wire result for a touched record.
    const auto new_flags = (flags & 0x0000fff1u) | 1u;
    if (variant == 1 && new_flags == flags && replacement == color)
        return false;

    const auto suffix = Bytes(payload.begin() +
                                  static_cast<std::ptrdiff_t>(suffix_offset),
                              payload.end());
    Writer rewritten;
    rewritten.i32(region);
    rewritten.u32(new_flags);
    rewritten.u64(replacement);
    rewritten.raw(suffix);
    payload = std::move(rewritten.data);
    return true;
}

template <class Rewrite>
std::uint64_t rewrite_pool(Bytes& pool, std::int32_t high_water,
                           Rewrite rewrite) {
    Reader reader(pool);
    Writer writer;
    std::set<std::int32_t> indices;
    std::uint64_t changed{};
    while (reader.remaining()) {
        if (reader.remaining() < 8)
            throw FormatError(reader.offset(), "Truncated vector pool entry");
        const auto index = reader.i32();
        if (index < 0 || high_water < 0 || index > high_water)
            throw FormatError(reader.offset() - 4,
                              "Vector pool index exceeds its high-water marker");
        if (!indices.insert(index).second)
            throw FormatError(reader.offset() - 4,
                              "Duplicate vector pool index");
        auto payload = bounded_section(reader);
        if (rewrite(payload)) ++changed;
        writer.i32(index);
        writer.section(payload);
    }
    if (changed) pool = std::move(writer.data);
    return changed;
}

RewriteCounts rewrite_vector_envelope(
    Bytes& envelope,
    const std::map<std::uint64_t, std::uint64_t>& replacements) {
    Reader reader(envelope);
    auto base_headers = bounded_section(reader);
    auto face_headers = bounded_section(reader);
    auto base_bodies_payload = bounded_section(reader);
    auto face_bodies_payload = bounded_section(reader);
    auto suffix = reader.rest();

    const auto base_high_water = read_high_water(base_headers);
    const auto face_high_water = read_high_water(face_headers);
    auto base_bodies = split_pool_group(base_bodies_payload);
    auto face_bodies = split_pool_group(face_bodies_payload);

    RewriteCounts changed;
    changed.lines = rewrite_pool(
        base_bodies.children[rvlcurve_pool_index],
        base_high_water[rvlcurve_pool_index],
        [&](Bytes& payload) {
            return rewrite_rvlcurve_payload(payload, replacements);
        });
    changed.faces = rewrite_pool(
        face_bodies.children[rvface_pool_index],
        face_high_water[rvface_pool_index],
        [&](Bytes& payload) {
            return rewrite_rvface_payload(payload, replacements);
        });

    if (changed.lines || changed.faces) {
        Writer rewritten;
        rewritten.section(base_headers);
        rewritten.section(face_headers);
        rewritten.section(join_pool_group(base_bodies));
        rewritten.section(join_pool_group(face_bodies));
        rewritten.raw(suffix);
        envelope = std::move(rewritten.data);
    }
    return changed;
}

} // namespace

VectorColorMetadataReplaceResult replace_vector_color_metadata(
    Bytes& layer_specific, std::span<const ColorReplaceRule> rules) {
    auto candidate = layer_specific;
    Reader reader(candidate);
    auto envelope = bounded_section(reader);
    const auto retained_layer_specific_tail = reader.rest();
    const auto changed =
        rewrite_vector_envelope(envelope, replacement_map(rules));

    if (changed.lines || changed.faces) {
        Writer rewritten;
        rewritten.section(envelope);
        rewritten.raw(retained_layer_specific_tail);
        layer_specific = std::move(rewritten.data);
    }
    return {changed.lines, changed.faces};
}

namespace {
std::vector<std::int32_t> reference_list(Reader& reader){
    const auto count=reader.i32();reader.require_count(count,4);
    if(count>100000)throw FormatError(reader.offset()-4,"Vector plane reference limit exceeded");
    std::vector<std::int32_t> result;result.reserve(std::size_t(count));std::set<std::int32_t> unique;
    for(int i=0;i<count;++i){const auto index=reader.i32();
        if(index<0||!unique.insert(index).second)throw FormatError(reader.offset()-4,"Invalid or duplicate plane pool reference");
        result.push_back(index);}
    return result;
}
std::vector<std::int32_t> vertex_line_references(Reader reader){
    auto topology=reader.section();(void)reference_list(topology);(void)reference_list(topology);
    auto lines=reader.section();return reference_list(lines);
}
}
VectorLinePlaneTopology vector_line_plane_topology(const Plane& plane){
    if(plane.opaque)throw std::invalid_argument("Opaque vector line plane");
    Reader reader(plane.specific);VectorLinePlaneTopology result;
    switch(plane.family){
    case 0x21110001:case 0x21120001:result.lines=reference_list(reader);break;
    case 0x21210001:case 0x21220001:{
        auto topology=reader.section();result.vertices=reference_list(topology);result.models=reference_list(topology);
        auto lines=reader.section();result.lines=reference_list(lines);break;
    }
    default:throw std::invalid_argument("Unsupported vector line topology envelope");
    }
    return result;
}
std::vector<std::int32_t> vector_line_plane_references(const Plane& plane){
    if(plane.opaque)throw std::invalid_argument("Opaque plane has no decoded line references");
    Reader reader(plane.specific);
    switch(plane.family){
    case 0x21110001:case 0x21120001:case 0x21210001:case 0x21220001:return vector_line_plane_topology(plane).lines;
    case 0x40000001:{auto bundle=reader.section();auto result=vertex_line_references(bundle);(void)reference_list(reader);return result;}
    case 0x40000002:{
        auto border=reader.section();auto topology=border.section();(void)reference_list(topology);(void)reference_list(topology);(void)reference_list(border);
        auto lines=reader.section();auto result=reference_list(lines);auto paint=reader.section();
        for(int i=0;i<3;++i)(void)reference_list(paint);(void)reference_list(reader);return result;
    }
    default:throw std::invalid_argument("Plane family does not expose this vector-line archive grammar");
    }
}
std::uint64_t set_vector_line_pool_color(Bytes& pool,std::int32_t high_water,
    std::span<const std::int32_t> indices,std::uint64_t color){
    if(high_water<-1)throw FormatError(0,"Invalid RVLCurve high-water marker");
    std::set<std::int32_t> requested;
    for(auto index:indices)if(index<0||index>high_water||!requested.insert(index).second)
        throw FormatError(0,"Invalid or duplicate requested RVLCurve index");
    Reader reader(pool);std::set<std::int32_t> live;std::vector<std::size_t> offsets;
    while(reader.remaining()){
        if(reader.remaining()<8)throw FormatError(reader.offset(),"Truncated RVLCurve pool entry");
        const auto index=reader.i32();
        if(index<0||index>high_water||!live.insert(index).second)throw FormatError(reader.offset()-4,"Invalid or duplicate live RVLCurve index");
        const auto size=reader.u32();const auto start=reader.offset();const auto payload=reader.take(size);
        const auto offset=rvlcurve_color_offset(payload);
        Reader field(payload);field.take(offset);const auto previous=field.u64();
        if(requested.contains(index)&&previous!=color)offsets.push_back(start+offset);
    }
    for(auto index:requested)if(!live.contains(index))throw FormatError(0,"Requested RVLCurve is not allocated");
    // Every validation and allocation precedes these fixed-width byte writes.
    for(auto offset:offsets)put_u64(pool,offset,color);
    return offsets.size();
}

namespace {
struct VectorPoolLocation {std::size_t offset{},size{};std::int32_t high_water{};};
std::int32_t last_high_water(Reader header,int count){
    std::int32_t value=-1;
    for(int i=0;i<count;++i){value=header.i32();if(value<-1)throw FormatError(header.offset()-4,"Invalid vector high-water marker");}
    return value;
}
std::array<VectorPoolLocation,6> base_pool_locations(Reader bodies,const std::array<std::int32_t,6>& high_water){
    std::array<VectorPoolLocation,6> result;
    for(std::size_t i=0;i<result.size();++i){auto pool=bodies.section();result[i]={pool.offset(),pool.remaining(),high_water[i]};}
    return result;
}
void validate_body_children(Reader body,int count){for(int i=0;i<count;++i)(void)body.section();}
std::array<VectorPoolLocation,6> locate_vector_pools(const Layer& layer){
    if(layer.opaque)throw std::invalid_argument("Opaque layer has no decoded vector owner envelope");
    bool face=false,ruler=false,shape=false;
    switch(layer.family){
    case 0x22100000:case 0x24400000:case 0x20000200:break;
    case 0x40004000:ruler=true;break;
    case 0x21200000:case 0x20800000:face=true;break;
    case 0x40002000:face=true;shape=true;break;
    default:throw std::invalid_argument("Unsupported vector owner family");
    }
    Reader outer(layer.specific);auto core=face?outer.section():outer;
    auto header=core.section();std::array<std::int32_t,6> high_water;
    for(auto& marker:high_water){marker=header.i32();if(marker<-1)throw FormatError(header.offset()-4,"Invalid vector high-water marker");}
    if(face)(void)last_high_water(core.section(),6);
    if(ruler||shape)(void)last_high_water(core.section(),1);
    const auto location=base_pool_locations(core.section(),high_water);
    if(face)validate_body_children(core.section(),6);
    if(ruler||shape)validate_body_children(core.section(),1);
    if(face){auto border=outer.section();(void)border.section();(void)border.section();}
    return location;
}
}
std::uint64_t set_vector_layer_line_colors(Layer& layer,
    std::span<const std::int32_t> indices,std::uint64_t color){
    const auto location=locate_vector_pools(layer).back();
    const auto first=layer.specific.begin()+location.offset;
    Bytes candidate(first,first+location.size);
    const auto changed=set_vector_line_pool_color(candidate,location.high_water,indices,color);
    // Fixed-width changes keep all enclosing section lengths and positions.
    // The owner is touched only after sibling boundaries and the line pool validate.
    if(changed)std::copy(candidate.begin(),candidate.end(),first);
    return changed;
}

namespace {
using RecordViews=std::map<std::int32_t,std::span<const std::uint8_t>>;
RecordViews records_at(const Layer& layer,const VectorPoolLocation& location){
    Reader reader(std::span<const std::uint8_t>(layer.specific).subspan(location.offset,location.size));RecordViews records;
    while(reader.remaining()){
        if(records.size()>=100000)throw FormatError(reader.offset(),"Vector owner pool record limit exceeded");
        const auto index=reader.i32();auto record=reader.section();
        if(index<0||index>location.high_water||!records.emplace(index,record.take(record.remaining())).second)
            throw FormatError(reader.offset(),"Invalid or duplicate vector pool record");
    }
    return records;
}
bool owns_line_references(std::uint32_t family){
    switch(family){case 0x21110001:case 0x21120001:case 0x21210001:case 0x21220001:case 0x40000001:case 0x40000002:return true;default:return false;}
}
bool known_without_line_references(std::uint32_t family){
    switch(family){case 0x10000001:case 0x11110002:case 0x11410001:case 0x11210001:case 0x11220001:case 0x11240001:case 0x12100001:
    case 0x12400001:case 0x11110001:case 0x24100001:case 0x22100001:case 0x22100002:case 0x40000010:case 0x40000004:case 0x40000080:case 0x40001008:case 0x40002008:return true;
    default:return false;}
}
}
std::vector<std::int32_t> validated_vector_plane_line_references(const Layer& layer,std::size_t plane){
    if(plane>=layer.planes.size()||!owns_line_references(layer.planes[plane].family))throw std::invalid_argument("No vector-line plane at the requested index");
    if(layer.specific.size()>64U*1024U*1024U||layer.planes.size()>100000)throw std::length_error("Vector line ownership limit exceeded");
    const auto pools=locate_vector_pools(layer);const auto vertices=records_at(layer,pools[0]),models=records_at(layer,pools[3]),lines=records_at(layer,pools[5]);
    std::map<std::int32_t,std::int32_t> parents,members;
    for(const auto& [index,bytes]:models){Reader record(bytes);const auto count=record.i32();
        if(count<0||record.remaining()<64||std::size_t(count)>(record.remaining()-64)/4)throw FormatError(0,"Truncated RVMCurve record");
        record.take(std::size_t(count)*4);record.u32();parents.emplace(index,record.i32());}
    for(const auto& [index,bytes]:lines){(void)rvlcurve_color_offset(bytes);Reader record(bytes);const auto count=record.i32();
        for(int i=0;i<count;++i){const auto model=record.i32();record.i32();
            if(model<0||!models.contains(model))throw FormatError(0,"RVLCurve references a missing RVMCurve");
            const auto [entry,inserted]=members.emplace(model,index);
            if(!inserted&&entry->second!=index)throw FormatError(0,"RVMCurve is shared across different RVLCurves");
            if(parents.at(model)!=index)throw FormatError(0,"RVMCurve parent disagrees with RVLCurve membership");}}
    for(const auto& [model,parent]:parents){
        if(parent>=0&&(!lines.contains(parent)||!members.contains(model)||members.at(model)!=parent))throw FormatError(0,"RVMCurve parent has no reciprocal member");
        if(parent<0&&members.contains(model))throw FormatError(0,"Nullable RVMCurve parent has a live owner");}
    std::set<std::int32_t> owned,owned_vertices,owned_models;std::vector<std::int32_t> selected;
    if(layer.family==0x21200000||layer.family==0x20800000){
        Reader outer(layer.specific);(void)outer.section();auto border=outer.section();(void)border.section();auto specific=border.section();auto topology=specific.section();
        for(auto vertex:reference_list(topology))if(!vertices.contains(vertex)||!owned_vertices.insert(vertex).second)throw FormatError(0,"Missing or shared Border RVVertex reference");
        for(auto model:reference_list(topology))if(!models.contains(model)||!owned_models.insert(model).second)throw FormatError(0,"Missing or shared Border RVMCurve reference");
    }
    for(std::size_t i=0;i<layer.planes.size();++i){const auto& node=layer.planes[i];
        if(node.opaque||(!owns_line_references(node.family)&&!known_without_line_references(node.family)))throw FormatError(0,"Unknown plane prevents vector-line ownership validation");
        if(!owns_line_references(node.family))continue;
        if(node.family==0x21210001||node.family==0x21220001){
            const auto topology=vector_line_plane_topology(node);
            for(auto vertex:topology.vertices)if(!vertices.contains(vertex)||!owned_vertices.insert(vertex).second)throw FormatError(0,"Missing or shared plane RVVertex reference");
            for(auto model:topology.models)if(!models.contains(model)||!owned_models.insert(model).second)throw FormatError(0,"Missing or shared plane RVMCurve reference");
        }
        auto refs=vector_line_plane_references(node);
        for(auto line:refs)if(!lines.contains(line)||!owned.insert(line).second)throw FormatError(0,"Missing or shared plane RVLCurve reference");
        if(i==plane)selected=std::move(refs);
    }
    return selected;
}
bool set_vector_plane_line_color(Layer& layer,std::size_t plane,std::uint64_t color){
    const auto references=validated_vector_plane_line_references(layer,plane);
    const auto changed=set_vector_layer_line_colors(layer,references,color);
    auto& common=layer.planes[plane].common;const bool different=common.color!=color;common.color=color;
    return different||changed!=0;
}

} // namespace retas
