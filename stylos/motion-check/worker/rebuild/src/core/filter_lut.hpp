#pragma once
#include "retas/filters.hpp"

namespace retas::filter_detail {
using ByteRgbTables = std::array<std::array<std::uint8_t,256>,3>;
// Validate the existing typed filter settings before binding or execution.
void validate_filter_settings(FilterKind,const FilterSettings&);
// Capture the bounded point records before the first allocating copy.
ToneCurveSettings freeze_tone_curve_settings(const ToneCurveSettings&);
// CAdjustPlane and ordinary table wrappers bind BYTE tables on WORD owners.
ByteRgbTables byte_rgb_tables(FilterKind,const FilterSettings&);
inline bool table_filter(FilterKind kind){return kind==FilterKind::Contrast||kind==FilterKind::Level||kind==FilterKind::ToneCurve;}
inline bool fixed_convolution(FilterKind kind){return kind==FilterKind::SharpSoft||kind==FilterKind::SharpHard||kind==FilterKind::BlurSoft||kind==FilterKind::BlurHard;}
inline bool low_convolution_depth(int depth){return depth==1||depth==2||depth==8||depth==16||depth==24;}
struct FilterCosts {std::uint64_t storage{},work{};};
FilterResult apply_dust(Raster&,const FilterSettings&,const std::optional<Rect>&,
    const FilterExecutionOptions&,std::stop_token,const std::optional<RasterSelection>&,
    FilterCosts* =nullptr,double owner_dpi=0,double plane_dpi=0,const Raster* companion=nullptr);
FilterResult apply_color_balance(Raster&,const FilterSettings&,const std::optional<Rect>&,
    std::stop_token,const std::optional<RasterSelection>&,FilterCosts* =nullptr);
FilterResult apply_low_gaussian(Raster&,const FilterSettings&,const std::optional<Rect>&,
    std::stop_token,FilterCosts* =nullptr,bool native_bounds=false);
Rect gaussian_copy_footprint(Rect,const Raster&);
FilterResult apply_unsharp(Raster&,const FilterSettings&,const std::optional<Rect>&,
    std::stop_token,const std::optional<RasterSelection>&,FilterCosts* =nullptr);
FilterResult apply_hsv(Raster&,const FilterSettings&,const std::optional<Rect>&,
    std::stop_token,const std::optional<RasterSelection>&,FilterCosts* =nullptr);
FilterResult apply_low_convolution(Raster&,FilterKind,const FilterSettings&,const std::optional<Rect>&,
    std::stop_token,const std::optional<RasterSelection>&,FilterCosts* =nullptr);
FilterResult apply_auto_contrast(Raster&,const FilterSettings&,const std::optional<Rect>&,
    const FilterExecutionOptions&,std::stop_token,const std::optional<RasterSelection>&,FilterCosts* =nullptr);
// Null bounds means native state0/1; present bounds or coverage is state>=2.
FilterResult apply_rgb24(Raster&,FilterKind,const FilterSettings&,const std::optional<Rect>&,
    const FilterExecutionOptions&,std::stop_token,const std::optional<RasterSelection>&,FilterCosts* =nullptr);
}
