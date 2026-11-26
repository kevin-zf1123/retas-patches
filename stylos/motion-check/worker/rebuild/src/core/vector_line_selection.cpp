#include "vector_native_mask_scope.hpp"
#include "vector_line_selection.hpp"
#include "vector_material_edit.hpp"
#include "retas/numeric.hpp"

namespace retas {
namespace {
using vector_probe_detail::Budget;
using vector_probe_detail::Scope;
using numeric_detail::binary64;
using vector_material_detail::charge;
constexpr std::uint64_t max_storage=512ull*1024*1024;
int raw(std::int64_t n){if(std::abs(n)>134217727)throw std::invalid_argument("Line selection exceeds bounded raw domain");return int(n);}
int floor_raw(double n){const auto q=std::floor(n);if(!std::isfinite(q)||q<-134217727.0||q>134217727.0)throw std::invalid_argument("Line selection mapped origin exceeds bounded raw domain");return raw(std::int64_t(q));}
std::uint64_t mask_bytes(const BinaryMask& mask){
    if(mask.width()<0||mask.height()<0||mask.width()>100000||mask.height()>100000||std::uint64_t(mask.width())*std::uint64_t(mask.height())>128ull*1024*1024)throw std::length_error("Line selection mask exceeds128Mi pixels");
    return sizeof(BinaryMask)+((std::uint64_t(mask.width())+31)/32)*std::uint64_t(mask.height())*4+((std::uint64_t(mask.width())+255)/256)*((std::uint64_t(mask.height())+255)/256);
}
void bounded(const Scope& scope){for(int n:{scope.bounds.left,scope.bounds.top,scope.bounds.right,scope.bounds.bottom})raw(n);}
}
struct FrozenVectorLineSelection::State {
    RegionSelection snapshot;std::shared_ptr<const BinaryMask> source_mask;
    double document_dpi{},mask_dpi{};Scope scope;
    std::uint64_t storage{},work{};
};
FrozenVectorLineSelection::FrozenVectorLineSelection(std::shared_ptr<const State> state):state_(std::move(state)){}
const RegionSelection& FrozenVectorLineSelection::source_snapshot()const noexcept{return state_->snapshot;}
bool FrozenVectorLineSelection::matches_source(const RegionSelection& source,double dpi)const noexcept{
    const auto& s=state_->snapshot;return dpi==state_->document_dpi&&source.state==s.state&&source.bounds==s.bounds&&source.origin==s.origin&&source.carrier_size==s.carrier_size&&source.carrier_dpi==s.carrier_dpi&&source.mask==state_->source_mask&&source.vector_cache==s.vector_cache&&source.vector_cache_origin==s.vector_cache_origin&&source.vector_flags==s.vector_flags;
}
bool FrozenVectorLineSelection::bypass()const noexcept{return !state_->scope.configured;}
double FrozenVectorLineSelection::selection_document_dpi()const noexcept{return state_->document_dpi;}
double FrozenVectorLineSelection::actual_mask_dpi()const noexcept{return state_->mask_dpi;}
FrozenVectorLineSelection prepare_vector_line_selection(const RegionSelection& input,double document_dpi,double mask_dpi,std::stop_token stop){
    using State=FrozenVectorLineSelection::State;
    std::uint64_t work{},storage{};Budget budget{work,stop};budget.charge();
    if(!std::isfinite(document_dpi)||document_dpi<=0||!std::isfinite(binary64(document_dpi*16)))throw std::invalid_argument("Line selection requires finite native document DPI");
    if(input.state!=SelectionLayerState::Cleared&&input.state!=SelectionLayerState::FullPlane&&input.state!=SelectionLayerState::Rectangle&&input.state!=SelectionLayerState::Mask&&input.state!=SelectionLayerState::VectorIndicated)throw std::invalid_argument("Unknown Line selection state");
    charge(storage,sizeof(State)+1024,max_storage,"Line selection retained storage exceeds512MiB");budget.charge(sizeof(State)+1024);
    auto state=std::make_shared<State>();state->snapshot=input;state->source_mask=input.mask;state->document_dpi=document_dpi;state->mask_dpi=mask_dpi;
    if(input.mask){const auto bytes=2*mask_bytes(*input.mask);charge(storage,bytes,max_storage,"Line selection retained masks exceed512MiB");budget.charge(bytes);state->snapshot.mask=std::make_shared<const BinaryMask>(*input.mask);budget.charge();}
    const auto& source=state->snapshot;
    if(source.state==SelectionLayerState::Rectangle){const auto& r=source.bounds;if(r.width<0||r.height<0)throw std::invalid_argument("Line selection rectangle has negative extent");state->scope=Scope::rectangle({raw(std::int64_t(r.x)*16),raw(std::int64_t(r.y)*16),raw((std::int64_t(r.x)+r.width)*16),raw((std::int64_t(r.y)+r.height)*16)});}
    else if(source.state==SelectionLayerState::Mask||source.state==SelectionLayerState::VectorIndicated){
        if(!source.mask||!std::isfinite(mask_dpi)||mask_dpi<=0)throw std::invalid_argument("Line raster selection requires actual backing and DPI");
        if(source.carrier_size!=std::array<int,2>{}&&source.carrier_size!=std::array<int,2>{source.mask->width(),source.mask->height()})throw std::invalid_argument("Line selection carrier metadata differs from actual mask");
        //5C76E0 stores quotient*16 before computing the reciprocal.5C6A60
        // floors each extent edge before adding an integer mapped origin.
        const auto forward=binary64(binary64(document_dpi/mask_dpi)*16);
        state->scope=vector_menu_scope_detail::mask(*source.mask,{0,0},source.bounds,budget,forward,mask_dpi);bounded(state->scope);
    }
    budget.charge();state->storage=storage;state->work=work;vector_material_detail::poll(stop);return FrozenVectorLineSelection(std::move(state));
}
namespace vector_line_selection_detail {
Scope borrow_mask_scope(const RegionSelection& source,double document_dpi,
    std::array<int,2> target,Budget budget){
    budget.charge();
    if(!source.mask||!std::isfinite(document_dpi)||document_dpi<=0)
        throw std::invalid_argument("Line raster selection requires actual backing and document DPI");
    if(source.carrier_size!=std::array<int,2>{}&&source.carrier_size!=std::array<int,2>{source.mask->width(),source.mask->height()})
        throw std::invalid_argument("Line selection carrier metadata differs from actual mask");
    (void)mask_bytes(*source.mask);
    const auto forward=binary64(binary64(document_dpi/selection_carrier_dpi(source,document_dpi))*16);
    std::array<int,2> offset;
    for(unsigned i=0;i<2;++i)offset[i]=floor_raw(binary64(double(std::int64_t(source.origin[i])-target[i])*forward));
    auto scope=vector_menu_scope_detail::mask(*source.mask,offset,source.bounds,budget,forward,selection_carrier_dpi(source,document_dpi));bounded(scope);return scope;
}
struct CarrierAccess {
    static Scope bind(const FrozenVectorLineSelection& packet,std::array<int,2> target,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){
        if(&storage==&work)throw std::invalid_argument("Line selection requires distinct storage/work ledgers");
        auto next_work=work,next_storage=storage;Budget budget{next_work,stop};budget.charge(packet.state_->work+64);charge(next_storage,packet.state_->storage,max_storage,"Line selection aggregate storage exceeds512MiB");
        auto scope=packet.state_->scope;
        if(scope.configured){std::array<int,2> offset;for(unsigned i=0;i<2;++i){const auto delta=std::int64_t(packet.state_->snapshot.origin[i])-target[i];offset[i]=scope.mask?floor_raw(binary64(double(delta)*scope.forward)):raw(delta*16);}
            scope.offset=offset;scope.bounds={raw(std::int64_t(scope.bounds.left)+offset[0]),raw(std::int64_t(scope.bounds.top)+offset[1]),raw(std::int64_t(scope.bounds.right)+offset[0]),raw(std::int64_t(scope.bounds.bottom)+offset[1])};}
        budget.charge();storage=next_storage;work=next_work;return scope;
    }
};
Scope reserve_line_selection_scope(const FrozenVectorLineSelection& packet,std::array<int,2> target,std::uint64_t& storage,std::uint64_t& work,std::stop_token stop){return CarrierAccess::bind(packet,target,storage,work,stop);}
}
}
