#pragma once
#include "retas/floating.hpp"

namespace retas::floating_detail {
// Document composition inserts source history bindings between the transfer
// and its single615390 tail. Both calls advance the same used ledger.
struct Content {FloatingCapture capture;std::vector<FloatingSource> sources;};
Content capture_content(const DocumentData&,const RegionSelection&,
    std::uint64_t& used_bytes,std::uint64_t& used_work,std::stop_token={},bool current_only=false);
void finish_capture(FloatingCapture&,const RegionSelection&,bool active,
    std::uint64_t& used_bytes,std::uint64_t& used_work,std::stop_token={});
}
