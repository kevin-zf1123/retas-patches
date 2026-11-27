#pragma once

#include "document.hpp"

namespace retas {

struct VectorPoint {
    double x{},y{};
    bool operator==(const VectorPoint&) const = default;
};
struct VectorBounds {
    std::int32_t left{},top{},right{},bottom{};
    bool operator==(const VectorBounds&) const = default;
};
template<class T> struct VectorPoolRecord {
    std::int32_t index{};
    T value;
    Bytes suffix;
};
struct VectorVertex {
    std::int32_t x{},y{};
    // Adjacency is an ordered pointer-node list: duplicate and negative
    // nullable references are preserved; it is not an intrusive owner list.
    std::vector<std::int32_t> subcurves;
};
struct VectorLineScale {
    std::uint32_t scale_bits{},parameter_bits{};
};
struct VectorSubcurve {
    std::int32_t vertex_a{},vertex_b{},control_x{},control_y{},model{};
    std::vector<std::int32_t> scales;
};
struct VectorModelCurve {
    std::vector<std::int32_t> subcurves;
    std::uint32_t flags{};
    std::int32_t line{};
    std::array<std::int32_t,2> target_kinds{},targets{};
    // Persistent cached center/stroke bounds. Ordinary Line projection derives
    // bounds from coordinates; native Ruler walkers retain the MC stroke gate.
    std::array<std::int32_t,8> scalars{};
    std::array<std::int32_t,2> crosspoints{};
};
struct VectorCrosspoint {
    std::array<std::uint32_t,14> words{};
};
struct VectorLineMember {std::int32_t model{},selector{};};
struct VectorLine {
    std::vector<VectorLineMember> members;
    std::uint32_t half_width_bits{};
    std::uint64_t color{}; // persisted AAAABBBBGGGGRRRR
    VectorBounds archived_bounds;
};
// Six typed base pools in exact native wire order. Storage is in archive order
// and uses sparse identities; a high-water marker never causes allocation.
struct VectorLineArchive {
    std::array<std::int32_t,6> high_water{};
    std::vector<VectorPoolRecord<VectorVertex>> vertices;
    std::vector<VectorPoolRecord<VectorLineScale>> scales;
    std::vector<VectorPoolRecord<VectorSubcurve>> subcurves;
    std::vector<VectorPoolRecord<VectorModelCurve>> models;
    std::vector<VectorPoolRecord<VectorCrosspoint>> crosspoints;
    std::vector<VectorPoolRecord<VectorLine>> lines;
    Bytes header_suffix,body_suffix,layer_suffix;
};
// Base line/temp-line owner envelopes only; face, shape and ruler wrappers
// require their extension graphs and are deliberately rejected.
VectorLineArchive decode_vector_line_archive(const Layer& layer);
// Emits retained raw scalar bits, sentinels, record order and suffixes. Does
// not materialize the native post-load endpoint-parameter normalization.
Bytes encode_vector_line_archive(const VectorLineArchive& archive);

struct VectorWidthNode {
    double parameter{},scale{}; // effective post-load values
};
struct VectorLineSpan {
    std::int32_t model_index{},subcurve_index{};
    VectorPoint start,control,end;
    bool linear{},fixed_width{},round_start{},round_end{};
    double half_width{};
    std::vector<VectorWidthNode> width_nodes;
    VectorBounds bounds; // checked geometry-derived conservative bounds
};
struct VectorLineStroke {
    std::int32_t line_index{};
    std::uint64_t color{};
    std::vector<VectorLineSpan> spans;
};
struct VectorLineGeometry {
    VectorLineArchive archive;
    std::vector<VectorLineStroke> strokes; // plane's ordered line references
};
// Pure/vertex-line aliases with isolated open model chains only. Endpoint
// degree is one; interior degree is two within that same model. Projection
// requires exact reciprocal adjacency, rejects profile-retention flags 0x30,
// custom profiles, closed/branching/cross-model attachments and unsupported
// width interpolation. Archive decoding/replay retains all those raw fields.
// Expanded projection is bounded to 100000 spans and 100000 width nodes.
// Does not touch raster.
VectorLineGeometry decode_vector_line_geometry(const Layer& layer,std::size_t plane);

} // namespace retas
