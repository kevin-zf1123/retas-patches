#pragma once
#include "retas/document.hpp"
namespace retas::page_geometry_detail {
std::int32_t inverse(std::int32_t kind);
void apply(DocumentData&,std::int32_t kind,std::uint64_t& bytes,std::uint64_t& work,std::stop_token);
}
