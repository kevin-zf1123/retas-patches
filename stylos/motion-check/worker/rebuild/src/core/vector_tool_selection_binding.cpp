#include "vector_tool_selection_binding.hpp"
#include "retas/fill.hpp"

namespace retas {
namespace {
constexpr std::uint64_t max_storage=512ull*1024*1024,max_wire=8ull*1024*1024;
using vector_probe_detail::Budget;
using vector_material_detail::charge;
struct Ledger {
    std::uint64_t storage{},work{};
    std::stop_token stop;
    Budget budget(){return {work,stop};}
    void retain(std::uint64_t bytes){budget().charge(bytes+1);charge(storage,bytes,max_storage,"Selection binding storage exceeds512MiB");}
};
std::uint64_t mask_bytes(const BinaryMask& mask){
    // BinaryMask owns packed32-bit rows and256x256 allocation flags. Its copy
    // constructor preserves both, including present-zero blocks and padding.
    if(mask.width()<0||mask.height()<0||mask.width()>100000||mask.height()>100000||
       std::uint64_t(mask.width())*std::uint64_t(mask.height())>128ull*1024*1024)
        throw std::length_error("Selection binding mask carrier exceeds128Mi pixels");
    return sizeof(BinaryMask)+((std::uint64_t(mask.width())+31)/32)*std::uint64_t(mask.height())*4+
        ((std::uint64_t(mask.width())+255)/256)*((std::uint64_t(mask.height())+255)/256);
}
RegionSelection snapshot(const RegionSelection& source,Ledger& ledger){
    ledger.retain(2*sizeof(RegionSelection)+1024);
    RegionSelection result=source;
    if(source.mask){ledger.retain(2*mask_bytes(*source.mask));result.mask=std::make_shared<const BinaryMask>(*source.mask);ledger.budget().charge();}
    return result;
}
void common_reservation(const CommonElement& common,Ledger& ledger){
    const auto bytes=std::uint64_t(common.name.size())+common.suffix.size();
    if(bytes>max_wire)throw std::length_error("Selection binding common metadata exceeds8MiB");
    // Native preparation copies Common into owner, Border and temporary
    // planes before its graph counts pass; reserve those payloads first.
    ledger.retain(4*bytes+4*sizeof(CommonElement));
}
void cache_reservation(const Layer& owner,Ledger& ledger){
    if(owner.opaque||owner.family!=0x20800000||owner.planes.size()!=1||owner.planes[0].opaque||owner.planes[0].raster||owner.planes[0].family!=0x22100001)
        throw std::invalid_argument("Selection binding requires actual208 Paint0-only cache");
    std::uint64_t bytes=sizeof(Layer)+sizeof(Plane);
    const auto add=[&](std::uint64_t n){charge(bytes,n,max_wire,"Selection binding cached wire/metadata exceeds8MiB");};
    for(const auto* p:{&owner.common.name,&owner.common.suffix,&owner.specific,&owner.suffix,&owner.plane_suffix,&owner.opaque_record,
                      &owner.planes[0].common.name,&owner.planes[0].common.suffix,&owner.planes[0].specific,&owner.planes[0].suffix,&owner.planes[0].opaque_record})add(p->size());
    // Typed decode and subsequent freeze each own separate temporary payloads.
    // Charge before either decoder allocates from untrusted stored counts.
    ledger.retain(16*bytes+4096*512);ledger.budget().charge(8*bytes);
}
}
struct FrozenVectorToolSelection::State {
    RegionSelection snapshot;
    std::shared_ptr<const BinaryMask> source_mask;
    vector_selection_detail::FrozenNativeSelection native;
    std::uint64_t storage{},work{};
    State(RegionSelection source,std::shared_ptr<const BinaryMask> identity,
          vector_selection_detail::FrozenNativeSelection frozen,Ledger ledger)
        :snapshot(std::move(source)),source_mask(std::move(identity)),native(std::move(frozen)),storage(ledger.storage),work(ledger.work){}
};
FrozenVectorToolSelection::FrozenVectorToolSelection(std::shared_ptr<const State> state):state_(std::move(state)){}
const RegionSelection& FrozenVectorToolSelection::source_snapshot()const noexcept{return state_->snapshot;}
bool FrozenVectorToolSelection::matches_source(const RegionSelection& source)const noexcept{
    const auto& frozen=state_->snapshot;
    return source.state==frozen.state&&source.bounds==frozen.bounds&&source.origin==frozen.origin&&source.carrier_size==frozen.carrier_size&&source.carrier_dpi==frozen.carrier_dpi&&source.mask==state_->source_mask&&source.vector_cache==frozen.vector_cache&&source.vector_cache_origin==frozen.vector_cache_origin&&source.vector_flags==frozen.vector_flags;
}
bool FrozenVectorToolSelection::bypass()const noexcept{return state_->native.bypass();}
bool FrozenVectorToolSelection::available()const noexcept{return state_->native.available();}
FrozenVectorToolSelection prepare_vector_tool_selection(const RegionSelection& source,const CommonElement& common,double dpi,std::stop_token stop){
    Ledger ledger{0,0,stop};ledger.budget().charge();auto frozen_source=snapshot(source,ledger);
    if(source.state!=SelectionLayerState::Cleared&&source.state!=SelectionLayerState::FullPlane)common_reservation(common,ledger);
    auto native=vector_selection_detail::prepare(frozen_source,common,dpi,ledger.storage,ledger.budget());ledger.budget().charge();
    auto state=std::make_shared<const FrozenVectorToolSelection::State>(std::move(frozen_source),source.mask,std::move(native),ledger);vector_material_detail::poll(stop);
    return FrozenVectorToolSelection(std::move(state));
}
FrozenVectorToolSelection prepare_gradation_vector_tool_selection(const RegionSelection& source,const CommonElement& common,double dpi,std::stop_token stop){
    Ledger ledger{0,0,stop};ledger.budget().charge();auto frozen_source=snapshot(source,ledger);
    if(source.state!=SelectionLayerState::Cleared)common_reservation(common,ledger);
    auto projected=frozen_source;
    if(projected.state==SelectionLayerState::FullPlane){
        const auto size=selection_carrier_size(projected,common.width,common.height);
        projected.state=SelectionLayerState::Rectangle;projected.bounds={0,0,size[0],size[1]};projected.mask.reset();
    }
    auto native=vector_selection_detail::prepare(projected,common,dpi,ledger.storage,ledger.budget());ledger.budget().charge();
    auto state=std::make_shared<const FrozenVectorToolSelection::State>(std::move(frozen_source),source.mask,std::move(native),ledger);vector_material_detail::poll(stop);
    return FrozenVectorToolSelection(std::move(state));
}
FrozenVectorToolSelection freeze_cached_vector_tool_selection(const Layer& actual208,std::array<int,2> origin,const RegionSelection& source,std::stop_token stop){
    Ledger ledger{0,0,stop};ledger.budget().charge();
    if(source.state!=SelectionLayerState::VectorIndicated)throw std::invalid_argument("Cached selection binding requires explicitstate4 snapshot");
    cache_reservation(actual208,ledger);auto frozen_source=snapshot(source,ledger);
    auto graph=decode_vector_paint_archive(actual208);ledger.budget().charge();
    //616950 state4 returns the stored cache directly. No trace or sampling
    // occurs here; private freeze validates all graph/material/owner contracts.
    auto native=vector_selection_detail::freeze_cache(graph,actual208,origin,ledger.storage,ledger.budget());ledger.budget().charge();
    auto state=std::make_shared<const FrozenVectorToolSelection::State>(std::move(frozen_source),source.mask,std::move(native),ledger);vector_material_detail::poll(stop);
    return FrozenVectorToolSelection(std::move(state));
}
namespace vector_selection_detail {
}
RegionSelection vector_indicated_selection(const Layer& cache,std::array<int,2> origin,RegionSelection source,std::stop_token stop){
    source.state=SelectionLayerState::VectorIndicated;
    //Validation and resource admission precede the owned archive copy.
    (void)freeze_cached_vector_tool_selection(cache,origin,source,stop);
    source.vector_cache=std::make_shared<const Layer>(cache);source.vector_cache_origin=origin;return source;
}
namespace vector_selection_detail {
struct BindingAccess {
    static const FrozenNativeSelection& reserve(const FrozenVectorToolSelection& packet,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
        if(&storage==&work)throw std::invalid_argument("Selection binding requires distinct storage/work ledgers");
        auto next_work=work,next_storage=storage;Budget budget{next_work,stop};budget.charge(packet.state_->work+1);
        charge(next_storage,packet.state_->storage,max_storage,"Selection binding aggregate storage exceeds512MiB");budget.charge();
        work=next_work;storage=next_storage;return packet.state_->native;
    }
};
const FrozenNativeSelection& reserve_vector_tool_selection(const FrozenVectorToolSelection& packet,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){return BindingAccess::reserve(packet,storage,work,stop);}
}
}
