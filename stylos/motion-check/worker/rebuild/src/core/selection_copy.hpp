#pragma once
#include "retas/clipboard.hpp"

namespace retas::selection_copy_detail {
Raster raster_payload(const Layer&,const Plane&,const RegionSelection&,std::array<int,2>,
    std::uint64_t& bytes,std::uint64_t& work,std::stop_token,const Plane* companion=nullptr);
bool query(const DocumentData&,const RegionSelection&,std::uint32_t flags,bool cutting=false) noexcept;
std::shared_ptr<const SelectionCopyPacket> selected(DocumentData&,const RegionSelection&,const SelectionCopyOptions&,
    bool& indication_active,std::uint64_t& bytes,std::uint64_t& work,std::stop_token);
std::shared_ptr<const SelectionCopyPacket> live(const FloatingSelection&,bool source_is_current,
    std::uint64_t& bytes,std::uint64_t& work,std::stop_token);
}
