#pragma once
#include "document.hpp"
namespace retas {
struct VectorIndicationUpdate {
    DocumentData data;
    bool active{},redraw{};
};
//Native4401B0: gated main-owner/member clearing. Source remains immutable.
//The returned Doc+284 is false even if orphan/unsupported markers remain.
VectorIndicationUpdate prepare_vector_indication_clear(const DocumentData&,bool active,std::stop_token stop={});
//Native615390: clear, then mark current eligible owners from the real bitmap
//or rectangle using current application flags. State0/bit0 still clear first.
//Carrier DPI is explicit, independent of owner dimensions and bbox metadata.
VectorIndicationUpdate prepare_vector_indication_refresh(const DocumentData&,bool active,
    const RegionSelection&,std::uint32_t application_flags,double actual_carrier_dpi,std::stop_token stop={});
}
