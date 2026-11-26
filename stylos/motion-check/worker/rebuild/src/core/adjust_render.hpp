#pragma once
#include "filter_lut.hpp"
#include <memory>

namespace retas::adjust_render_detail {
using Tables = filter_detail::ByteRgbTables;
using Remap = std::shared_ptr<const Tables>;
using OwnerTables = std::vector<Remap>;
struct TableCache {Tables tables;Bytes specific;};
std::uint64_t cache_storage(const Plane&) noexcept;
std::shared_ptr<const TableCache> fresh_cache(const Plane&);
std::shared_ptr<const TableCache> retain_cache(const Plane&,const Bytes& next);
struct Binding {bool succeeded{};std::shared_ptr<const TableCache> cache;};
Binding bind(const DocumentData&,std::size_t layer,const Plane&,const RegionSelection&,std::stop_token={});
// Indexed by the complete saved owner stack, before request exclusions.
// Empty means that no Adjust owner exists; identity table presence is retained.
OwnerTables prepare(const DocumentData&,std::uint64_t& storage,std::uint64_t& work,std::stop_token={});
OwnerTables prepare(const DocumentData&,std::stop_token={});
inline Pixel general_rgb(Pixel pixel,const Tables* tables){
    if(tables){pixel.r=std::uint16_t((*tables)[0][pixel.r>>8]*257);pixel.g=std::uint16_t((*tables)[1][pixel.g>>8]*257);pixel.b=std::uint16_t((*tables)[2][pixel.b>>8]*257);}
    return pixel;
}
}
