#pragma once
#include "vector_face_archive.hpp"

namespace retas {
struct VectorArchiveRuler {
    std::int32_t line{-1}; // Any negative value is a nullable pointer sentinel.
    std::uint32_t flags{};
    std::int32_t x{},y{},length_20{},length_24{};
    std::uint32_t angle_bits{};
    std::int32_t field_2c{},length_30{},length_34{};
    bool operator==(const VectorArchiveRuler&)const=default;
};
struct VectorArchiveShape {
    std::int32_t line{-1},region{-1}; // Ordinary pointers, not intrusive ownership.
    std::uint32_t flags{};
    std::int32_t x{},y{},length_24{},length_28{};
    std::uint32_t angle_bits{};
    std::int32_t field_30{};
    bool operator==(const VectorArchiveShape&)const=default;
};
struct VectorRulerArchive {
    VectorLineArchive base;
    std::int32_t high_water{-1};
    std::vector<VectorPoolRecord<VectorArchiveRuler>> rulers;
    Bytes header_suffix,body_suffix;
};
struct VectorShapeArchive {
    VectorPaintArchive paint;
    std::int32_t high_water{-1};
    std::vector<VectorPoolRecord<VectorArchiveShape>> shapes;
    Bytes header_suffix,body_suffix;
};
// Complete native extension envelopes, sparse identities and known references.
// Bounded to64MiB and100000 records/references per collection. Lossless writers
// retain full flags (native canonical Ruler/Shape masks are00F0FFFF/0000FFFF),
// raw F32 bits, nullable sentinels, all suffixes and original record order.
VectorRulerArchive decode_vector_ruler_archive(const Layer&);
Bytes encode_vector_ruler_archive(const VectorRulerArchive&);
VectorShapeArchive decode_vector_shape_archive(const Layer&);
Bytes encode_vector_shape_archive(const VectorShapeArchive&);
void store_vector_shape_archive(Layer&,const VectorShapeArchive&);

struct VectorExtensionPlaneReferences {
    std::vector<std::int32_t> vertices,models,regions,lines,faces,gradations,textures,extensions;
};
// Combined Ruler/Shape plane grammar; unrelated suffixes stay in specific.
// This parses lists only. The owner validates live intrusive identities across
// all siblings, including hidden planes and the separate Shape Border.
VectorExtensionPlaneReferences vector_extension_plane_references(const Plane&);
}
