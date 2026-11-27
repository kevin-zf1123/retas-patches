#include "retas/cut_folder_root.hpp"
#include <algorithm>
#include <limits>

namespace retas {
namespace {
using Path=std::filesystem::path;
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("CutFolder manager cancelled");}
struct Ledger {
    CelFolderBudget& budget;std::stop_token stop;
    void visit(std::uint64_t n){poll(stop);constexpr auto cap=128ull*1024*1024;if(budget.work>cap||n>cap-budget.work)throw std::length_error("CutFolder manager exceeds128Mi work");budget.work+=n;}
    void allocate(std::size_t n){poll(stop);constexpr auto cap=512ull*1024*1024;if(budget.allocation_bytes>cap||n>cap-budget.allocation_bytes)throw std::length_error("CutFolder manager exceeds512MiB memory");budget.allocation_bytes+=n;}
};
void path_bound(const Path& path){if(path.empty()||path.native().size()>4096||std::ranges::find(path.native(),Path::value_type{})!=path.native().end())throw std::invalid_argument("Invalid CutFolder manager path");}
std::string key(const Path& path,Ledger& ledger){
    path_bound(path);ledger.visit(path.native().size()*4+1);ledger.allocate(path.native().size()*sizeof(Path::value_type)*4+128);auto value=path.generic_string();while(value.size()>1&&value.back()=='/')value.pop_back();for(auto& c:value)if(c>='A'&&c<='Z')c=char(c+32);return value;
}
}
CutFolderRoot* CutFolderRootManager::acquire(const Path& metadata,const CutFolderRootLoadSettings& settings,std::stop_token stop){CelFolderBudget budget;return acquire(metadata,settings,budget,stop);}
CutFolderRoot* CutFolderRootManager::acquire(const Path& metadata,const CutFolderRootLoadSettings& settings,CelFolderBudget& budget,std::stop_token stop){
    Ledger ledger{budget,stop};poll(stop);path_bound(metadata);const auto absolute=std::filesystem::absolute(metadata);const auto requested=key(absolute,ledger);
    for(auto& entry:roots_){const auto cached=key(entry.root->archive_path(),ledger);ledger.visit(requested.size()+cached.size()+1);if(requested!=cached)continue;if(entry.references==std::uint32_t(std::numeric_limits<std::int32_t>::max()))throw std::length_error("CutFolder manager reference count exceeds int32");poll(stop);++entry.references;return entry.root.get();}
    if(roots_.size()==mn6_max_records)throw std::length_error("CutFolder manager exceeds100000 roots");ledger.allocate(sizeof(CutFolderRoot));if(roots_.size()==roots_.capacity()){const auto capacity=std::min<std::size_t>(mn6_max_records,std::max<std::size_t>(8,roots_.size()*2));ledger.allocate(capacity*sizeof(Entry));roots_.reserve(capacity);}
    auto root=std::make_unique<CutFolderRoot>(CutFolderRoot::load(absolute,settings,budget,stop));poll(stop);const auto out=root.get();roots_.push_back({std::move(root),1});return out;
}
bool CutFolderRootManager::release(CutFolderRoot*& root,std::stop_token stop){
    poll(stop);if(!root)return false;const auto found=std::ranges::find_if(roots_,[&](const auto& entry){return entry.root.get()==root;});if(found==roots_.end())return false;
    if(found->references==1){found->root->close(stop);roots_.erase(found);}else --found->references;root=nullptr;return true;
}
std::uint32_t CutFolderRootManager::references(const CutFolderRoot* root)const noexcept{for(const auto& entry:roots_)if(entry.root.get()==root)return entry.references;return 0;}
void CutFolderRootManager::close(std::stop_token stop){
    poll(stop);std::size_t done{};try{for(auto& entry:roots_){entry.root->close(stop);++done;}}catch(...){roots_.erase(roots_.begin(),roots_.begin()+std::ptrdiff_t(done));throw;}roots_.clear();
}
}
