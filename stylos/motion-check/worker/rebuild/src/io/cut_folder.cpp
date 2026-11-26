#include "retas/cut_folder.hpp"
#include "cut_folder_internal.hpp"
#include "catalog_compare.hpp"
#include "native_preference_encoding.hpp"
#include "../core/batch_text.hpp"
#include <algorithm>
#include <fstream>
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
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("MNG catalog operation cancelled");}
struct Ledger {
    CelFolderBudget& budget;std::stop_token stop;
    void visit(std::uint64_t n){poll(stop);constexpr auto cap=128ull*1024*1024;if(budget.work>cap||n>cap-budget.work)throw std::length_error("MNG catalog exceeds128Mi work");budget.work+=n;}
    void allocate(std::size_t n){constexpr auto cap=512ull*1024*1024;if(budget.allocation_bytes>cap||n>cap-budget.allocation_bytes)throw std::length_error("MNG catalog exceeds512MiB memory");budget.allocation_bytes+=n;}
};
std::string lower(std::string value){for(auto& c:value)if(c>='A'&&c<='Z')c=char(c+32);return value;}
bool mng_extension(std::string_view name){const auto dot=name.rfind('.');return dot!=std::string_view::npos&&lower(std::string(name.substr(dot+1)))=="mng";}
void path_bound(const Path& path){if(path.empty()||path.native().size()>4096||std::ranges::find(path.native(),Path::value_type{})!=path.native().end())throw std::invalid_argument("Invalid MNG catalog path");}
void component(std::string_view value,NativePreferenceEncoding encoding={}){
    const auto text=batch_text_detail::decode(value,encoding);
    if(value.empty()||value.size()>255||std::any_of(text.begin(),text.end(),[](unsigned char c){return c<32||c==127||c=='/'||c=='\\'||c==':';}))throw std::invalid_argument("MNG catalog requires safe basenames of at most255 encoded bytes");
}
void validate(const MngCatalogOptions& options){native_preference_detail::validate_encoding(options.text_encoding);if(options.image_extensions.size()>cel_folder_max_records)throw std::length_error("Too many MNG image extensions");for(const auto& ext:options.image_extensions){component(ext);if(ext.find('.')!=std::string::npos)throw std::invalid_argument("MNG image extension requires no dot");}}
bool hidden(const Path& path){
#ifdef _WIN32
    const auto flags=GetFileAttributesW(path.c_str());if(flags==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Cannot query MNG catalog attributes");return bool(flags&FILE_ATTRIBUTE_HIDDEN);
#else
#ifdef __APPLE__
    struct stat state{};if(::lstat(path.c_str(),&state)!=0)throw std::runtime_error("Cannot query MNG catalog flags");if(state.st_flags&UF_HIDDEN)return true;
#endif
    return path.filename().string().starts_with('.');
#endif
}
bool directory(const std::filesystem::directory_entry& entry){const auto s=entry.symlink_status();return !std::filesystem::is_symlink(s)&&std::filesystem::is_directory(s)&&!hidden(entry.path());}
int rank(std::string_view name){if(name=="Layout"||name=="-LO")return -4;if(name=="BG"||name=="-BG")return -3;if(name=="Key Animation"||name=="-Key")return -2;if(name=="Inbetween")return -1;return 0;}
std::uint16_t flags(std::string_view name,bool nested){return std::uint16_t((nested?0x2018:0x2008)|((name=="Layout"||name=="-LO")?1:(name=="BG"||name=="-BG")?2:0));}
Bytes read_mng(const Path& path,Ledger& ledger){
    if(!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))throw std::runtime_error("MNG metadata requires a regular file");
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Cannot open MNG management file");const auto size=file.tellg();
    if(size<0||size>std::streamoff(management_max_bytes))throw std::length_error("MNG file exceeds64MiB");
    // Twelve first-match lookups; account their worst-case byte traversal before
    // touching file bytes. This is a conservative modern cumulative limit.
    ledger.visit(std::uint64_t(size)*12);ledger.allocate(std::size_t(size)*3);Bytes bytes(std::size_t(size),0);file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))throw std::runtime_error("Short MNG management file read");return bytes;
}
bool numeric(std::string_view value){while(!value.empty()&&(value.front()==' '||(value.front()>='\t'&&value.front()<='\r')))value.remove_prefix(1);if(!value.empty()&&(value.front()=='+'||value.front()=='-'))value.remove_prefix(1);return !value.empty()&&value.front()>='0'&&value.front()<='9';}
MngCutFolder open_mng(const Path& path,const MngCatalogOptions& options,Ledger& ledger){
    const auto encoding=options.text_encoding;const auto filename=[&](const Path& p){return batch_text_detail::native_path(p.filename(),encoding);};
    poll(ledger.stop);path_bound(path);component(filename(path),encoding);if(!mng_extension(filename(path)))throw std::invalid_argument("MNG CutFolder requires .mng extension");
    if(std::filesystem::is_symlink(std::filesystem::symlink_status(path)))throw std::invalid_argument("MNG management symlink is unsupported");
    MngCutFolder result;result.management_path=std::filesystem::absolute(path);const auto name=filename(path);result.archive_path=result.management_path.parent_path()/batch_text_detail::host_path(name.substr(0,name.rfind('.'))+".mn6",encoding);
    result.info=decode_mng_cut_folder_info(read_mng(path,ledger),ledger.stop);const auto root=result.management_path.parent_path();std::size_t visited{};std::set<std::u16string> admitted;
    const auto append=[&](const Path& candidate,bool nested){
        poll(ledger.stop);if(result.folders.size()==cel_folder_max_records)throw std::length_error("MNG catalog exceeds100000 folders");
        const auto name=filename(candidate);component(name,encoding);const auto identity=cut_folder_detail::comparison_key(batch_text_detail::native_path(candidate.lexically_relative(root),encoding),encoding);if(admitted.contains(identity))return;
        ledger.allocate(sizeof(MngCelFolder)+candidate.native().size()*sizeof(Path::value_type)*2+identity.size()+sizeof(std::string)*3);
        try{auto owner=CelFolder::open(candidate,options.image_extensions,ledger.budget,ledger.stop,encoding);result.folders.push_back({candidate,flags(name,nested),std::move(owner)});admitted.insert(identity);}
        catch(const std::runtime_error& error){poll(ledger.stop);ledger.allocate(sizeof(MngFolderFailure)+candidate.native().size()*sizeof(Path::value_type)+std::char_traits<char>::length(error.what()));result.rejected_folders.push_back({candidate,error.what()});}
    };
    for(const auto& outer:std::filesystem::directory_iterator(root)){
        ledger.visit(1);if(visited++==cel_folder_max_records)throw std::length_error("MNG catalog exceeds100000 directory entries");if(!directory(outer))continue;const auto name=filename(outer.path());component(name,encoding);if(name=="-Key")continue;
        bool inner_found{};for(const auto& inner:std::filesystem::directory_iterator(outer.path())){
            ledger.visit(1);if(visited++==cel_folder_max_records)throw std::length_error("MNG catalog exceeds100000 directory entries");if(!directory(inner))continue;component(filename(inner.path()),encoding);inner_found=true;append(inner.path(),true);
        }
        if(!inner_found)append(outer.path(),false);
    }
    ledger.allocate(result.folders.size()*sizeof(MngCelFolder));std::stable_sort(result.folders.begin(),result.folders.end(),[&](const MngCelFolder& a,const MngCelFolder& b){
        const auto left=filename(a.path),right=filename(b.path);ledger.visit(left.size()+right.size()+1);const auto ar=rank(left),br=rank(right);if(ar!=br)return ar<br;
        if(options.compare_paths){const auto compared=options.compare_paths(a.path,b.path);poll(ledger.stop);if(compared)return *compared<0;}
        return cut_folder_detail::comparison_key(left,encoding)<cut_folder_detail::comparison_key(right,encoding);
    });return result;
}
}
MngCatalogOptions cut_folder_detail::snapshot(const MngCatalogOptions& options,CelFolderBudget& budget){
    validate(options);std::size_t size=sizeof(MngCatalogOptions)+options.image_extensions.size()*sizeof(std::string);for(const auto& ext:options.image_extensions)size+=ext.size();
    Ledger ledger{budget,{}};ledger.allocate(size);auto frozen=options;
#ifdef _WIN32
    if(!frozen.compare_paths)frozen.compare_paths=shell_compare;
#endif
    return frozen;
}
MngCutFolderInfo decode_mng_cut_folder_info(std::span<const std::uint8_t> bytes,std::stop_token stop){
    MngCutFolderInfo result;const auto integer=[&](std::uint32_t tag,std::int32_t& target){const auto text=management_string(bytes,tag,stop);if(!text||!numeric(*text))throw FormatError(0,"MNG CutFolder requires numeric tags1..4");management_integer(bytes,tag,target,stop);};
    integer(3,result.dpi);integer(1,result.width);integer(2,result.height);
    for(auto [tag,target]:{std::pair{5u,&result.field_0c},std::pair{6u,&result.field_18},std::pair{8u,&result.field_24},std::pair{7u,&result.field_30}}){const auto text=management_string(bytes,tag,stop);if(!text)throw FormatError(0,"MNG CutFolder requires text tags5..8");*target=*text;}integer(4,result.rate);return result;
}
void MngCutFolder::close(std::stop_token stop){for(auto& folder:folders){poll(stop);folder.owner.close(stop);}}
MngCutFolder open_mng_cut_folder(const Path& path,const MngCatalogOptions& options,std::stop_token stop){CelFolderBudget budget;return open_mng_cut_folder(path,options,budget,stop);}
MngCutFolder open_mng_cut_folder(const Path& path,const MngCatalogOptions& options,CelFolderBudget& budget,std::stop_token stop){poll(stop);const auto frozen=cut_folder_detail::snapshot(options,budget);Ledger ledger{budget,stop};return open_mng(path,frozen,ledger);}
MngOutputCatalog scan_mng_output_catalog(const Path& path,const MngCatalogOptions& options,std::stop_token stop){CelFolderBudget budget;return scan_mng_output_catalog(path,options,budget,stop);}
MngOutputCatalog scan_mng_output_catalog(const Path& path,const MngCatalogOptions& options,CelFolderBudget& budget,std::stop_token stop){
    poll(stop);path_bound(path);const auto frozen=cut_folder_detail::snapshot(options,budget);MngOutputCatalog result;result.scan_path=path;Ledger ledger{budget,stop};if(!std::filesystem::is_directory(path))return result;std::size_t visited{};
    for(const auto& entry:std::filesystem::directory_iterator(path)){
        ledger.visit(1);if(visited++==cel_folder_max_records)throw std::length_error("MNG output scan exceeds100000 directory entries");const auto state=entry.symlink_status();
        if(std::filesystem::is_symlink(state)||!std::filesystem::is_regular_file(state)||hidden(entry.path()))continue;
        const auto name=batch_text_detail::native_path(entry.path().filename(),frozen.text_encoding);component(name,frozen.text_encoding);if(!mng_extension(name))continue;
        auto root=open_mng(entry.path(),frozen,ledger);result.management_found=true;
        if(root.folders.size()>cel_folder_max_records-result.folders.size())throw std::length_error("MNG output catalog exceeds100000 entries");
        for(const auto& child:root.folders){ledger.allocate(sizeof(Path)+child.path.native().size()*sizeof(Path::value_type));result.folders.push_back(child.path);}
        if(root.rejected_folders.size()>cel_folder_max_records-result.rejected_folders.size())throw std::length_error("MNG output catalog exceeds100000 rejections");
        for(auto& failure:root.rejected_folders)result.rejected_folders.push_back(std::move(failure));root.close(stop);
    }return result;
}
}
