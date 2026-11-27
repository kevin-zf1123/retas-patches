#pragma once
#include "filter_lut.hpp"
#include "filter_document.hpp"
#include "retas/fill.hpp"
#include "retas/filter_tool_settings.hpp"
#include <functional>
#include <span>

namespace retas::filter_detail {
// Carrier coordinates = owner coordinates + offset. Both transforms are
// explicit: the request uses owner/plane DPI, cell replication uses mask DPI.
struct DustMask {
    int width{},height{};
    std::array<std::int64_t,2> offset{};
    double request_scale{1},cell_scale{1};
    std::function<bool(int,int)> test;
};
// Native success is independent of stored differences and positive damage.
using DustOutcome=FilterWorkerOutcome;
// Converted size, native component damage, and cumulative costs are shared
// by ordinary FILTER_DUST and the active range tool. Masks execute in order.
DustOutcome dust_region(Raster&,int type,int size,bool white,Rect,
    std::span<const DustMask>,std::stop_token,const Raster* companion=nullptr,
    FilterCosts* costs=nullptr);
using DustBinding=FilterBinding;
using DustFilterTargets=FilterTargets;
int dust_size(const DustSettings&,double owner_dpi,double plane_dpi);
DustFilterResult dust_filter_data(DocumentData&,const DustSettings&,const RegionSelection&,
    std::stop_token,DustFilterTargets&,FilterCosts&,bool prebind=true);
// Return every original work ROI, including empty intersections. Shared costs
// let Document preparation and execution enforce one cumulative operation cap.
FillResult dust_tool_data(DocumentData&,const FilterToolSettings&,bool metric,
    const std::optional<Rect>&,const BinaryMask*,std::stop_token,
    const std::optional<RasterSelection>&,std::vector<DustBinding>* =nullptr,
    FilterCosts* =nullptr);

}
