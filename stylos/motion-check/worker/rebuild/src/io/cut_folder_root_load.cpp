#include "retas/cut_folder_root.hpp"
#include "retas/cut_folder.hpp"
#include "retas/cut_folder_model_path.hpp"
#include "retas/document.hpp"
#include "retas/retas_resources.hpp"
#include "native_ascii_string.hpp"
#include <algorithm>
#include <fstream>
#include <map>
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
struct Ledger {
    CelFolderBudget& budget;std::stop_token stop;
    void poll()const{if(stop.stop_requested())throw std::runtime_error("CutFolder load cancelled");}
    void visit(std::uint64_t n){poll();constexpr auto cap=128ull*1024*1024;if(budget.work>cap||n>cap-budget.work)throw std::length_error("CutFolder load exceeds128Mi work");budget.work+=n;}
    void allocate(std::size_t n){poll();constexpr auto cap=512ull*1024*1024;if(budget.allocation_bytes>cap||n>cap-budget.allocation_bytes)throw std::length_error("CutFolder load exceeds512MiB memory");budget.allocation_bytes+=n;}
};
std::string lower(std::string value){for(auto& c:value)if(c>='A'&&c<='Z')c=char(c+32);return value;}
void path_bound(const Path& path){if(path.empty()||path.native().size()>4096||std::ranges::find(path.native(),Path::value_type{})!=path.native().end())throw std::invalid_argument("Invalid CutFolder load path");}
bool hidden(const Path& path){
#ifdef _WIN32
    const auto flags=GetFileAttributesW(path.c_str());if(flags==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Cannot read CutFolder attributes");return bool(flags&FILE_ATTRIBUTE_HIDDEN);
#else
#ifdef __APPLE__
    struct stat state{};if(::lstat(path.c_str(),&state))throw std::runtime_error("Cannot read CutFolder flags");if(state.st_flags&UF_HIDDEN)return true;
#endif
    return path.filename().string().starts_with('.');
#endif
}
bool readonly(const Path& path){
#ifdef _WIN32
    const auto flags=GetFileAttributesW(path.c_str());if(flags==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Cannot read model attributes");return bool(flags&FILE_ATTRIBUTE_READONLY);
#else
    return (std::filesystem::status(path).permissions()&(std::filesystem::perms::owner_write|std::filesystem::perms::group_write|std::filesystem::perms::others_write))==std::filesystem::perms::none;
#endif
}
Bytes file_bytes(const Path& path,std::size_t cap,Ledger& ledger,bool metadata=false){
    path_bound(path);if(!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))throw std::runtime_error("CutFolder input requires a regular file");std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)throw std::runtime_error("Cannot open CutFolder input");const auto n=file.tellg();if(n<0||std::uint64_t(n)>cap)throw std::length_error("CutFolder input exceeds wire limit");
    ledger.visit(std::uint64_t(n)*(metadata?4:1));ledger.allocate(std::size_t(n)*(metadata?8:1));Bytes bytes(std::size_t(n),0);file.seekg(0);if(!file.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))throw std::runtime_error("Short CutFolder input read");ledger.poll();return bytes;
}
Bytes decode_text(const Bytes& wire,Ledger& ledger){ledger.allocate(wire.size()*3+1);return native_ascii_detail::decode_string(wire,ledger);}
Bytes decode_metadata(const Bytes& wire,Ledger& ledger,NativePreferenceEncoding encoding){
    ledger.allocate(wire.size()*3+1);
    auto value=native_ascii_detail::decode_string(wire,ledger,false);
    (void)decode_native_preference_text(value,encoding);
    return value;
}
std::span<const std::uint8_t> label_prefix(const Bytes& bytes){const auto bounded=std::span(bytes).first(std::min<std::size_t>(bytes.size(),65536));const auto end=std::ranges::find(bounded,0);const auto size=std::size_t(end-bounded.begin());if(size>65535)throw std::length_error("CutFolder label exceeds65535 bytes");return bounded.first(size);}
Bytes label(const Bytes& bytes){const auto value=label_prefix(bytes);native_ascii_detail::ascii_only(value);return {value.begin(),value.end()};}
Bytes runtime_label(const Bytes& bytes,Ledger& ledger){const auto prefix=native_ascii_detail::c_prefix(bytes);ledger.visit(bytes.size()+prefix.size());ledger.allocate(prefix.size()+1);return label(bytes);}
void settings_bound(const CutFolderRootLoadSettings& settings,Ledger& ledger){
    if(settings.model_root.empty())throw std::invalid_argument("CutFolder load requires an explicit native model root");ledger.visit(settings.model_root.size()*8);(void)load_cut_folder_model_path(settings.model_root,ascii(":"),1,ledger.stop);
    if(settings.model_locations.size()>mn6_max_records||settings.image_extensions.size()>mn6_max_records)throw std::length_error("Too many CutFolder model locations/extensions");
    for(const auto& value:settings.category_names){ledger.visit(std::min<std::size_t>(value.size(),65536));const auto prefix=label_prefix(value);ledger.visit(prefix.size()*2+1);ledger.allocate(prefix.size()*2+1);(void)label(value);}
    for(const auto& entry:settings.model_locations){ledger.visit(entry.native_path.size()*8+entry.host_path.native().size()*2+1);if(!entry.host_path.empty())path_bound(entry.host_path);if(entry.native_path.empty()||entry.native_path.find('\0')!=std::string::npos)throw std::invalid_argument("Model location requires a nonempty native path");(void)load_cut_folder_model_path(settings.model_root,ascii(entry.native_path),0,ledger.stop);ledger.allocate(entry.native_path.size()*2+entry.host_path.native().size()*sizeof(Path::value_type)+sizeof(entry)+128);}
    for(const auto& ext:settings.image_extensions){ledger.visit(ext.size()*8+1);if(ext.empty()||ext.size()>255||std::ranges::any_of(ext,[](unsigned char c){return c<33||c>126||c=='.'||c=='/'||c=='\\'||c==':';}))throw std::invalid_argument("CutFolder image registry requires ASCII extensions without dots");ledger.allocate(ext.size()*2+sizeof(ext)+128);}
}
unsigned comparison_bound(std::size_t count){unsigned n=2;while(count){count/=2;n+=2;}return n;}
struct Frozen {
    CutFolderRootLoadSettings value;
    std::map<std::string,std::size_t> locations;
    std::set<std::string> extensions;
};
Frozen freeze(const CutFolderRootLoadSettings& supplied,Ledger& ledger){
    settings_bound(supplied,ledger);Frozen out;out.value.metadata_encoding=supplied.metadata_encoding;out.value.read_only=supplied.read_only;out.value.model_root=load_cut_folder_model_path(supplied.model_root,ascii(":"),1,ledger.stop);out.value.model_locations=supplied.model_locations;out.value.image_extensions=supplied.image_extensions;for(std::size_t i=0;i<9;++i)out.value.category_names[i]=label(supplied.category_names[i]);
    for(std::size_t i=0;i<out.value.model_locations.size();++i){const auto& entry=out.value.model_locations[i];ledger.visit(entry.native_path.size()*comparison_bound(i));out.locations.emplace(lower(entry.native_path),i);}
    for(const auto& ext:out.value.image_extensions){ledger.visit(ext.size()*comparison_bound(out.extensions.size()));out.extensions.insert(lower(ext));}return out;
}
Mn6RootInfo project_info(const Mn6RootInfo& raw,Ledger& ledger,NativePreferenceEncoding encoding){
    auto info=raw;for(auto* suffix:{&info.scalar_suffix,&info.paper_name_suffix,&info.paper_suffix,&info.suffix,&info.base.suffix})suffix->clear();
    for(auto* value:{&info.text_04,&info.text_10,&info.text_1c,&info.text_28,&info.text_48,&info.paper_name})*value=decode_metadata(*value,ledger,encoding);
    const auto defaults=make_mn6_archive(0,{}).info.base;
    for(std::size_t i=0;i<16;++i){auto& l=info.base.lengths[i];if(l.unit==0xfffffffeu)l.unit=defaults.lengths[i].unit;const auto dpi=std::bit_cast<double>(l.dpi_bits);if(!(dpi>0))l.dpi_bits=defaults.lengths[i].dpi_bits;}return info;
}
void categories(Mn6Archive& archive,const CutFolderRootLoadSettings& settings,Ledger& ledger){
    archive.categories_suffix.clear();
    for(auto& group:archive.categories)for(auto& category:group){
        ledger.visit(1);
        // Built-in names are localized resource labels, even in an ASCII project.
        if(category.kind>=0&&category.kind<9)category.name=runtime_label(settings.category_names[std::size_t(category.kind)],ledger);
        else category.name=decode_text(category.name,ledger);
        category.second_text=decode_metadata(category.second_text,ledger,settings.metadata_encoding);category.suffix.clear();
        for(auto& child:category.children){ledger.visit(1);child.first_text=decode_metadata(child.first_text,ledger,settings.metadata_encoding);child.second_text=decode_metadata(child.second_text,ledger,settings.metadata_encoding);child.suffix.clear();}
    }
}
Path companion(const Path& directory,Ledger& ledger){std::size_t count{};for(const auto& item:std::filesystem::directory_iterator(directory)){ledger.visit(item.path().native().size()+1);if(++count>mn6_max_records)throw std::length_error("Too many CutFolder companion entries");if(std::filesystem::is_regular_file(item.symlink_status())&&!hidden(item.path())&&lower(item.path().extension().string())==".mng")return item.path();}return {};}
Path locate(const std::string& native,const Path& directory,const Frozen& frozen,std::string& bound,Ledger& ledger){
    if(native.empty())return {};ledger.visit((native.size()+1)*comparison_bound(frozen.locations.size()));const auto key=lower(native);const auto found=frozen.locations.find(key);const auto& settings=frozen.value;if(found!=frozen.locations.end()){const auto& entry=settings.model_locations[found->second];bound=entry.native_path;return entry.host_path;}
#ifdef _WIN32
    bound=native;return Path(native);
#else
    auto root=settings.model_root;while(root.size()>3&&root.back()=='\\')root.pop_back();const auto prefix=lower(root);
    if(key!=prefix&&!(key.starts_with(prefix)&&key.size()>prefix.size()&&(prefix.ends_with('\\')||key[prefix.size()]=='\\')))return {};
    Path path=directory;std::size_t at=root.size();while(at<native.size()){while(at<native.size()&&native[at]=='\\')++at;const auto end=native.find('\\',at);if(at<native.size())path/=native.substr(at,end==std::string::npos?native.size()-at:end-at);if(end==std::string::npos)break;at=end;}
    bound=native;return path;
#endif
}
bool cel_layered(std::span<const std::uint8_t> bytes,bool dga,Ledger& ledger){
    Reader file(bytes);if(file.u32()!=(dga?0x43444741u:0x4343454cu)||file.u16()>0x1ff)throw FormatError(0,"Invalid model CEL header");(void)file.section();(void)file.section();auto layers=file.section();const auto count=layers.i32();layers.i32();if(count>100000)throw std::length_error("Model CEL probe exceeds100000 layers");for(std::int32_t i=0;i<count;++i){ledger.visit(1);if(layers.u32()==0x10040000u)return true;(void)layers.section();}return false;
}
bool ff06_present(std::span<const std::uint8_t> bytes){
    if(bytes.size()<=retas_resource_directory_size)return false;auto directory=bytes.last(retas_resource_directory_size);constexpr std::uint8_t magic[]{'R','E','T','A','S'};if(!std::equal(std::begin(magic),std::end(magic),directory.begin()))return false;Reader entries(directory.subspan(8),Endian::Little);for(int i=0;i<256;++i){const auto id=entries.u16();entries.take(8);if(id==0xff06)return true;}return false;
}
CutFolderModelBinding bind_model(const std::string& native,const Path& directory,const Frozen& settings,Ledger& ledger){
    CutFolderModelBinding binding;auto path=locate(native,directory,settings,binding.native_path,ledger);if(path.empty()){binding.native_path.clear();return binding;}path_bound(path);const auto state=std::filesystem::symlink_status(path);if(state.type()==std::filesystem::file_type::not_found){binding.native_path.clear();return binding;}
    if(!std::filesystem::is_regular_file(state)&&!std::filesystem::is_directory(state))throw std::invalid_argument("Model binding requires a regular file or directory");binding.host_path=path;
    if(std::filesystem::is_directory(state))return binding;const auto ext=lower(Path(native).extension().string());const bool cel=ext==".cel",dga=ext==".dga";
    ledger.visit((ext.size()+1)*comparison_bound(settings.extensions.size()));const bool registered=!ext.empty()&&settings.extensions.contains(ext.substr(1));if(!cel&&!dga&&(!registered||ext==".tsf"))return binding;
    Bytes bytes;try{bytes=file_bytes(path,512ull*1024*1024,ledger);binding.layered=cel||dga?cel_layered(bytes,dga,ledger):ff06_present(bytes);binding.image=true;}catch(const std::length_error&){throw;}catch(const std::bad_alloc&){throw;}catch(const std::exception&){ledger.poll();return binding;}
    const ModelThumbnailContext context{&ledger.budget,ledger.stop};binding.thumbnail=read_model_thumbnail(bytes,cel?ModelThumbnailContainer::Cel:dga?ModelThumbnailContainer::Dga:ModelThumbnailContainer::Generic,context);
    if(binding.thumbnail||cel||dga)return binding;
    try{binding.thumbnail=make_model_thumbnail(decode_model_thumbnail_source(bytes,ext,context),context);binding.generated=true;}catch(const std::length_error&){throw;}catch(const std::bad_alloc&){throw;}catch(const std::exception&){ledger.poll();binding.image=binding.layered=false;return binding;}
    if(settings.value.read_only||readonly(path)||!binding.thumbnail->image){binding.write_suppressed=true;return binding;}
    auto candidate=store_model_thumbnail(bytes,*binding.thumbnail,context);ledger.poll();write_file_atomic(path,candidate);return binding;
}
Mn6Archive mng_archive(std::span<const std::uint8_t> bytes,const CutFolderRootLoadSettings& settings,Ledger& ledger){
    const auto mng=decode_mng_cut_folder_info(bytes,ledger.stop);std::array<Bytes,9> names;for(std::size_t i=0;i<9;++i){names[i]=runtime_label(settings.category_names[i],ledger);ledger.allocate(names[i].size()+1);}auto archive=make_mn6_archive(2,names);auto& info=archive.info;auto& base=info.base;const auto bits=[](double v){return std::bit_cast<std::uint64_t>(v);};
    const auto metadata=[&](const std::string& value){auto bytes=ascii(value);(void)decode_native_preference_text(bytes,settings.metadata_encoding);return bytes;};info.text_04=metadata(mng.field_0c);info.text_10=metadata(mng.field_18);info.text_1c=metadata(mng.field_24);info.text_28=metadata(mng.field_30);info.field_38=mng.rate;info.field_54=0;
    base.lengths[4]={bits(mng.width),0,bits(mng.dpi>0?mng.dpi:144)};base.lengths[5]={bits(mng.height),0,bits(mng.dpi>0?mng.dpi:144)};for(int i=6;i<10;++i)base.lengths[std::size_t(i)]={bits(0),1,bits(144)};base.dpi_bits=bits(mng.dpi);base.flags=0;
    ledger.poll();return archive;
}
}
CutFolderRoot CutFolderRoot::load(const Path& metadata,const CutFolderRootLoadSettings& supplied,std::stop_token stop){
    CelFolderBudget budget;return load(metadata,supplied,budget,stop);
}
CutFolderRoot CutFolderRoot::load(const Path& metadata,const CutFolderRootLoadSettings& supplied,CelFolderBudget& budget,std::stop_token stop){
    Ledger ledger{budget,stop};ledger.poll();path_bound(metadata);const auto settings=freeze(supplied,ledger);CutFolderRoot out;out.root_=std::filesystem::absolute(metadata).parent_path();out.model_root_=settings.value.model_root;out.metadata_encoding_=settings.value.metadata_encoding;out.read_only_=settings.value.read_only;
    if(lower(metadata.extension().string())==".mng"){
        out.management_path_=std::filesystem::absolute(metadata);out.archive_path_=out.management_path_;out.archive_path_.replace_extension(".mn6");out.archive_=mng_archive(file_bytes(metadata,management_max_bytes,ledger,true),settings.value,ledger);ledger.poll();return out;
    }
    out.archive_path_=std::filesystem::absolute(metadata);out.archive_=decode_mn6_archive(file_bytes(metadata,mn6_max_bytes,ledger,true),stop);auto& archive=out.archive_;archive.version=0x100;archive.info=project_info(archive.info,ledger,settings.value.metadata_encoding);categories(archive,settings.value,ledger);archive.models_suffix.clear();archive.pencil_groups_suffix.clear();archive.tail.clear();
    ledger.allocate(archive.models.size()*(sizeof(Mn6Model)+sizeof(CutFolderModelBinding)+sizeof(CutFolderModelFailure)));std::vector<Mn6Model> admitted;admitted.reserve(archive.models.size());out.models_.reserve(archive.models.size());
    for(std::size_t i=0;i<archive.models.size();++i){ledger.visit(1);try{auto model=std::move(archive.models[i]);model.text_54=decode_metadata(model.text_54,ledger,settings.value.metadata_encoding);model.text_60=decode_metadata(model.text_60,ledger,settings.value.metadata_encoding);model.text_6c=decode_metadata(model.text_6c,ledger,settings.value.metadata_encoding);model.suffix.clear();
        ledger.visit(model.image_path.size()*2+std::uint64_t(std::ranges::count(model.image_path,'\r'))*model.image_path.size()*2+4096);const auto path=load_cut_folder_model_path(settings.value.model_root,model.image_path,model.relative_path_flag,stop);auto binding=bind_model(path,out.root_,settings,ledger);model.image_path=ascii(binding.native_path);model.relative_path_flag=0;admitted.push_back(std::move(model));out.models_.push_back(std::move(binding));
    }catch(const std::length_error&){throw;}catch(const std::bad_alloc&){throw;}catch(const std::exception& error){ledger.poll();const auto n=std::char_traits<char>::length(error.what());ledger.visit(n);ledger.allocate(sizeof(CutFolderModelFailure)+n*2+1);out.model_failures_.push_back({i,error.what()});}}
    archive.models=std::move(admitted);if(archive.pencil_groups.empty())archive.pencil_groups.emplace_back();for(auto& group:archive.pencil_groups){ledger.visit(1);group.canvas_width=640;group.canvas_height=480;for(auto& stroke:group.strokes){ledger.visit(stroke.points.size());stroke.suffix.clear();}}
    if(archive.info.kind!=1)out.management_path_=companion(out.root_,ledger);ledger.poll();return out;
}
void CutFolderRoot::set_direction(std::optional<std::size_t> row,Mn6Model model,
                                  const CutFolderRootLoadSettings& supplied,std::uint32_t now){
    if(closed_||children_active_)throw std::logic_error("CutFolder root is unavailable");
    if(row&&*row>=models_.size())throw std::out_of_range("Invalid CutFolder model");
    if(!row&&models_.size()>=mn6_max_records)throw std::length_error("Too many CutFolder models");
    for(const auto* field:{&model.text_54,&model.text_60,&model.text_6c}){
        if(field->empty()||field->size()>65535||std::ranges::find(*field,0)!=field->end())
            throw std::invalid_argument("Invalid CutFolder direction text");
        (void)decode_native_preference_text(*field,metadata_encoding_);
        (void)native_ascii_detail::encode_string(*field,false);
    }
    CelFolderBudget budget;
    Ledger ledger{budget,{}};
    const auto settings=freeze(supplied,ledger);
    if(settings.value.model_root!=model_root_)throw std::invalid_argument("CutFolder model root changed");
    CutFolderModelBinding binding;
    if(row&&archive_.models[*row].image_path==model.image_path)binding=models_[*row];
    else {
        const auto path=load_cut_folder_model_path(model_root_,model.image_path,model.relative_path_flag);
        binding=bind_model(path,root_,settings,ledger);
    }
    model.image_path=ascii(binding.native_path);
    model.relative_path_flag=0;
    auto candidate=archive_;
    if(row){
        model.field_00=candidate.models[*row].field_00;
        model.time=candidate.models[*row].time;
        candidate.models[*row]=std::move(model);
    }else{
        model.field_00=int(models_.size()+1);
        model.time=now;
        candidate.models.insert(candidate.models.begin(),std::move(model));
    }
    validate_candidate(candidate);
    if(row)models_[*row]=std::move(binding);
    else models_.insert(models_.begin(),std::move(binding));
    archive_=std::move(candidate);
    dirty_=true;
}
CutFolderAdmission admit_cut_folder_root(const Path& directory,const CutFolderRootLoadSettings& settings,std::stop_token stop){
    CelFolderBudget budget;return admit_cut_folder_root(directory,settings,budget,stop);
}
CutFolderAdmission admit_cut_folder_root(const Path& directory,const CutFolderRootLoadSettings& settings,CelFolderBudget& budget,std::stop_token stop){
    Ledger ledger{budget,stop};ledger.poll();path_bound(directory);settings_bound(settings,ledger);if(!std::filesystem::is_directory(std::filesystem::symlink_status(directory)))throw std::invalid_argument("CutFolder admission requires a directory");CutFolderAdmission result;Path mng;std::size_t count{};
    for(const auto& entry:std::filesystem::directory_iterator(directory)){ledger.visit(entry.path().native().size()+1);if(++count>mn6_max_records)throw std::length_error("CutFolder admission exceeds100000 entries");if(!std::filesystem::is_regular_file(entry.symlink_status())||hidden(entry.path()))continue;const auto ext=lower(entry.path().extension().string());if(ext==".mn6")result.selected_path=entry.path();else if(ext==".mng")mng=entry.path();}
    if(result.selected_path.empty()){if(!mng.empty()){result.selected_path=mng;result.kind=2;}ledger.poll();return result;}
    try{const auto root=CutFolderRoot::load(result.selected_path,settings,budget,stop);result.kind=root.archive().info.kind;}catch(const std::length_error&){throw;}catch(const std::bad_alloc&){throw;}catch(const std::exception& error){ledger.poll();result.error=error.what();}ledger.poll();return result;
}
}
