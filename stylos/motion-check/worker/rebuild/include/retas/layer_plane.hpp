#pragma once
#include "document.hpp"
#include "adjust_plane.hpp"
#include <string_view>

namespace retas {
struct ElementFamily {std::uint32_t id;std::string_view name;};
std::span<const ElementFamily> new_layer_families();
std::vector<std::uint32_t> new_layer_families(const DocumentData&);
// Native document mode follows its successful open/save path, not CEL flags.
std::vector<std::uint32_t> new_layer_families(const Document&);
bool reset_plane_alpha_editing(DocumentData&) noexcept;
std::string_view layer_family_name(std::uint32_t);
std::string_view plane_family_name(std::uint32_t);
std::vector<std::uint32_t> new_plane_families(const Layer&);
std::vector<std::uint32_t> convert_plane_families(const Layer&);
std::vector<std::uint32_t> convert_layer_families(const Layer&);
std::vector<std::uint32_t> convert_layer_families(const DocumentData&);
bool plane_conversion_keep_source(const Layer&,std::uint32_t target,bool requested);
bool layer_conversion_keep_source(const DocumentData&,std::uint32_t target,bool requested);
struct LayerCreateOptions {
    std::uint32_t family{0x10000100};
    int depth{32};Bytes name;
    std::uint64_t color{0xffff000000000000ull};
    AdjustParameters adjust;
};
Layer make_layer(const BaseInfo&,const LayerCreateOptions&);
Plane make_plane(const Layer&,std::uint32_t family,Bytes name={},std::uint64_t color=0xffff000000000000ull,
                 const AdjustParameters& adjust={});
enum class ElementConvertTextFonts { None, Qt };
struct ElementConvertOptions {
    std::uint32_t family{};Bytes name;bool keep_source{};int depth{32};
    // Optional, lazy native Text mask consumer for ordinary Text/Indication.
    // Admitted Qt work requires the font backend main-thread/application contract.
    ElementConvertTextFonts text_fonts{ElementConvertTextFonts::None};
};
// Candidate workers publish only complete results; Document owns history.
bool convert_layer(DocumentData&,const ElementConvertOptions&,bool batch=false);
bool convert_planes(DocumentData&,const ElementConvertOptions&);
bool merge_same_layers(DocumentData&);
bool merge_same_planes(DocumentData&);
bool can_set_current_plane(const DocumentData&) noexcept;
}
