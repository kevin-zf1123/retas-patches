#pragma once
#include "vector_face_render.hpp"

namespace retas {
struct VectorArchiveLoop {
    // Fresh native pointer nodes permit same-owner repeated model-side pairs.
    // Keep every occurrence and the full selector; different owners may not
    // compete for a model's zero/nonzero selector lane.
    std::vector<VectorLineMember> members;
    std::int32_t region{};
    VectorBounds archived_bounds;
    Bytes suffix; // embedded-loop suffix; standalone record suffix is separate
};
struct VectorArchiveRegion {
    VectorArchiveLoop outer;
    std::vector<std::int32_t> holes,faces;
};
struct VectorArchiveFace {
    std::int32_t region{};
    std::uint32_t flags{};
    std::uint64_t color{}; // only exact low nibble 1 has a color on the wire
    std::int32_t material{}; // low nibble 2/4: nullable gradation/texture index
};
struct VectorArchiveBorder {
    CommonElement common;
    std::vector<std::int32_t> vertices,models,regions;
    Bytes topology_suffix,specific_suffix,record_suffix;
};
struct VectorPaintArchive {
    // Base header/body suffixes belong to their own children. base.layer_suffix
    // is unused here: the face owner's core/outer suffixes are separate below.
    VectorLineArchive base;
    std::array<std::int32_t,6> face_high_water{};
    std::vector<VectorPoolRecord<VectorArchiveLoop>> loops;
    std::vector<VectorPoolRecord<VectorArchiveRegion>> regions;
    std::vector<VectorPoolRecord<VectorArchiveFace>> faces;
    // Bounded typed pool identities and complete raw leaf payloads. This codec
    // retains even malformed inactive leaves. Active gradients/textures are
    // typed only during projection; unrelated raw leaves remain uninterpreted.
    std::vector<VectorPoolRecord<Bytes>> gradnodes,gradations,textures;
    VectorArchiveBorder border;
    Bytes face_header_suffix,face_body_suffix,core_suffix,outer_suffix;
};
// Paint/select owners only; shape adds another persistent extension grammar.
// All framing, pool identities and known references are validated atomically.
// Limits: 64 MiB archive, 100000 records/references per collection. Original
// flags, scalar/float bits, negative sentinels, order and suffixes are retained.
VectorPaintArchive decode_vector_paint_archive(const Layer&);
Bytes encode_vector_paint_archive(const VectorPaintArchive&);
//Publish validated wire plus live non-solid Color64. Retired/solid IDs leave
//the runtime map. The byte-only encoder remains a pure persistent codec.
void store_vector_paint_archive(Layer&,const VectorPaintArchive&);

struct VectorArchiveGradNode {
    std::uint64_t color{};
    std::uint32_t position_bits{};
    Bytes suffix;
    bool operator==(const VectorArchiveGradNode&) const = default;
};
struct VectorArchiveGradation {
    std::uint32_t flags{};
    std::vector<std::int32_t> stops,faces;
    std::int32_t start_x{},start_y{},end_x{},end_y{};
    std::uint32_t minor_ratio_bits{};
    Bytes suffix;
    bool operator==(const VectorArchiveGradation&) const = default;
};
// Typed known leaf grammar, with suffix/full flag/F32 preservation. Packed
// identities remain in their containing pools. These lossless writers do not
// apply the native canonical save low16 flag mask. Raw PaintArchive still
// accepts bounded inactive opaque leaves; activation validates their graph.
VectorArchiveGradNode decode_vector_gradnode(const Bytes&);
Bytes encode_vector_gradnode(const VectorArchiveGradNode&);
VectorArchiveGradation decode_vector_gradation(const Bytes&);
Bytes encode_vector_gradation(const VectorArchiveGradation&);

struct VectorArchiveTexture {
    Bytes offscreen; // complete framed embedded image, including nested suffixes
    Bytes metadata; // RCString raw bytes, without CR/DBCS or Unicode rewriting
    std::uint32_t flags{};
    std::vector<std::int32_t> faces; // nullable fresh pointer nodes, file order
    std::int32_t origin_x{},origin_y{};
    std::uint32_t scale_x_bits{},scale_y_bits{},rotation_bits{},shear_x_bits{};
    Bytes suffix;
    bool operator==(const VectorArchiveTexture&) const = default;
};
VectorArchiveTexture decode_vector_texture(const Bytes&);
Bytes encode_vector_texture(const VectorArchiveTexture&);

struct VectorPaintFaceGeometry {
    std::int32_t face_index{},region_index{};
    VectorFaceGeometry geometry;
};
struct VectorPaintPlaneGeometry {
    std::vector<VectorPaintFaceGeometry> faces; // native forward face order
};
// Parse once, then project each native paint-plane alias. Global plane list
// ownership is checked, including hidden paint siblings. Dense runtime IDs
// distinguish standalone loops from embedded region loops. Projection admits
// solid1, gradient2 or embedded-texture4 faces. Native Face traversal consumes
// only the model linear bit: all width/endpoint/marked flags remain raw and
// do not restrict this projection,
// with Type1/2 finite ordered stops, exclusive intrusive node ownership and
// reciprocal face/material/plane membership. All active sibling material
// lists (including hidden planes) are validated; aggregate active node and
// per-face stop occurrence limits are100000 before material expansion.
// Other inactive raw material leaves are not interpreted. A drawn Face's outer
// and holes need identity-closed, simple nondegenerate control-hull-separated
// curves and strictly contained, mutually disjoint holes whose containment has
// a hull-exterior endpoint. Failed nonadjacent hull pairs may be proven by
// exact dyadic half splits (depth<=16, shared work cap); one pinched Vertex is
// admitted in an outer without explicit holes when its two opposite cycles
// nest and its four rays do not alternate. An explicit same-owner internal
// hole may instead have both model sides exactly once and unique physical
// spans forming one embedded linear tree (connected E=V-1), or one simple
// open spine when any span is quadratic. A tree may meet itself only at shared
// Vertex identities; overlaps, crossings and unrelated contacts reject.
// Components stay strictly separated from boundaries and other components,
// inside the outer and outside real holes. Only their transient
// proof omits the positive-area gate: every original hole/member/span remains
// in the renderer packet for native Region queries and no-point block gates.
// Undrawn closure loops need only
// connected identity closure and unique reciprocal model sides, so they may
// revisit a Vertex or walk an open rod forward and back. Texture sources admit dense canonical1/32/64, repeat/clamp, finite
// invertible scales, finite shear and |angle radians|<2^63. Native FSIN/FCOS
// C2 out-of-range angles stay raw and reject before image expansion.
// Missing blocks/empty images/unknown
// cursor modes reject. Before decompression, certified clipped AA2 span query
// endpoints must fit signed cursor conversion, including zero-coverage tails.
// Control hulls without weak separation/other curves/unknown materials remain raw but reject here.
// A model's other side may refer to an adjacent external region/loop; their
// full closure is retained in the projected graph for reciprocal ownership.
// Bounds come from coordinates, not archived rectangle scalars, except that a
// canonical empty main outer (no members, empty/inverted saved box) yields an
// empty draw plan; a positive saved box on empty members rejects. Certification
// is limited to 8 Mi work units and 100000 expanded vertices/spans/members.
// No AA, opacity, DPI, raster publication or Document cache policy is inferred.
VectorPaintPlaneGeometry project_vector_paint_plane(
    const VectorPaintArchive&,const Layer&,std::size_t plane);
// Saved plane order; non-paint slots are empty and require the caller's
// companion-family admission. All active materials, including hidden siblings,
// are checked/billed before image decompression. Each unique image is decoded
// once and shared by its faces. Selected-plane wrapper retains its geometry
// certification scope; this entry certifies the complete owner jointly.
//512MiB aggregate storage includes two wire copies, source+snapshot images,
// gradient metadata, two AA scratch surfaces and caller-reserved output bytes.
std::vector<VectorPaintPlaneGeometry> project_vector_paint_planes(
    const VectorPaintArchive&,const Layer&,std::uint64_t reserved_storage=0);
}
