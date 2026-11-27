#pragma once
#include "retas/vector_tool_selection.hpp"
#include "vector_tool_selection.hpp"

namespace retas::vector_selection_detail {
// Call once for each candidate operation, before transfer. Preparation and
// retained snapshot costs join the tool's existing ledgers without re-running
// preparation. Both counters stay unchanged on failure/cancellation. The
// returned immutable borrow is valid while the public packet remains alive.
const FrozenNativeSelection& reserve_vector_tool_selection(
    const FrozenVectorToolSelection&,std::uint64_t& storage,std::uint64_t& work,
    std::stop_token stop={});
}
