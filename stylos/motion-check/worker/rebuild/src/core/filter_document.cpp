#include "filter_dust.hpp"
#include "modal_document_guard.hpp"
#include "raster_history.hpp"
#include <cmath>
#include <climits>

namespace retas {
namespace {
int family_group(std::uint32_t family){
    switch(family){
    case 0x10000100:case 0x10000400:case 0x11000800:case 0x11001000:case 0x18004000:return 1;
    case 0x10008000:case 0x12000200:case 0x14002000:return 2;
    default:return 0;
    }
}
void validate_plane(const Plane& p,bool match_dpi=true){
    if(p.opaque||!p.raster)throw std::invalid_argument("Filter requires decoded raster children");
    const auto& r=*p.raster;
    if(!(p.family&0x10000000u)||p.common.width!=r.width||p.common.height!=r.height||p.common.depth!=r.depth||(match_dpi&&p.common.resolution!=r.resolution)
        ||!std::isfinite(p.common.resolution)||p.common.resolution<=0
        ||!std::isfinite(r.resolution)||r.resolution<=0)throw std::invalid_argument("Filter plane and offscreen metadata differ");
    const auto depth=r.depth;
    if(!(depth==1||depth==2||depth==8||depth==16||depth==24||depth==32||depth==64)||r.color_order!=(depth==24?4:depth>=32?3:0))
        throw std::invalid_argument("Filter requires canonical raster storage");
    raster_history_detail::validate(r);
}
Rect mapped(Rect b,const RegionSelection& selection,const Layer& owner,double dpi){
    const double ratio=dpi/owner.common.resolution;
    const auto edge=[&](std::int64_t coordinate,int axis){
        const double value=double(coordinate+std::int64_t(selection.origin[axis])-(axis?owner.common.y:owner.common.x))*ratio;
        if(!std::isfinite(value)||value<INT32_MIN||value>INT32_MAX)throw std::invalid_argument("Filter ROI mapping exceeds int32");return std::int64_t(value);
    };
    const auto l=edge(b.x,0),t=edge(b.y,1),r=edge(std::int64_t(b.x)+std::max(0,b.width),0),d=edge(std::int64_t(b.y)+std::max(0,b.height),1);
    if(r-l>INT32_MAX||d-t>INT32_MAX)throw std::invalid_argument("Filter ROI extent exceeds int32");return {int(l),int(t),int(r-l),int(d-t)};
}
}
bool can_apply_dust_filter(const DocumentData& data){return can_apply_document_filter(data,FilterKind::Dust);}
bool document_filter_executes_on_layer(FilterKind kind,std::uint32_t family){
    if(kind==FilterKind::Dust)return family_group(family)!=0;
    return (kind==FilterKind::Invert||kind==FilterKind::GaussianBlur||kind==FilterKind::AutoContrast||kind==FilterKind::Hsv||kind==FilterKind::ColorBalance||kind==FilterKind::UnsharpMask||filter_detail::fixed_convolution(kind)||filter_detail::table_filter(kind))&&(family==0x10000100||family==0x11000800||family==0x11001000||family==0x18004000);
}
bool can_apply_document_filter(const DocumentData& data,FilterKind kind){
    if(kind!=FilterKind::Dust&&kind!=FilterKind::GaussianBlur&&kind!=FilterKind::AutoContrast&&kind!=FilterKind::Hsv&&kind!=FilterKind::ColorBalance&&kind!=FilterKind::UnsharpMask&&!filter_detail::fixed_convolution(kind)&&!filter_detail::table_filter(kind))return false;
    if(data.current_layer<0||data.current_layer>=int(data.layers.size()))return false;
    const auto& owner=data.layers[std::size_t(data.current_layer)];const auto group=family_group(owner.family);
    if(owner.family==0x40000200){
        if(owner.opaque||!filter_detail::table_filter(kind)||!owner.common.flags[1])return false;
        for(const auto& plane:owner.planes)if(plane.common.flags[1]&&plane.common.flags[2]&&plane.common.runtime_operable)return true;
        return false;
    }
    if(owner.opaque||!group||(kind!=FilterKind::Dust&&group!=1)||!owner.common.flags[1]||owner.current_plane<0||owner.current_plane>=int(owner.planes.size()))return false;
    if((kind==FilterKind::Dust||kind==FilterKind::AutoContrast)&&group==1&&owner.planes[std::size_t(owner.current_plane)].common.opacity_mode==4)return false;
    for(const auto& plane:owner.planes)if(plane.common.flags[1]&&plane.common.flags[2]&&plane.common.runtime_operable)return true;
    return false;
}
DustFilterResult filter_detail::dust_filter_data(DocumentData& data,const DustSettings& settings,const RegionSelection& selection,
    std::stop_token stop,DustFilterTargets& capture,FilterCosts& costs,bool prebind){
    return filter_document_data(data,FilterKind::Dust,settings,selection,stop,capture,costs,prebind);
}
FilterDriverResult filter_detail::filter_document_data(DocumentData& data,FilterKind kind,const FilterSettings& settings,const RegionSelection& selection,
    std::stop_token stop,FilterTargets& capture,FilterCosts& costs,bool prebind,std::optional<std::size_t> batch_owner){
    try{
        modal_detail::poll(stop);
        if((!batch_owner&&!can_apply_document_filter(data,kind))||!settings_match_filter(kind,settings))throw std::invalid_argument("Filter has no admitted owner or settings");
        const auto layer=batch_owner.value_or(data.current_layer<0?data.layers.size():std::size_t(data.current_layer));
        if(layer>=data.layers.size())throw std::invalid_argument("Filter owner index is invalid");const auto& source=data.layers[layer];
        if(source.opaque||source.planes.empty()||source.current_plane<0||std::size_t(source.current_plane)>=source.planes.size())throw std::invalid_argument("Filter owner has no decoded current child");
        if(!document_filter_executes_on_layer(kind,source.family))throw std::invalid_argument("Ordinary filter requires an execution owner");
        const auto owner_dpi=source.common.resolution;
        if(!std::isfinite(owner_dpi)||owner_dpi<=0)throw std::invalid_argument("Invalid filter owner DPI");
        if(kind==FilterKind::Dust)(void)dust_size(std::get<DustSettings>(settings),owner_dpi,owner_dpi);
        else if(kind==FilterKind::GaussianBlur){const auto& value=std::get<GaussianBlurSettings>(settings);
            if(!std::isfinite(value.size_mm)||value.size_mm<0||value.size_mm>1000||value.size_px<0||value.size_px>20000)
                throw std::invalid_argument("Invalid Gaussian settings");}
        else validate_filter_settings(kind,settings);
        if(int(selection.state)<0||int(selection.state)>4)throw std::invalid_argument("Invalid filter selection state");
        const auto size=selection_carrier_size(selection,data.base.width(),data.base.height());
        const auto carrier_dpi=selection_carrier_dpi(selection,data.base.dpi);
        const bool cleanup=selection.state==SelectionLayerState::Mask||selection.state==SelectionLayerState::VectorIndicated;
        if(cleanup&&!selection.mask)throw std::invalid_argument("Filter selection has no physical mask backing");
        const auto charge=[&](std::uint64_t bytes){modal_detail::poll(stop);modal_detail::charge(costs.storage,bytes,modal_detail::max_bytes,"Filter driver exceeds512MiB");};
        charge(source.planes.size()*(sizeof(DustBinding)+sizeof(std::size_t)));
        DustFilterTargets prepared;prepared.layer=layer;prepared.bound.reserve(source.planes.size());prepared.published.reserve(source.planes.size());
        std::optional<Rect> common_roi;
        if(prebind&&selection.state!=SelectionLayerState::Cleared){
            validate_plane(source.planes.front(),kind==FilterKind::Dust);
            common_roi=raster_history_detail::clipped(mapped(selection.bounds,selection,source,source.planes.front().common.resolution),*source.planes.front().raster);
        }
        const bool bind=prebind&&(!common_roi||(common_roi->width>0&&common_roi->height>0));
        if(bind)for(std::size_t i=0;i<source.planes.size();++i){
            modal_detail::poll(stop);validate_plane(source.planes[i],kind==FilterKind::Dust);const auto& raster=*source.planes[i].raster;
            prepared.bound.push_back({layer,i,common_roi?raster_history_detail::clipped(*common_roi,raster):Rect{0,0,raster.width,raster.height}});
            prepared.published.push_back(i);
        }
        modal_detail::data_charge(data,costs.storage,0,false);auto staged=data;auto& owner=staged.layers[layer];
        for(const auto& binding:prepared.bound){auto& plane=owner.planes[binding.plane];plane.pixels_changed=true;plane.raster_modified=true;}
        DustFilterResult result;std::size_t executed{};
        const auto execute=[&](std::size_t index){
            modal_detail::poll(stop);auto& plane=owner.planes[index];validate_plane(plane,kind==FilterKind::Dust);
            if(!bind)prepared.published.push_back(index);
            FilterWorkerOutcome outcome;
            if(kind==FilterKind::GaussianBlur)outcome=gaussian_document_worker(plane,owner,selection,size,std::get<GaussianBlurSettings>(settings),stop,costs);
            else if(kind==FilterKind::AutoContrast)outcome=auto_contrast_document_worker(plane,owner,selection,stop,costs);
            else if(kind==FilterKind::Hsv)outcome=hsv_document_worker(plane,owner,selection,std::get<HsvSettings>(settings),stop,costs);
            else if(kind==FilterKind::ColorBalance)outcome=color_balance_document_worker(plane,owner,selection,std::get<ColorBalanceSettings>(settings),stop,costs);
            else if(kind==FilterKind::UnsharpMask)outcome=unsharp_document_worker(plane,owner,selection,std::get<UnsharpMaskSettings>(settings),stop,costs);
            else if(kind==FilterKind::Invert||fixed_convolution(kind))outcome=convolution_document_worker(plane,owner,selection,kind,stop,costs);
            else if(table_filter(kind))outcome=table_document_worker(plane,owner,selection,kind,settings,stop,costs);
            else {
                const auto bounds=selection.state==SelectionLayerState::Cleared?Rect{0,0,size[0],size[1]}:selection.bounds;
                const auto work=mapped(bounds,selection,owner,plane.common.resolution);
                std::vector<DustMask> masks;
                if(cleanup){charge(sizeof(DustMask));masks.push_back({size[0],size[1],{std::int64_t(owner.common.x)-selection.origin[0],std::int64_t(owner.common.y)-selection.origin[1]},
                    owner_dpi/plane.common.resolution,plane.common.resolution/carrier_dpi,[mask=selection.mask](int x,int y){return mask->test(x,y);}});}
                const Raster* companion{};
                if(plane.family==0x12100001)for(const auto& other:owner.planes)if(other.family==0x11110002){if(other.common.flags[1]){validate_plane(other);companion=&*other.raster;}break;}
                const auto& dust=std::get<DustSettings>(settings);
                outcome=dust_region(*plane.raster,int(dust.mode),dust_size(dust,owner_dpi,plane.common.resolution),plane.common.opacity_mode==5,work,masks,stop,companion,&costs);
            }
            if(outcome.result.cancelled)throw std::runtime_error("Filter cancelled");
            if(!outcome.result.handled)throw std::runtime_error(outcome.result.error);
            plane.pixels_changed|=outcome.modified;result.changed_pixels+=outcome.result.changed_pixels;result.storage_changed|=outcome.result.storage_changed;
            if(!outcome.worker_succeeded){result.status=DustFilterStatus::WorkerFailed;return false;}
            ++executed;return true;
        };
        if((batch_owner||owner.common.flags[2])&&owner.common.flags[1]&&owner.common.runtime_operable){
            if(owner.planes[std::size_t(owner.current_plane)].common.opacity_mode==4){
                if(owner.planes[std::size_t(owner.current_plane)].common.flags[1])execute(std::size_t(owner.current_plane));
            }else for(std::size_t i=0;i<owner.planes.size();++i)if(owner.planes[i].common.flags[1]&&(batch_owner||owner.planes[i].common.flags[2]))if(!execute(i))break;
        }
        if(result.status!=DustFilterStatus::WorkerFailed&&executed)result.status=DustFilterStatus::Applied;
        modal_detail::poll(stop);
        for(const auto index:prepared.published){auto& live=data.layers[layer].planes[index];auto& output=owner.planes[index];*live.raster=std::move(*output.raster);live.pixels_changed=output.pixels_changed;live.raster_modified=output.raster_modified;}
        capture=std::move(prepared);return result;
    }catch(const std::exception& error){DustFilterResult result;result.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())result.error=error.what();return result;}
}
std::vector<filter_detail::FilterBinding> filter_detail::filter_preview_bindings(const DocumentData& data,FilterKind kind,const RegionSelection& selection,
    std::stop_token stop,FilterCosts& costs,std::optional<std::size_t> batch_owner){
    const auto layer=batch_owner.value_or(data.current_layer<0?data.layers.size():std::size_t(data.current_layer));
    if(layer>=data.layers.size())throw std::invalid_argument("Filter preview has no owner");
    if(int(selection.state)<0||int(selection.state)>4)throw std::invalid_argument("Invalid filter selection state");
    const auto& owner=data.layers[layer];
    if(!std::isfinite(owner.common.resolution)||owner.common.resolution<=0)throw std::invalid_argument("Invalid filter owner DPI");
    modal_detail::charge(costs.storage,owner.planes.size()*sizeof(DustBinding),modal_detail::max_bytes,"Filter preview bindings exceed512MiB");
    std::vector<DustBinding> result;result.reserve(owner.planes.size());
    for(std::size_t i=0;i<owner.planes.size();++i){modal_detail::poll(stop);const auto& plane=owner.planes[i];if(!(plane.family&0x10000000u))continue;validate_plane(plane,kind==FilterKind::Dust);
        const auto& image=*plane.raster;const auto roi=selection.state==SelectionLayerState::Cleared?Rect{0,0,image.width,image.height}:
            raster_history_detail::clipped(mapped(selection.bounds,selection,owner,plane.common.resolution),image);
        if(roi.width>0&&roi.height>0)result.push_back({layer,i,roi});
    }
    return result;
}
bool DustFilterPreview::ready() const noexcept {return state_&&state_->ready;}
DustFilterResult DustFilterPreview::run(const DustSettings& settings,bool cached_preview,std::stop_token stop){
    return filter_detail::run_filter_preview(state_,settings,cached_preview,stop);
}
bool FilterPreview::ready() const noexcept {return state_&&state_->ready;}
std::array<std::uint64_t,2> FilterPreview::resources()const noexcept {return state_?std::array<std::uint64_t,2>{state_->execution_costs.storage,state_->execution_costs.work}:std::array<std::uint64_t,2>{};}
FilterDriverResult FilterPreview::run(const FilterSettings& settings,bool cached_preview,std::stop_token stop){
    return filter_detail::run_filter_preview(state_,settings,cached_preview,stop);
}
FilterDriverResult filter_detail::run_filter_preview(const std::shared_ptr<FilterPreviewState>& input,const FilterSettings& settings,bool cached_preview,std::stop_token stop){
    const auto state=input;
    if(!state||state->consumed){DustFilterResult result;result.status=DustFilterStatus::Rejected;result.error="Filter preview has no unused capture";return result;}
    try{
        FilterPreviewState::Use use(state->busy);state->ready=false;state->output.reset();state->targets={};
        if(state->batch_owner&&cached_preview)throw std::invalid_argument("Batch filters require direct execution");
        if(!settings_match_filter(state->kind,settings))throw std::invalid_argument("Filter preview settings do not match its captured kind");
        modal_detail::poll(stop);auto costs=state->initial_costs;modal_detail::data_charge(state->baseline,costs.storage,0,false);
        if(state->kind==FilterKind::ToneCurve)modal_detail::charge(costs.storage,4*(sizeof(ToneCurveSettings)+4*32*sizeof(ToneCurvePoint)),modal_detail::max_bytes,"Tone curve captures exceed512MiB");
        const auto request=state->kind==FilterKind::ToneCurve?FilterSettings{freeze_tone_curve_settings(std::get<ToneCurveSettings>(settings))}:settings;
        auto output=state->baseline;filter_detail::DustFilterTargets targets;
        auto result=filter_document_data(output,state->kind,request,state->selection,stop,targets,costs,!cached_preview,state->batch_owner);
        if(result.rejected()||result.cancelled())return result;
        modal_detail::poll(stop);state->output=std::move(output);state->targets=std::move(targets);state->execution_costs=costs;
        state->result=result;state->cached=cached_preview;state->ready=true;return result;
    }catch(const std::exception& error){DustFilterResult result;result.status=stop.stop_requested()?DustFilterStatus::Cancelled:DustFilterStatus::Rejected;if(!stop.stop_requested())result.error=error.what();return result;}
}
}
