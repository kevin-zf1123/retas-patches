#include "retas/cut_folder_root.hpp"
#include "cut_folder_internal.hpp"
#include "catalog_compare.hpp"
#include "../core/batch_text.hpp"
#include <algorithm>
#include <set>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <sys/stat.h>
#endif

namespace retas {
namespace {
using Path=std::filesystem::path;
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("CutFolder children cancelled");}
struct Ledger {
    CelFolderBudget& budget;std::stop_token stop;
    void visit(std::uint64_t n){poll(stop);constexpr auto cap=128ull*1024*1024;if(budget.work>cap||n>cap-budget.work)throw std::length_error("CutFolder children exceed128Mi work");budget.work+=n;}
    void allocate(std::size_t n){poll(stop);constexpr auto cap=512ull*1024*1024;if(budget.allocation_bytes>cap||n>cap-budget.allocation_bytes)throw std::length_error("CutFolder children exceed512MiB memory");budget.allocation_bytes+=n;}
};
struct Active {
    bool& flag;
    explicit Active(bool& value):flag(value){if(flag)throw std::logic_error("CutFolder child operation is active");flag=true;}
    ~Active(){flag=false;}
};
bool selected(std::uint16_t kind,std::uint32_t mask,std::size_t group){
    if(kind==2)return group==2;if(kind!=1)return false;constexpr std::uint16_t bits[]{1,4,8,2};const auto low=std::uint16_t(mask);return !low||bool(low&bits[group]);
}
void path_bound(const Path& path){if(path.empty()||path.native().size()>4096||std::ranges::find(path.native(),Path::value_type{})!=path.native().end())throw std::invalid_argument("Invalid CutFolder child path");}
void component(std::string_view raw,NativePreferenceEncoding encoding){
    const auto name=batch_text_detail::decode(raw,encoding);
    if(raw.empty()||raw.size()>255||name=="."||name==".."||std::ranges::any_of(name,[](unsigned char c){return c<32||c==127||c=='/'||c=='\\'||c==':';}))
        throw std::invalid_argument("Invalid CutFolder child basename");
}
bool hidden(const Path& path){
#ifdef _WIN32
    const auto flags=GetFileAttributesW(path.c_str());if(flags==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Cannot query CutFolder child attributes");return bool(flags&FILE_ATTRIBUTE_HIDDEN);
#else
#ifdef __APPLE__
    struct stat state{};if(::lstat(path.c_str(),&state))throw std::runtime_error("Cannot query CutFolder child flags");if(state.st_flags&UF_HIDDEN)return true;
#endif
    return path.filename().string().starts_with('.');
#endif
}
bool visible_directory(const std::filesystem::directory_entry& item){return std::filesystem::is_directory(item.symlink_status())&&!hidden(item.path());}
int rank(std::string_view name){if(name=="Layout"||name=="-LO")return -4;if(name=="BG"||name=="-BG")return -3;if(name=="Key Animation"||name=="-Key")return -2;if(name=="Inbetween")return -1;return 0;}
std::uint16_t flags(std::string_view name,std::uint16_t base){return std::uint16_t(base|((name=="Layout"||name=="-LO")?1:(name=="BG"||name=="-BG")?2:0));}
unsigned comparisons(std::size_t count){unsigned value=2;while(count){count/=2;value+=2;}return value;}
}

std::size_t CutFolderRoot::child_count(std::uint32_t mask)const{
    if(children_active_)throw std::logic_error("CutFolder child operation is active");std::size_t size{};for(std::size_t group=0;group<4;++group)if(selected(archive_.info.kind,mask,group))size+=children_[group].size();return size;
}
std::vector<CutFolderChildView> CutFolderRoot::child_folders(std::uint32_t mask)const{
    std::vector<CutFolderChildView> out;out.reserve(child_count(mask));
    for(std::size_t group=0;group<4;++group)if(selected(archive_.info.kind,mask,group))for(const auto& child:children_[group])out.push_back({child->path,child->flags,child->owner});return out;
}
void CutFolderRoot::clear_children_selected(std::uint32_t mask,std::stop_token stop){
    for(std::size_t group=0;group<4;++group)if(selected(archive_.info.kind,mask,group)){
        auto& values=children_[group];std::size_t done{};try{for(const auto& child:values){poll(stop);child->owner.close(stop);++done;++children_revision_;}}catch(...){values.erase(values.begin(),values.begin()+std::ptrdiff_t(done));throw;}values.clear();
    }
}
void CutFolderRoot::clear_children(std::uint32_t mask,std::stop_token stop){
    if(closed_)throw std::logic_error("CutFolder root is closed");Active active(children_active_);poll(stop);clear_children_selected(mask,stop);
}
void CutFolderRoot::expand_children(std::uint32_t mask,const MngCatalogOptions& options,std::stop_token stop){CelFolderBudget budget;expand_children(mask,options,budget,stop);}
void CutFolderRoot::expand_children(std::uint32_t mask,const MngCatalogOptions& supplied,CelFolderBudget& budget,std::stop_token stop){
    if(closed_)throw std::logic_error("CutFolder root is closed");Active active(children_active_);poll(stop);path_bound(root_);if(!std::filesystem::exists(root_))return;
    Ledger ledger{budget,stop};const auto initial_revision=children_revision_;if(supplied.image_extensions.size()>cel_folder_max_records)throw std::length_error("Too many CutFolder child extensions");for(const auto& ext:supplied.image_extensions)ledger.visit(ext.size()*8+1);const auto options=cut_folder_detail::snapshot(supplied,budget);if(children_revision_!=initial_revision)throw std::logic_error("CutFolder changed while copying child options");
    const auto encoding=options.text_encoding;
    const auto filename=[&](const Path& p){return batch_text_detail::native_path(p.filename(),encoding);};
    const auto identity=[&](const Path& path,Ledger& bounds){
        bounds.visit(path.native().size()*4+1);bounds.allocate(path.native().size()*sizeof(Path::value_type)*4+128);
        return cut_folder_detail::comparison_key(batch_text_detail::native_path(path.lexically_relative(root_),encoding),encoding);
    };
    const auto kind=archive_.info.kind;if(kind!=1&&kind!=2)return;child_failures_.clear();
    // Pruning occurs before the native enumeration catch. A failed Close keeps
    // that owner available rather than destroying a pending write.
    for(std::size_t group=0;group<4;++group)if(selected(kind,mask,group)){
        auto& values=children_[group];std::size_t read{},write{};try{for(;read<values.size();++read){ledger.visit(values[read]->path.native().size()+1);if(std::filesystem::exists(values[read]->path)){if(write!=read)values[write]=std::move(values[read]);++write;continue;}values[read]->owner.close(stop);values[read].reset();++children_revision_;}}catch(...){for(;read<values.size();++read){if(write!=read)values[write]=std::move(values[read]);++write;}values.resize(write);throw;}values.resize(write);
    }
    std::array<std::set<std::u16string>,4> admitted;
    for(std::size_t group=0;group<4;++group)if(selected(kind,mask,group))for(const auto& child:children_[group]){auto key=identity(child->path,ledger);ledger.visit((key.size()+1)*comparisons(admitted[group].size()));admitted[group].insert(std::move(key));}
    std::size_t visited{},total{};for(const auto& group:children_)total+=group.size();
    const auto visit=[&](const std::filesystem::directory_entry& item){ledger.visit(item.path().native().size()+1);if(++visited>cel_folder_max_records)throw std::length_error("CutFolder child scan exceeds100000 entries");path_bound(item.path());};
    const auto append=[&](const Path& path,std::size_t group,std::uint16_t base){
        const auto name=filename(path);component(name,encoding);const auto key=identity(path,ledger);ledger.visit((key.size()+1)*comparisons(admitted[group].size())*2);if(admitted[group].contains(key))return;
        if(total==cel_folder_max_records)throw std::length_error("CutFolder cache exceeds100000 children");ledger.allocate(sizeof(Child)+sizeof(std::unique_ptr<Child>)*3+path.native().size()*sizeof(Path::value_type)*2);
        try{
            auto owner=CelFolder::open(path,options.image_extensions,budget,stop,encoding,read_only_);poll(stop);auto child=std::make_unique<Child>(Child{path,flags(name,base),std::move(owner)});admitted[group].insert(key);children_[group].push_back(std::move(child));++total;++children_revision_;
        }catch(const std::length_error&){throw;}catch(const std::bad_alloc&){throw;}catch(const std::exception& error){poll(stop);const auto n=std::char_traits<char>::length(error.what());ledger.visit(n);ledger.allocate(sizeof(MngFolderFailure)+path.native().size()*sizeof(Path::value_type)+n+1);child_failures_.push_back({path,error.what()});}
    };
    try{
        for(const auto& outer:std::filesystem::directory_iterator(root_)){
            visit(outer);if(!visible_directory(outer))continue;const auto name=filename(outer.path());
            if(kind==2){
                component(name,encoding);if(name=="-Key")continue;bool nested{};for(const auto& inner:std::filesystem::directory_iterator(outer.path())){visit(inner);if(!visible_directory(inner))continue;nested=true;append(inner.path(),2,0x2018);}if(!nested)append(outer.path(),2,0x2008);
            }else if(name=="Layout"||name=="BG"){
                const std::size_t group=name=="Layout"?0:3;if(selected(kind,mask,group))append(outer.path(),group,name=="Layout"?0x1001:0x1002);
            }else if(name=="Key Animation"||name=="Inbetween"){
                const std::size_t group=name=="Key Animation"?1:2;if(!selected(kind,mask,group))continue;for(const auto& inner:std::filesystem::directory_iterator(outer.path())){visit(inner);if(visible_directory(inner))append(inner.path(),group,group==1?0x1004:0x1008);}
            }
        }
    }catch(const std::length_error&){throw;}catch(const std::bad_alloc&){throw;}catch(const std::exception&){poll(stop);clear_children_selected(mask,stop);throw;}
    // Sort owned key copies. A provider cannot invalidate live vector storage
    // while std::stable_sort is unwinding. The revision also detects moves.
    struct Key {Path path;std::u16string folded;std::size_t index;int rank;};
    const auto sort=[&](std::size_t group){
        auto& values=children_[group];ledger.allocate(values.size()*(sizeof(Key)*2+sizeof(std::unique_ptr<Child>)));std::vector<Key> keys;keys.reserve(values.size());
        for(std::size_t i=0;i<values.size();++i){const auto& path=values[i]->path;ledger.visit(path.native().size()+1);ledger.allocate(path.native().size()*sizeof(Path::value_type)*2+512);const auto name=filename(path);keys.push_back({path,cut_folder_detail::comparison_key(name,encoding),i,rank(name)});}
        const auto revision=children_revision_;std::stable_sort(keys.begin(),keys.end(),[&](const Key& a,const Key& b){ledger.visit(a.folded.size()+b.folded.size()+1);if(a.rank!=b.rank)return a.rank<b.rank;
            if(options.compare_paths){const auto compared=options.compare_paths(a.path,b.path);if(children_revision_!=revision)throw std::logic_error("CutFolder changed during child comparison");poll(stop);if(compared)return *compared<0;}return a.folded<b.folded;
        });poll(stop);if(children_revision_!=revision)throw std::logic_error("CutFolder changed during child comparison");std::vector<std::unique_ptr<Child>> ordered;ordered.reserve(values.size());for(const auto& key:keys)ordered.push_back(std::move(values[key.index]));values.swap(ordered);++children_revision_;
    };
    if(kind==2)sort(2);else for(const auto group:{0u,3u,1u,2u})sort(group);poll(stop);
}
}
