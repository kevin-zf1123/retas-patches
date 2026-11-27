#include "filter_document.hpp"
#include "filter_tool_kernels.hpp"
#include "modal_document_guard.hpp"
#include "raster_history.hpp"
#include "selection_raster.hpp"
#include <algorithm>
#include <climits>
#include <cmath>

namespace retas::filter_detail {
namespace {
Rect checked_rect(std::int64_t left,std::int64_t top,std::int64_t right,std::int64_t bottom){
    if(left<INT32_MIN||left>INT32_MAX||top<INT32_MIN||top>INT32_MAX||right<INT32_MIN||right>INT32_MAX||bottom<INT32_MIN||bottom>INT32_MAX
        ||right-left>INT32_MAX||bottom-top>INT32_MAX)
        throw std::invalid_argument("Gaussian geometry exceeds int32");
    return {int(left),int(top),int(std::max<std::int64_t>(0,right-left)),int(std::max<std::int64_t>(0,bottom-top))};
}
Rect worker_bounds(const Plane& plane,const Layer& owner,const RegionSelection& selection,std::array<int,2> size){
    // 455CD0 subtracts only owner origin. 61F770 then scales carrier GetRect;
    // the stored selection bbox below is deliberately left at its original DPI.
    const double ratio=plane.common.resolution/owner.common.resolution;
    const auto edge=[&](std::int64_t value){const double v=double(value)*ratio;
        if(!std::isfinite(v)||v<INT32_MIN||v>INT32_MAX)throw std::invalid_argument("Gaussian page mapping exceeds int32");return std::int64_t(v);};
    const auto left=edge(-std::int64_t(owner.common.x)),top=edge(-std::int64_t(owner.common.y));
    const auto right=edge(std::int64_t(size[0])-owner.common.x),bottom=edge(std::int64_t(size[1])-owner.common.y);
    const auto page=checked_rect(left,top,right,bottom);
    if(selection.state==SelectionLayerState::Cleared)return page;
    const auto x=std::int64_t(selection.bounds.x)+selection.origin[0]-owner.common.x;
    const auto y=std::int64_t(selection.bounds.y)+selection.origin[1]-owner.common.y;
    return checked_rect(std::max(left,x),std::max(top,y),std::min(right,x+std::max(0,selection.bounds.width)),
        std::min(bottom,y+std::max(0,selection.bounds.height)));
}
bool rgba_copyback(Raster& image,Rect work,std::stop_token stop,FilterCosts& costs){
    const auto r=gaussian_copy_footprint(work,image);if(r.width<=0||r.height<=0)return false;
    const int left=r.x,top=r.y,right=left+r.width,bottom=top+r.height;
    bool changed{};
    const auto materialize=[&](TileCoord key){
        modal_detail::poll(stop);modal_detail::charge(costs.work,1,modal_detail::max_work,"Gaussian copy exceeds128Mi work");
        if(image.tiles.contains(key))return;
        modal_detail::charge(costs.storage,image.tile_bytes()+128,modal_detail::max_bytes,"Gaussian copy exceeds512MiB");
        modal_detail::charge(costs.work,65536,modal_detail::max_work,"Gaussian copy exceeds128Mi work");
        selection_detail::store_raw_pixel(image,key.x*256,key.y*256,selection_detail::clear_word(image.depth));changed=true;
    };
    // 659120 holds the final scratch write lock and moves its cursor before
    // the final mask gate. Its transparent-white samples still acquire tiles.
    for(int y=top/256;y<=(bottom-1)/256;++y)for(int x=left/256;x<=(right-1)/256;++x)materialize({x,y});
    // Copy32 advances its normal cursor after the last partial-width row.
    // Copy64 uses a different virtual entry and has no such one-past tail.
    if(image.depth==32&&image.width%256&&right==image.width&&bottom<image.height)materialize({image.width/256,bottom/256});
    return changed;
}
}
FilterWorkerOutcome gaussian_document_worker(Plane& plane,const Layer& owner,const RegionSelection& selection,
    std::array<int,2> carrier_size,const GaussianBlurSettings& settings,std::stop_token stop,FilterCosts& costs){
    modal_detail::poll(stop);const auto work=worker_bounds(plane,owner,selection,carrier_size);FilterWorkerOutcome outcome;
    // 65C0A0 reports false only for an empty nonzero-state page intersection.
    if(selection.state!=SelectionLayerState::Cleared&&(work.width<=0||work.height<=0))return outcome;
    auto& image=*plane.raster;
    const double radius=settings.use_millimeters?(image.resolution*settings.size_mm)*0x1.42850a142850ap-5:double(settings.size_px);
    if(!std::isfinite(radius)||radius<0||radius>double(INT_MAX/2-1))throw std::invalid_argument("Gaussian radius exceeds signed support domain");
    if(low_convolution_depth(image.depth)){
        const auto result=apply_low_gaussian(image,settings,work,stop,&costs,true);
        outcome.result.handled=!result.rejected();outcome.result.cancelled=result.cancelled();outcome.result.error=result.error;
        outcome.result.changed_pixels=result.changed_pixels;outcome.result.changed=result.changed_pixels!=0;
        outcome.result.storage_changed=result.modified();outcome.modified=result.modified();
    }else {
        const auto count=image.tiles.size();
        const auto coverage=(selection.state==SelectionLayerState::Mask||selection.state==SelectionLayerState::VectorIndicated)
            ?project_selection(selection,{owner.common.x,owner.common.y},image.width,image.height):std::nullopt;
        outcome.result=detail::gaussian_region(image,plane.common.opacity_mode,radius,work,nullptr,stop,coverage,&costs,true);
        if(outcome.result.handled&&!outcome.result.cancelled){
            const bool copied=rgba_copyback(image,work,stop,costs);
            outcome.result.storage_changed=copied||image.tiles.size()!=count;
            outcome.modified=outcome.result.changed||outcome.result.storage_changed;
        }
    }
    // The normal outer tail ignores the lower worker's Boolean. Modern input,
    // resource and cancellation errors remain atomic rejections, not that tail.
    outcome.worker_succeeded=true;outcome.result.performed=true;return outcome;
}
}
