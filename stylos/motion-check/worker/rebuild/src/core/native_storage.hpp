#pragma once
#include "retas/document.hpp"

namespace retas::storage_detail {
void write_checked(const std::filesystem::path&,std::span<const std::uint8_t>,const std::function<void()>& before_replace);
void write_new_checked(const std::filesystem::path&,std::span<const std::uint8_t>,const std::function<void()>& before_publish);
inline void optimize(DocumentData& data){
    for(auto& layer:data.layers)if(!layer.opaque)
        for(auto& plane:layer.planes)if(!plane.opaque&&plane.raster)
            if(plane.raster->optimize_storage())plane.pixels_changed=true;
}
}
