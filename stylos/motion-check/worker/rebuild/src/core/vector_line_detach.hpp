#pragma once
#include "vector_graph_edit.hpp"
namespace retas::vector_line_detail {
struct LineDetachResult {
    bool detached{},source_released{};
    int source_line{-1},created_line{-1};
};
//5B59D0: detach one MC from its LC; a middle member moves the following
// members to a fresh LC inserted immediately before the source in its real
// Line-plane owner. The return is a native nullable new-LC identity: callers
// append it to their temporary pointer array separately from owner ordering.
// Border geometry ownership stays in Editor. Optional mirrored_owner_lines
// borrows the same current Line-plane list (e.g. scratch Editor::owner.lines)
// and is checked before edits, then synchronized with the persistent owner.
// Modern admission requires unique MC identities throughout the source LC:
// repeated target/peer members are rejected before graph/allocator/budget edits.
// Candidate mutation only: atomic publication/rollback belongs to the caller.
LineDetachResult detach_model_from_line(vector_graph_detail::Editor&,int model,
    std::map<int,std::size_t>& line_planes,
    std::vector<int>* mirrored_owner_lines=nullptr);
}
