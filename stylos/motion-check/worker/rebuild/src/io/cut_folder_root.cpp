#include "retas/cut_folder_root.hpp"
#include "retas/document.hpp"
#include "retas/management_file.hpp"
#include "retas/numeric.hpp"
#include "retas/cut_folder_model_path.hpp"
#include "native_ascii_string.hpp"
#include "cut_folder_export_storage.hpp"
#include <algorithm>
#include <charconv>
#include <utility>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif

namespace retas {
namespace {
using Path=std::filesystem::path;
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("CutFolder root operation cancelled");}
void component(std::string_view value){if(value.empty()||value.size()>255||value=="."||value==".."||std::any_of(value.begin(),value.end(),[](unsigned char c){return c<32||c>=127||c=='/'||c=='\\'||c==':'||c=='*'||c=='?'||c=='\"'||c=='<'||c=='>'||c=='|';})||value.back()=='.'||value.back()==' ')throw std::invalid_argument("CutFolder root requires safe ASCII components of at most255 bytes");}
void path_bound(const Path& path){if(path.empty()||path.native().size()>4096||std::ranges::find(path.native(),Path::value_type{})!=path.native().end())throw std::invalid_argument("Invalid CutFolder root path");}
Bytes text(std::span<const std::uint8_t> value){const auto prefix=native_ascii_detail::c_prefix(value);if(prefix.size()>65535)throw std::length_error("CutFolder text exceeds65535 bytes");native_ascii_detail::ascii_only(prefix);return {prefix.begin(),prefix.end()};}
Bytes metadata_text(std::span<const std::uint8_t> value,NativePreferenceEncoding encoding){const auto prefix=native_ascii_detail::c_prefix(value);if(prefix.size()>65535)throw std::length_error("CutFolder text exceeds65535 bytes");(void)decode_native_preference_text(prefix,encoding);return {prefix.begin(),prefix.end()};}
bool positive(std::uint64_t bits){return !(bits>>63)&&(bits&0x7fffffffffffffffull)&&((bits&0x7ff0000000000000ull)!=0x7ff0000000000000ull||!(bits&0x000fffffffffffffull));}
Mn6BaseInfo assign_paper(const Mn6BaseInfo& current,const Mn6BaseInfo& source){
    Mn6BaseInfo out;out.frame_mode=source.frame_mode;out.flags=source.flags;out.scale_bits=source.scale_bits;out.dpi_bits=source.dpi_bits;out.dialog_dpi_bits=source.dialog_dpi_bits;
    for(std::size_t i=0;i<16;++i){out.lengths[i]=current.lengths[i];out.lengths[i].value_bits=source.lengths[i].value_bits;if(source.lengths[i].unit!=0xfffffffeu)out.lengths[i].unit=source.lengths[i].unit;if(positive(source.lengths[i].dpi_bits))out.lengths[i].dpi_bits=source.lengths[i].dpi_bits;}return out;
}
Mn6RootInfo assign_info(const Mn6RootInfo& current,const CutFolderNewMetadata& source,NativePreferenceEncoding encoding={}){
    Mn6RootInfo out;out.kind=current.kind;out.field_34=current.field_34;out.field_38=source.field_38;out.field_54=source.field_54;out.paper_name=metadata_text(source.paper_name,encoding);out.text_04=metadata_text(source.text_04,encoding);out.text_10=metadata_text(source.text_10,encoding);out.text_1c=metadata_text(source.text_1c,encoding);out.text_28=metadata_text(source.text_28,encoding);out.text_48=metadata_text(source.text_48,encoding);out.base=assign_paper(current.base,source.paper);return out;
}
Bytes archive_bytes(const Mn6Archive& state,std::string_view model_root,std::stop_token stop,NativePreferenceEncoding encoding={}){
    const auto encode=[&](const Bytes& value){return native_ascii_detail::encode_string(metadata_text(value,encoding),false);};
    auto wire=state;
    auto& info=wire.info;
    for(auto* value:{&info.paper_name,&info.text_04,&info.text_10,&info.text_1c,&info.text_28,&info.text_48}){
        poll(stop);*value=encode(*value);
    }
    for(auto& group:wire.categories)for(auto& category:group){
        poll(stop);category.name=native_ascii_detail::encode_string(category.name);
        category.second_text=encode(category.second_text);
        for(auto& child:category.children){
            child.first_text=encode(child.first_text);
            child.second_text=encode(child.second_text);
        }
    }
    for(auto& model:wire.models){
        poll(stop);
        for(auto* value:{&model.text_54,&model.text_60,&model.text_6c})*value=encode(*value);
        const auto reference=save_cut_folder_model_path(model_root,std::string_view(reinterpret_cast<const char*>(model.image_path.data()),model.image_path.size()),stop);
        model.image_path=reference.image_path;model.relative_path_flag=reference.relative_path_flag;
    }
    for(auto& group:wire.pencil_groups){group.canvas_width=640;group.canvas_height=480;for(auto& stroke:group.strokes){poll(stop);stroke.coordinate_width=std::ranges::all_of(stroke.points,[](const auto& p){return p.x<=65535&&p.y<=65535;})?2:4;if(stroke.coordinate_width==2)for(auto& p:stroke.points){p.x=std::uint16_t(p.x);p.y=std::uint16_t(p.y);}}}return encode_mn6_archive(wire,stop);
}
bool readonly(const Path& path){
    if(!std::filesystem::exists(path))return false;
#ifdef _WIN32
    const auto flags=GetFileAttributesW(path.c_str());if(flags==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Cannot query CutFolder root attributes");return bool(flags&FILE_ATTRIBUTE_READONLY);
#else
    return (std::filesystem::status(path).permissions()&(std::filesystem::perms::owner_write|std::filesystem::perms::group_write|std::filesystem::perms::others_write))==std::filesystem::perms::none;
#endif
}
void regular_target(const Path& path){const auto state=std::filesystem::symlink_status(path);if(state.type()!=std::filesystem::file_type::not_found&&!std::filesystem::is_regular_file(state))throw std::invalid_argument("CutFolder metadata target requires a regular file or new path");}
std::int32_t pixel(const Mn6Length& l,double dpi){
    using numeric_detail::binary64;const auto value=std::bit_cast<double>(l.value_bits),ref=std::bit_cast<double>(l.dpi_bits);double v{};
    switch(l.unit){case 0:if(ref==0)return 0;v=binary64(binary64(value/ref)*dpi);break;case 1:v=binary64(binary64(dpi*value)*0.3937007874015748);break;case 2:v=binary64(binary64(dpi*value)*0.03937007874015748);break;case 3:v=binary64(dpi*value);break;case 5:v=binary64(binary64(dpi*value)*0.01388888888888889);break;case 6:v=binary64(binary64(dpi*value)*0.00984251968503937);break;default:return 0;}
    const auto result=legacy_rt_double_to_int(v);if(!result)throw std::invalid_argument("CutFolder management dimensions exceed finite int32");return *result;
}
Bytes management_bytes(const Mn6RootInfo& info,std::stop_token stop){
    poll(stop);const auto dpi=std::bit_cast<double>(info.base.dpi_bits);if(!std::isfinite(dpi)||std::trunc(dpi)<INT32_MIN||std::trunc(dpi)>INT32_MAX)throw std::invalid_argument("CutFolder management DPI exceeds finite int32");
    auto bytes=make_management_file();set_management_integer(bytes,1,pixel(info.base.lengths[4],dpi),stop);set_management_integer(bytes,2,pixel(info.base.lengths[5],dpi),stop);set_management_integer(bytes,3,std::int32_t(dpi),stop);set_management_integer(bytes,4,info.field_38,stop);
    for(auto [tag,value]:{std::pair{5u,&info.text_04},std::pair{6u,&info.text_10},std::pair{7u,&info.text_28},std::pair{8u,&info.text_1c}})set_management_string(bytes,tag,std::string_view(reinterpret_cast<const char*>(value->data()),value->size()),stop);return bytes;
}
std::string padded(const Bytes& input,std::size_t width){
    const auto bytes=text(input);std::string value(bytes.begin(),bytes.end());if(!value.empty()&&(value[0]<'0'||value[0]>'9'))return value;const auto end=value.find_first_not_of("0123456789");const auto n=end==std::string::npos?value.size():end;
    std::uint32_t number{};for(std::size_t i=0;i<n;++i){const auto digit=unsigned(value[i]-'0');if(number>(std::uint32_t(INT32_MAX)-digit)/10)throw std::out_of_range("CutFolder name number exceeds int32");number=number*10+digit;}
    auto decimal=std::to_string(number);if(decimal.size()<width)decimal.insert(0,width-decimal.size(),'0');decimal+=value.substr(n);return decimal;
}
std::vector<Path> directories(const Path& root,std::uint16_t kind,const CutFolderLayout& layout,std::stop_token stop){
    const auto count=std::int64_t(layout.layer_count)+(kind==2?1:0)-(layout.omit_background!=0);if(count>100000)throw std::length_error("CutFolder layout exceeds100000 layers");std::vector<Path> out;
    const auto append=[&](Path path){poll(stop);if(out.size()==100000)throw std::length_error("CutFolder layout exceeds100000 directories");path_bound(path);out.push_back(std::move(path));};
    if(kind==2){
        const auto size=std::size_t(std::max<std::int64_t>(0,count));if((!layout.key.empty()&&layout.key.size()<size)||(!layout.inbetween.empty()&&layout.inbetween.size()<size))throw std::invalid_argument("CutFolder layout flags do not cover its layers");
        for(std::int64_t n=0;n<count;++n){const auto name=cut_folder_layer_name(std::int32_t(n));component(name);const auto parent=root/name;append(parent);const bool key=!layout.key.empty()&&layout.key[std::size_t(n)],inbetween=!layout.inbetween.empty()&&layout.inbetween[std::size_t(n)];if(key||inbetween)append(parent/name);if(key)append(parent/(name+"+"));if(inbetween)append(parent/(name+"-"));}append(root/"-LO");
    }else{for(const auto* name:{"Layout","Key Animation","Inbetween","BG"})append(root/name);for(std::int64_t n=1;n<=count;++n){const auto name=cut_folder_layer_name(std::int32_t(n));component(name);append(root/"Key Animation"/name);append(root/"Inbetween"/name);}}return out;
}
}
Bytes cut_folder_detail::encode_runtime_mn6(const Mn6Archive& state,std::string_view native_root,NativePreferenceEncoding encoding,std::stop_token stop){return archive_bytes(state,native_root,stop,encoding);}
Bytes cut_folder_detail::encode_runtime_mng(const Mn6RootInfo& info,std::stop_token stop){return management_bytes(info,stop);}
CutFolderNewMetadata::CutFolderNewMetadata(){const auto value=make_mn6_archive(0,{});paper=value.info.base;text_10=text_1c=text_28=ascii("1");}
std::string cut_folder_name(std::int32_t kind,const CutFolderNewMetadata& info){
    const auto first=padded(info.text_10,3),second=padded(info.text_1c,3),third=padded(info.text_28,4);switch(kind){case 0:return "C-"+second+"-"+third;case 1:return second+"-"+third;case 2:return first+"-"+second+"-"+third;case 3:{const auto p=text(info.text_04);return std::string(p.begin(),p.end())+"-"+first+"-"+second+"-"+third;}default:return {};}
}
std::string cut_folder_layer_name(std::int32_t index){
    if(index==0)return "-BG";const auto number=std::bit_cast<std::uint32_t>(index)-1u,q=number/26,r=number%26;std::string out;const auto prefix=std::uint8_t(q+0x40);if(q&&prefix)out.push_back(char(prefix));out.push_back(char('A'+r));return out;
}
CutFolderNewMetadata project_cut_folder_new_metadata(const Mn6RootInfo& wire,std::stop_token stop){
    struct Work {std::uint64_t used{};std::stop_token stop;void visit(std::uint64_t n){poll(stop);constexpr auto cap=128ull*1024*1024;if(n>cap-used)throw std::length_error("CutFolder string projection exceeds128Mi work");used+=n;}} work{0,stop};
    CutFolderNewMetadata out;const auto decode=[&](const Bytes& v){if(v.size()>65535)throw std::length_error("CutFolder wire text exceeds65535 bytes");return native_ascii_detail::decode_string(v,work);};
    poll(stop);out.paper=assign_paper(out.paper,wire.base);out.paper_name=decode(wire.paper_name);out.text_04=decode(wire.text_04);out.text_10=decode(wire.text_10);out.text_1c=decode(wire.text_1c);out.text_28=decode(wire.text_28);out.text_48=decode(wire.text_48);out.field_38=wire.field_38;out.field_54=wire.field_54;return out;
}
CutFolderRoot::CutFolderRoot(CutFolderRoot&& other)noexcept{*this=std::move(other);}
CutFolderRoot& CutFolderRoot::operator=(CutFolderRoot&& other)noexcept{if(this!=&other){++children_revision_;++other.children_revision_;root_=std::move(other.root_);archive_path_=std::move(other.archive_path_);management_path_=std::move(other.management_path_);archive_=std::move(other.archive_);model_root_=std::move(other.model_root_);metadata_encoding_=other.metadata_encoding_;models_=std::move(other.models_);model_failures_=std::move(other.model_failures_);children_=std::move(other.children_);child_failures_=std::move(other.child_failures_);dirty_=std::exchange(other.dirty_,false);closed_=std::exchange(other.closed_,true);write_suppressed_=other.write_suppressed_;read_only_=other.read_only_;}return *this;}
std::optional<CutFolderRoot> CutFolderRoot::create(const Path& parent,const CutFolderNewSettings& settings,std::stop_token stop){
    poll(stop);path_bound(parent);const auto name=settings.name.empty()?cut_folder_name(settings.name_kind,settings.metadata):settings.name;component(name);const auto root=std::filesystem::absolute(parent)/name;path_bound(root);if(std::filesystem::symlink_status(root).type()!=std::filesystem::file_type::not_found)return {};
    if(!std::filesystem::is_directory(std::filesystem::symlink_status(parent)))throw std::invalid_argument("CutFolder parent requires an existing directory");
    // Freeze all supplied owners and preflight names before creating anything.
    CutFolderRoot out;out.root_=root;component(name+".mn6");out.archive_path_=root/(name+".mn6");if(settings.kind==2)out.management_path_=root/(name+".mng");path_bound(out.archive_path_);if(!out.management_path_.empty())path_bound(out.management_path_);
    std::array<Bytes,9> labels;for(std::size_t n=0;n<9;++n)labels[n]=text(settings.category_names[n]);out.archive_=make_mn6_archive(settings.kind,labels);out.metadata_encoding_=settings.metadata_encoding;out.archive_.info=assign_info(out.archive_.info,settings.metadata,out.metadata_encoding_);const auto paths=directories(root,settings.kind,settings.layout,stop);(void)archive_bytes(out.archive_,out.model_root_,stop,out.metadata_encoding_);poll(stop);
    if(!std::filesystem::create_directory(root))return {};try{for(const auto& path:paths){poll(stop);if(!std::filesystem::create_directory(path)&&!std::filesystem::is_directory(path))throw std::runtime_error("Cannot create CutFolder child directory");}out.save(stop);}catch(...){std::error_code ignored;std::filesystem::remove_all(root,ignored);throw;}return out;
}
void CutFolderRoot::set_metadata(const CutFolderNewMetadata& info,std::stop_token stop){if(closed_||children_active_)throw std::logic_error("CutFolder root is closed or expanding children");poll(stop);auto next=assign_info(archive_.info,info,metadata_encoding_);poll(stop);archive_.info=std::move(next);dirty_=true;}
void CutFolderRoot::validate_candidate(const Mn6Archive& candidate)const{
    (void)archive_bytes(candidate,model_root_,{},metadata_encoding_);
    if(candidate.info.kind==2)(void)management_bytes(candidate.info,{});
}
void CutFolderRoot::set_category_progress(std::size_t group,std::size_t row,Bytes name,std::optional<std::uint32_t> date){
    if(closed_||children_active_)throw std::logic_error("CutFolder root is unavailable");
    auto candidate=archive_;auto& category=candidate.categories.at(group).at(row);
    name=metadata_text(name,metadata_encoding_);
    (void)native_ascii_detail::encode_string(name,false);
    category.second_text=std::move(name);
    category.field_1c=int(date.has_value());
    category.time=date.value_or(UINT32_MAX);
    validate_candidate(candidate);archive_=std::move(candidate);dirty_=true;
}
void CutFolderRoot::append_category_check(std::size_t group,std::size_t row,Mn6CategoryChild check){
    if(closed_||children_active_)throw std::logic_error("CutFolder root is unavailable");
    auto candidate=archive_;auto& category=candidate.categories.at(group).at(row);
    if(category.children.size()>=mn6_max_records)throw std::length_error("Too many CutFolder checks");
    check.first_text=metadata_text(check.first_text,metadata_encoding_);
    check.second_text=metadata_text(check.second_text,metadata_encoding_);
    if(check.first_text.empty()||check.field_0c<0||check.field_0c>2)throw std::invalid_argument("Invalid CutFolder check");
    (void)native_ascii_detail::encode_string(check.first_text,false);
    (void)native_ascii_detail::encode_string(check.second_text,false);
    category.children.push_back(std::move(check));
    validate_candidate(candidate);archive_=std::move(candidate);dirty_=true;
}
void CutFolderRoot::save(std::stop_token stop){
    if(closed_||children_active_)throw std::logic_error("CutFolder root is closed or expanding children");poll(stop);regular_target(archive_path_);write_suppressed_=false;if(readonly(archive_path_)){dirty_=false;write_suppressed_=true;return;}
    const auto bytes=archive_bytes(archive_,model_root_,stop,metadata_encoding_);poll(stop);write_file_atomic(archive_path_,bytes);
    if(archive_.info.kind==2&&!management_path_.empty()&&!readonly(archive_path_)){regular_target(management_path_);const auto management=management_bytes(archive_.info,stop);poll(stop);write_file_atomic(management_path_,management);}dirty_=false;
}
void CutFolderRoot::close(std::stop_token stop){if(children_active_)throw std::logic_error("CutFolder root is expanding children");if(closed_)return;poll(stop);if(dirty_&&!read_only_)save(stop);clear_children(0,stop);closed_=true;}
}
