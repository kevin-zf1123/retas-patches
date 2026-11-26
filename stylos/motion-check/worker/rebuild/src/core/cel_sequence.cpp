#include "retas/cel_sequence.hpp"
#include <stdexcept>
#include <algorithm>
#include <utility>
#include "modal_document_guard.hpp"

namespace retas {
namespace {
struct Entry {bool& entered;explicit Entry(bool& value):entered(value){if(value)throw std::logic_error("Reentrant CEL sequence operation");value=true;}~Entry(){entered=false;}};
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("CEL sequence cancelled");}
template<class F>auto live_callback(Document& document,std::stop_token stop,F callback){
    const auto generation=document.state_generation();const auto path=document.path;const auto sessions=modal_detail::sessions(document);
    std::uint64_t storage{};modal_detail::data_charge(document.data,storage);const auto frozen=modal_detail::frozen(document.data,stop);
    const auto check=[&]{if(document.state_generation()!=generation||document.path!=path||modal_detail::sessions(document)!=sessions||!modal_detail::data_same(document.data,frozen))throw CelSequenceSourceChanged("CEL document changed during provider");};
    try{auto result=callback();check();return result;}catch(...){check();throw;}
}
struct PlaneState {bool line{};int gray_selector{};bool gray_visible{};};
PlaneState plane_state(const DocumentData& data){
    PlaneState state;for(const auto& l:data.layers)if(l.family&0x01000000u){bool draw{},line{};for(const auto& p:l.planes){if((p.family&0x02020000u)&&p.common.flags[1])draw=true;if((p.family&0x00010000u)&&p.common.flags[1])line=true;}if(draw)break;if(line){state.line=true;break;}}
    for(const auto& l:data.layers)if(l.family==0x11001000u){int line=-1,color=-1;for(std::size_t i=0;i<l.planes.size();++i){if(line<0&&l.planes[i].family==0x11110001u)line=int(i);if(color<0&&l.planes[i].family==0x12100001u)color=int(i);}const auto selected=l.current_plane==line?color:l.current_plane==color?line:line>=0?line:color;if(selected>=0){state.gray_selector=selected==line?1:2;state.gray_visible=l.planes[std::size_t(selected)].common.flags[1]!=0;}break;}
    return state;
}
void inherit_state(DocumentData& data,const PlaneState& state){
    const auto choose=[](Layer& l,std::size_t i){for(auto& p:l.planes)p.common.flags[2]=0;l.planes[i].common.flags[2]=1;l.current_plane=int(i);};
    for(auto& l:data.layers)if(l.family&0x01000000u){
        for(std::size_t i=0;i<l.planes.size();++i){auto& p=l.planes[i];if(state.line){p.common.flags[1]=(p.family&0x10000u)!=0;if(p.common.flags[1])choose(l,i);}else if(p.family&0x10000u){if(l.family==0x11001000u)p.common.flags[1]=0;}else if(p.family&0x100000u){p.common.flags[1]=1;choose(l,i);}}
        if(!state.line)for(std::size_t i=0;i<l.planes.size();++i)if(l.planes[i].family==0x22100001u)choose(l,i);
    }
    if(state.gray_selector)for(auto& l:data.layers)if(l.family==0x11001000u){const auto family=state.gray_selector==1?0x11110001u:0x12100001u;for(auto& p:l.planes)if(p.family==family){p.common.flags[1]=state.gray_visible?1:0;break;}break;}
}

}
CelSequence::CurrentLease::CurrentLease(CelSequence& sequence):sequence_(&sequence){if(sequence.entered_)throw std::logic_error("Reentrant CEL sequence operation");sequence.entered_=true;}
CelSequence::CurrentLease::CurrentLease(CurrentLease&& other)noexcept:sequence_(std::exchange(other.sequence_,nullptr)){}
CelSequence::CurrentLease::~CurrentLease(){if(sequence_)sequence_->entered_=false;}
Document& CelSequence::CurrentLease::document()const noexcept{return *sequence_->current_;}
CelSequence::CurrentLease CelSequence::borrow_current(){return CurrentLease(*this);}
CelSequence::CelSequence(std::vector<std::filesystem::path> paths,std::size_t current,
    BatchDecodedImage image,CelSequenceProviders providers):paths_(std::move(paths)),index_(current),
    current_(std::make_unique<Document>(std::move(image.data))),context_(std::move(image.context)),providers_(std::move(providers)){
    if(paths_.empty()||paths_.size()>10000||index_>=paths_.size())throw std::invalid_argument("Invalid CEL sequence index/count");
    std::size_t bytes{};for(const auto& p:paths_){const auto text=p.generic_string();if(text.empty()||text.size()>4096||text.find('\0')!=text.npos||bytes>16*1024*1024-text.size())throw std::invalid_argument("Invalid CEL sequence path");bytes+=text.size();}
    current_->path=paths_[index_];
}
std::optional<BatchDecodedImage> CelSequence::load_frame(std::size_t index,std::stop_token stop)const{
    poll(stop);if(index>=paths_.size())throw std::out_of_range("CEL sequence frame");if(!providers_.load)throw std::invalid_argument("CEL sequence has no loader");
    auto image=providers_.load(paths_[index],stop);poll(stop);return image;
}
bool CelSequence::save_impl(std::stop_token stop){
    poll(stop);if(current_->has_text_preview()||current_->has_floating_transform()||current_->has_frame_move()||current_->has_vanishing_point_move())throw std::logic_error("Settle transient CEL editing before Save");if(!providers_.save)throw std::invalid_argument("CEL sequence has no save provider");
    const auto generation=current_->state_generation();std::uint64_t storage{};modal_detail::data_charge(current_->data,storage);auto baseline=std::make_shared<DocumentData>(modal_detail::frozen(current_->data,stop));
    auto saved_path=current_->path;auto saved_bytes=std::make_shared<Bytes>();auto bytes=live_callback(*current_,stop,[&]{return providers_.save(saved_path,*baseline,context_,stop);});
    if(current_->state_generation()!=generation||!modal_detail::data_same(current_->data,*baseline))throw std::logic_error("CEL sequence document changed during save provider");
    if(!bytes)return false;*saved_bytes=std::move(*bytes);current_->mark_saved(std::move(saved_path),std::move(saved_bytes),std::move(baseline));return true;
}
bool CelSequence::save_current(std::stop_token stop){Entry guard(entered_);return save_impl(stop);}
bool CelSequence::reload_current(std::stop_token stop){
    Entry guard(entered_);const auto generation=current_->state_generation();if(!providers_.load)throw std::invalid_argument("CEL sequence has no loader");auto image=live_callback(*current_,stop,[&]{return providers_.load(current_->path,stop);});poll(stop);
    if(current_->state_generation()!=generation)throw std::logic_error("CEL sequence document changed during reload provider");
    if(!image)return false;auto next=std::make_unique<Document>(std::move(image->data));next->path=current_->path;current_=std::move(next);context_=std::move(image->context);return true;
}
CelBatchPreparation CelSequence::prepare_batch(std::stop_token stop){
    if(std::ranges::any_of(modal_detail::sessions(*current_),[](bool active){return active;}))throw std::logic_error("Settle transient CEL editing before Batch");
    if(!current_->modified())return CelBatchPreparation::Ready;
    if(current_->modified()){
        if(!providers_.settle)throw std::invalid_argument("Source2 requires an explicit dirty settlement decision");
        Entry guard(entered_);const auto generation=current_->state_generation();const auto decision=live_callback(*current_,stop,[&]{return providers_.settle(*current_);});
        if(current_->state_generation()!=generation)throw std::logic_error("CEL sequence document changed during Batch settlement");
        if(decision==CloseSaveResponse::Cancel)return CelBatchPreparation::Cancelled;if(decision==CloseSaveResponse::Save)return save_impl(stop)?CelBatchPreparation::Ready:CelBatchPreparation::Failed;
    }
    return reload_current(stop)?CelBatchPreparation::Ready:CelBatchPreparation::Failed;
}
CelNavigation CelSequence::navigate(std::size_t target,std::stop_token stop,bool inherit_plane_state){
    Entry guard(entered_);poll(stop);if(target>=paths_.size())throw std::out_of_range("CEL sequence navigation");if(target==index_)return CelNavigation::Unchanged;
    const auto state=plane_state(current_->data);
    if(current_->modified()){
        if(!providers_.settle)throw std::invalid_argument("Modified CEL navigation requires an explicit Save/Discard/Cancel decision");
        const auto generation=current_->state_generation();const auto decision=live_callback(*current_,stop,[&]{return providers_.settle(*current_);});
        if(current_->state_generation()!=generation)throw std::logic_error("CEL sequence document changed during settlement");
        if(decision==CloseSaveResponse::Cancel)return CelNavigation::Cancelled;
        if(decision==CloseSaveResponse::Save&&!save_impl(stop))return CelNavigation::Cancelled;
    }
    const auto generation=current_->state_generation();auto image=live_callback(*current_,stop,[&]{return load_frame(target,stop);});if(current_->state_generation()!=generation)throw std::logic_error("CEL sequence document changed during loader");
    if(!image)return CelNavigation::Unavailable;if(inherit_plane_state)inherit_state(image->data,state);auto next=std::make_unique<Document>(std::move(image->data));next->path=paths_[target];
    current_=std::move(next);context_=std::move(image->context);index_=target;return CelNavigation::Moved;
}
CelPreviewResult run_cel_preview(CelSequence& sequence,std::size_t maximum_steps,const std::function<bool(const CelSequence&)>& pump,std::stop_token stop){
    if(maximum_steps>1000000)throw std::length_error("CEL preview step budget exceeded");CelPreviewResult result;
    for(std::size_t n=0;n<maximum_steps;++n){if(stop.stop_requested()){result.cancelled=true;break;}
        const auto moved=sequence.navigate((sequence.index()+1)%sequence.paths().size(),stop,true);
        if(moved!=CelNavigation::Moved){result.cancelled=moved==CelNavigation::Cancelled;result.unavailable=moved==CelNavigation::Unavailable;break;}
        ++result.navigated;if(pump&&!pump(sequence))break;
    }return result;
}
}
