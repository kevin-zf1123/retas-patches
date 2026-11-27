#pragma once

#include "color_replace.hpp"

namespace retas {

struct VectorColorMetadataReplaceResult {
    std::uint64_t line_colors_changed{};
    std::uint64_t face_colors_changed{};

    std::uint64_t records_changed() const {
        return line_colors_changed + face_colors_changed;
    }
    bool modified() const { return records_changed() != 0; }
};

// Rewrites the two color-bearing record families in a CVectorPaintLayer's
// layer-specific RCArchive payload. Geometry records and bounded unknown
// suffixes are retained byte-for-byte. The input is committed only after the
// complete nested archive has been validated.
VectorColorMetadataReplaceResult replace_vector_color_metadata(
    Bytes& layer_specific, std::span<const ColorReplaceRule> rules);

// Read ordered RVLCurve references from the parsed plane-specific archive.
// Supports pure line aliases, vertex-line aliases, ruler and shape wrappers.
// Checks local bounds and uniqueness; owner graph resolution is separate.
std::vector<std::int32_t> vector_line_plane_references(const Plane& plane);

// Independent intrusive identity domains of pure/vertex-line aliases. Pure
// lines have no vertex/model owner lists. Bounded suffixes remain in specific.
struct VectorLinePlaneTopology {
    std::vector<std::int32_t> vertices,models,lines;
};
VectorLinePlaneTopology vector_line_plane_topology(const Plane& plane);

// Overwrite exact 64-bit color in the selected live RVLCurve pool records.
// The argument is one unwrapped base-pool body, not a complete layer payload.
// Validates pool framing, record bounds and all requested live indices before
// publishing bytes. RVMCurve graph/ownership resolution is a caller responsibility.
// Does not normalize alpha, change common plane color or resolve other pools.
std::uint64_t set_vector_line_pool_color(Bytes& pool,std::int32_t high_water,
    std::span<const std::int32_t> indices,std::uint64_t color);

// Locate that pool inside a parsed vector owner and publish only its color bytes.
// Supports line/temp, ruler, paint/select and shape owner envelopes. Preserves
// embedded border data and all wrapper suffixes. This validates wrapper framing,
// not the semantics of unrelated pools or global reference ownership.
std::uint64_t set_vector_layer_line_colors(Layer& layer,
    std::span<const std::int32_t> indices,std::uint64_t color);

// Validate direct plane-to-line exclusive ownership and the bidirectional
// RVLCurve/RVMCurve color graph, then return this plane's ordered line indices.
// Vertex-line topology lists resolve their own vertex/model identity domains,
// including hidden siblings and the Paint/Select owner's separate Border.
std::vector<std::int32_t> validated_vector_plane_line_references(
    const Layer& layer,std::size_t plane);
// The native raw64 propagation worker: update the selected plane's common
// color and every directly owned line atomically; no history/cache publication.
bool set_vector_plane_line_color(Layer& layer,std::size_t plane,std::uint64_t color);

} // namespace retas
