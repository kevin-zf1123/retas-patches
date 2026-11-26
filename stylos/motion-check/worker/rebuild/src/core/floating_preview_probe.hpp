#pragma once
#include "retas/floating.hpp"

namespace retas::floating_preview_detail {
// CommandBegin's physical helper-block gate for the admitted normal same-DPI
// General32/64 profile. Temporary helper storage is reserved against available
// bytes and released on return; only work remains consumed. Exceptions leave
// the caller's ledger unchanged and never mean an empty helper.
bool transform_has_blocks(const DocumentData&,const FloatingSelection&,
    std::uint64_t& work_remaining,std::uint64_t bytes_available,std::stop_token stop={});
}
