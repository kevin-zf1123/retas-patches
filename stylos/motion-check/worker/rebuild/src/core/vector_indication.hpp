#pragma once
#include <cstdint>
#include <stop_token>

namespace retas {struct DocumentData;struct RegionSelection;}
namespace retas::vector_indication_detail {
// Mutate a private candidate using the caller's cumulative ledgers. Source
// copying/metadata are charged by the caller. Counters publish only on success.
void clear_marks(DocumentData&,bool active,std::uint64_t& storage,std::uint64_t& work,std::stop_token);
// Return the resulting global state; redraw is old active OR this result.
bool refresh_marks(DocumentData&,bool active,const RegionSelection&,std::uint32_t flags,
    double carrier_dpi,std::uint64_t& storage,std::uint64_t& work,std::stop_token);
}
