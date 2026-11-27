#pragma once
#include "filter_lut.hpp"
#include "retas/fill.hpp"
#include <functional>
#include <stdexcept>

namespace retas::filter_detail {
struct FilterBinding {std::size_t layer{},plane{};Rect roi;};
struct FilterTargets {
    std::size_t layer{};
    std::vector<FilterBinding> bound;
    std::vector<std::size_t> published;
};
struct FilterWorkerOutcome {FillResult result;bool modified{},worker_succeeded{};};
FilterWorkerOutcome gaussian_document_worker(Plane&,const Layer&,const RegionSelection&,
    std::array<int,2> carrier_size,const GaussianBlurSettings&,std::stop_token,FilterCosts&);
FilterWorkerOutcome table_document_worker(Plane&,const Layer&,const RegionSelection&,
    FilterKind,const FilterSettings&,std::stop_token,FilterCosts&);
FilterWorkerOutcome auto_contrast_document_worker(Plane&,const Layer&,const RegionSelection&,
    std::stop_token,FilterCosts&);
FilterWorkerOutcome hsv_document_worker(Plane&,const Layer&,const RegionSelection&,
    const HsvSettings&,std::stop_token,FilterCosts&);
FilterWorkerOutcome color_balance_document_worker(Plane&,const Layer&,const RegionSelection&,
    const ColorBalanceSettings&,std::stop_token,FilterCosts&);
FilterWorkerOutcome unsharp_document_worker(Plane&,const Layer&,const RegionSelection&,
    const UnsharpMaskSettings&,std::stop_token,FilterCosts&);
FilterWorkerOutcome convolution_document_worker(Plane&,const Layer&,const RegionSelection&,
    FilterKind,std::stop_token,FilterCosts&);
struct FilterPreviewState {
    struct Use {
        bool& active;
        explicit Use(bool& value):active(value){if(active)throw std::logic_error("Filter preview is already in use");active=true;}
        ~Use(){active=false;}
        Use(const Use&)=delete;
    };
    const Document* owner{};
    FilterKind kind{FilterKind::Dust};
    std::optional<std::size_t> batch_owner;
    DocumentData baseline;
    RegionSelection selection;
    std::function<bool(const Document&)> current;
    FilterCosts initial_costs,execution_costs;
    std::optional<DocumentData> output;
    FilterTargets targets;
    FilterDriverResult result;
    bool cached{},ready{},consumed{},busy{};
};
FilterDriverResult filter_document_data(DocumentData&,FilterKind,const FilterSettings&,const RegionSelection&,
    std::stop_token,FilterTargets&,FilterCosts&,bool prebind=true,std::optional<std::size_t> batch_owner={});
std::vector<FilterBinding> filter_preview_bindings(const DocumentData&,FilterKind,const RegionSelection&,std::stop_token,FilterCosts&,std::optional<std::size_t> batch_owner={});
FilterDriverResult run_filter_preview(const std::shared_ptr<FilterPreviewState>&,const FilterSettings&,bool,std::stop_token);
}
