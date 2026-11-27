#pragma once
#include "retas/vector_render.hpp"
namespace retas::vector_archive_detail {
// The caller has decoded/validated the complete owner graph. Projection keeps
// the ordinary strict open-chain/profile domain, and retains requested order.
// Shape/Ruler may admit isolated Model0..3 closed chains and saved
// descriptors; new variable domains need two normalized width nodes per span.
// Archive load does not reconstruct seams.
enum class LineClosurePolicy {OpenOnly,ShapeStoredNull,StoredProfiles,NativeStored};
// Complete stored owner attachment admission, including unused and Border
// records. Nullable adjacency slots are retained; live edges must be reciprocal.
// This does not reconstruct descriptors or constrain cross-Model attachments.
void validate_stored_line_attachments(const VectorLineArchive&);
// NativeStored consumes flags&3 and saved CPs for locally connected chains,
// admitting cross-MC shared vertices with reciprocal stored adjacency. It
// reads descriptors without running edit-time reconstruction.
// StoredProfiles projects saved kind0..4 descriptors into an owning flat
// span sidecar. A non-NULL descriptor needs the sidecar; it is never discarded.
std::vector<VectorLineStroke> project_line_strokes(const VectorLineArchive&,std::span<const std::int32_t>,LineClosurePolicy=LineClosurePolicy::OpenOnly,std::vector<VectorSpanProfiles>* profiles=nullptr);
}
