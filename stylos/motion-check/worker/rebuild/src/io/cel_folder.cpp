#include "retas/cel_folder.hpp"
#include "retas/document.hpp"
#include "native_ascii_string.hpp"
#include "native_preference_encoding.hpp"
#include "../core/batch_text.hpp"
#include <fstream>
#include <set>
#include <utility>
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
void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("CelFolder operation cancelled");}
void bounded(std::size_t n){if(n>cel_folder_max_bytes)throw std::length_error("CF06 exceeds64MiB");}
void add(std::size_t& size,std::size_t n){if(n>cel_folder_max_bytes-size)throw std::length_error("CF06 exceeds64MiB");size+=n;}
struct Ledger {
    std::stop_token stop;CelFolderBudget& budget;
    void visit(std::uint64_t n){poll(stop);constexpr auto cap=128ull*1024*1024;if(budget.work>cap||n>cap-budget.work)throw std::length_error("CelFolder exceeds128Mi work");budget.work+=n;}
    void allocate(std::size_t n){constexpr auto cap=512ull*1024*1024;if(budget.allocation_bytes>cap||n>cap-budget.allocation_bytes)throw std::length_error("CelFolder exceeds512MiB memory");budget.allocation_bytes+=n;}
};
std::string runtime_text(std::span<const std::uint8_t> wire,Ledger& ledger,NativePreferenceEncoding encoding){
    ledger.allocate(wire.size()*3+1);const auto value=native_ascii_detail::decode_string(wire,ledger,encoding.code_page==20127);return {value.begin(),value.end()};
}
Bytes wire_text(std::string_view text,NativePreferenceEncoding encoding){
    const auto bytes=std::span(reinterpret_cast<const std::uint8_t*>(text.data()),text.size());
    if(native_ascii_detail::c_prefix(bytes).size()>65535)throw std::length_error("Native CF06 C-prefix exceeds65535");return native_ascii_detail::encode_string(bytes,encoding.code_page==20127);
}
std::string lower(std::string value){for(auto& c:value)if(c>='A'&&c<='Z')c=char(c+32);return value;}
void component(std::string_view name,NativePreferenceEncoding encoding={}){
    const auto text=batch_text_detail::decode(name,encoding);
    if(name.empty()||name.size()>255||text=="."||text==".."||std::any_of(text.begin(),text.end(),[](unsigned char c){return c<32||c==127||c=='/'||c=='\\'||c==':';}))throw std::invalid_argument("CelFolder filename requires a safe component of at most255 encoded bytes");
}
std::string extension(std::string_view name){const auto dot=name.rfind('.');return dot==std::string_view::npos?std::string{}:lower(std::string(name.substr(dot+1)));}
Bytes file_bytes(const Path& path,Ledger& ledger){
    if(!std::filesystem::is_regular_file(std::filesystem::symlink_status(path)))throw std::runtime_error("CF06 metadata requires a regular file");
    std::ifstream stream(path,std::ios::binary|std::ios::ate);if(!stream)throw std::runtime_error("Cannot open CF06 file");
    const auto length=stream.tellg();if(length<0||length>std::streamoff(cel_folder_max_bytes))throw std::length_error("CF06 exceeds64MiB");
    ledger.allocate(std::size_t(length)*3);
    Bytes bytes(static_cast<std::size_t>(length));stream.seekg(0);if(!stream.read(reinterpret_cast<char*>(bytes.data()),std::streamsize(bytes.size())))throw std::runtime_error("Short CF06 file read");return bytes;
}
bool readonly(const Path& path){
#ifdef _WIN32
    const auto flags=GetFileAttributesW(path.c_str());if(flags==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Cannot query CF06 attributes");return bool(flags&FILE_ATTRIBUTE_READONLY);
#else
    const auto permissions=std::filesystem::status(path).permissions();return (permissions&(std::filesystem::perms::owner_write|std::filesystem::perms::group_write|std::filesystem::perms::others_write))==std::filesystem::perms::none;
#endif
}
bool hidden(const Path& path){
#ifdef _WIN32
    const auto flags=GetFileAttributesW(path.c_str());if(flags==INVALID_FILE_ATTRIBUTES)throw std::runtime_error("Cannot query CelFolder attributes");return bool(flags&FILE_ATTRIBUTE_HIDDEN);
#else
#ifdef __APPLE__
    struct stat state{};if(::lstat(path.c_str(),&state)!=0)throw std::runtime_error("Cannot query CelFolder flags");if(state.st_flags&UF_HIDDEN)return true;
#endif
    return path.filename().string().starts_with('.'); // Explicit POSIX host mapping.
#endif
}
// 43B6F0 only inspects the archive header and layer-list prefix. A family hit
// returns immediately; other layers are skipped without decoding their data.
bool cel_probe(const Path& path,const std::string& ext,Ledger& ledger){
    std::ifstream file(path,std::ios::binary|std::ios::ate);if(!file)return false;
    const auto length=file.tellg();if(length<0)return false;if(length>std::streamoff(512ull*1024*1024))throw std::length_error("CelFolder CEL probe exceeds512MiB");
    const auto size=std::size_t(length);std::size_t at{};file.seekg(0);
    const auto integer=[&](unsigned n,std::size_t limit){
        ledger.visit(1);if(at>limit||n>limit-at)throw FormatError(at,"Truncated CelFolder CEL probe");
        std::uint8_t bytes[4]{};if(!file.read(reinterpret_cast<char*>(bytes),n))throw FormatError(at,"Short CelFolder CEL probe");at+=n;std::uint32_t value{};for(unsigned i=0;i<n;++i)value=(value<<8)|bytes[i];return value;
    };
    const auto section=[&](std::size_t limit){const auto n=integer(4,limit);if(n>limit-at)throw FormatError(at,"Truncated CelFolder CEL section");return at+n;};
    const auto seek=[&](std::size_t next){at=next;file.seekg(std::streamoff(at));if(!file)throw FormatError(at,"Cannot seek CelFolder CEL probe");};
    try{
        if(integer(4,size)!=(ext=="cel"?0x4343454cu:0x43444741u)||integer(2,size)>0x1ff)return false;
        for(int i=0;i<2;++i){const auto end=section(size);seek(end);}
        const auto end=section(size);const auto count=std::bit_cast<std::int32_t>(integer(4,end));(void)integer(4,end);
        std::size_t visited{};for(std::int32_t i=0;i<count;++i){if(visited++==cel_folder_max_records)throw std::length_error("CelFolder CEL probe exceeds100000 layers");if(integer(4,end)==0x10040000u)return true;const auto next=section(end);seek(next);}
        return true;
    }catch(const FormatError&){return false;}
}
CelFolderIndex project(const CelFolderMetadata& raw,Ledger& ledger,NativePreferenceEncoding encoding){
    CelFolderIndex result;result.field_70=raw.field_70;result.field_74=raw.field_74;ledger.allocate(raw.records.size()*sizeof(CelFolderEntry));result.entries.reserve(raw.records.size());
    for(const auto& row:raw.records){ledger.visit(1);result.entries.push_back({runtime_text(row.filename,ledger,encoding),runtime_text(row.number,ledger,encoding),row.position});}return result;
}
std::int32_t increment(std::int32_t value){return std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(value)+1u);}
}
CelFolderMetadata decode_cel_folder_metadata(std::span<const std::uint8_t> bytes,std::stop_token stop){
    bounded(bytes.size());poll(stop);Reader r(bytes);if(r.u32()!=0x43463036)throw FormatError(0,"Invalid CF06 magic");
    CelFolderMetadata result;result.version=r.u16();if(result.version>0x1ff)throw FormatError(4,"Unsupported CF06 version");
    auto body=r.section();result.field_70=body.i32();const auto count=body.i32();result.field_74=body.i32();body.require_count(count,12,cel_folder_max_records);result.records.reserve(std::size_t(count));
    for(std::int32_t i=0;i<count;++i){poll(stop);auto row=body.section();CelFolderRecord record;record.filename=row.string();record.number=row.string();record.position=row.i32();record.suffix=row.rest();result.records.push_back(std::move(record));}
    result.body_suffix=body.rest();result.file_suffix=r.rest();return result;
}
Bytes encode_cel_folder_metadata(const CelFolderMetadata& value,std::stop_token stop){
    poll(stop);if(value.version>0x1ff)throw std::invalid_argument("Unsupported CF06 version");if(value.records.size()>cel_folder_max_records)throw std::length_error("CF06 exceeds100000 records");
    std::size_t size=22;add(size,value.body_suffix.size());add(size,value.file_suffix.size());
    for(const auto& row:value.records){poll(stop);if(row.filename.size()>65535||row.number.size()>65535)throw std::length_error("CF06 wire string exceeds65535");add(size,12);add(size,row.filename.size());add(size,row.number.size());add(size,row.suffix.size());}
    Writer w,body;w.u32(0x43463036);w.u16(value.version);body.i32(value.field_70);body.i32(std::int32_t(value.records.size()));body.i32(value.field_74);
    for(const auto& entry:value.records){poll(stop);Writer row;row.string(entry.filename);row.string(entry.number);row.i32(entry.position);row.raw(entry.suffix);body.section(row.data);}
    body.raw(value.body_suffix);w.section(body.data);w.raw(value.file_suffix);return std::move(w.data);
}
CelFolderIndex load_native_cel_folder_index(std::span<const std::uint8_t> bytes,std::stop_token stop){return load_native_cel_folder_index(bytes,stop,{});}
CelFolderIndex load_native_cel_folder_index(std::span<const std::uint8_t> bytes,std::stop_token stop,NativePreferenceEncoding encoding){
    native_preference_detail::validate_encoding(encoding);
    const auto raw=decode_cel_folder_metadata(bytes,stop);CelFolderBudget budget;Ledger ledger{stop,budget};ledger.allocate(bytes.size()*3);return project(raw,ledger,encoding);
}
Bytes save_native_cel_folder_index(const CelFolderIndex& index,std::stop_token stop){return save_native_cel_folder_index(index,stop,{});}
Bytes save_native_cel_folder_index(const CelFolderIndex& index,std::stop_token stop,NativePreferenceEncoding encoding){
    native_preference_detail::validate_encoding(encoding);
    poll(stop);if(index.entries.size()>cel_folder_max_records)throw std::length_error("CF06 exceeds100000 entries");CelFolderMetadata raw;raw.field_70=index.field_70;raw.field_74=index.field_74;
    CelFolderBudget budget;Ledger ledger{stop,budget};ledger.allocate(index.entries.size()*sizeof(CelFolderRecord));raw.records.reserve(index.entries.size());std::size_t size=22;
    for(const auto& entry:index.entries){poll(stop);add(size,12);const auto name=std::string_view(entry.filename).substr(0,entry.filename.find('\0')),number=std::string_view(entry.number).substr(0,entry.number.find('\0'));
        if(name.size()>65535||number.size()>65535)throw std::length_error("Native CF06 C-prefix exceeds65535");ledger.allocate(name.size()+number.size());auto filename=wire_text(name,encoding),label=wire_text(number,encoding);add(size,filename.size());add(size,label.size());raw.records.push_back({std::move(filename),std::move(label),entry.position,{}});}
    return encode_cel_folder_metadata(raw,stop);
}
std::string cel_folder_number(std::string_view filename){return cel_folder_number(filename,{});}
std::string cel_folder_number(std::string_view filename,NativePreferenceEncoding encoding){
    filename=filename.substr(0,filename.find('\0'));component(filename,encoding);std::size_t dot=filename.size();
    for(std::size_t i=0;i<filename.size();){if(native_preference_detail::double_byte(filename,i,encoding)){i+=2;continue;}if(filename[i]=='.')dot=i;++i;}
    std::string stem(filename.substr(0,dot));
    for(std::size_t i=0;i<stem.size();){if(native_preference_detail::double_byte(stem,i,encoding)){i+=2;continue;}auto& c=stem[i++];if(c>='a'&&c<='z')c=char(c-32);}
    // The native numeric search is byte-wise, including possible trail digits.
    auto end=stem.size();while(end&&!(stem[end-1]>='0'&&stem[end-1]<='9'))--end;if(!end)return {};auto begin=end;while(begin&&stem[begin-1]>='0'&&stem[begin-1]<='9')--begin;
    std::uint32_t value{};for(auto i=begin;i<end;++i){const auto digit=std::uint32_t(stem[i]-'0');if(value>(2147483647u-digit)/10)throw std::out_of_range("CelFolder filename number exceeds int32");value=value*10+digit;}if(!value)return {};
    std::array<char,2> suffix{};std::size_t slot{};
    for(auto i=end;i<stem.size()&&slot<2;){if(native_preference_detail::double_byte(stem,i,encoding)){i+=2;continue;}const char c=stem[i++];if((c>='A'&&c<='Z')||(c>='0'&&c<='9')||c=='+'||c=='-')suffix[slot]=c;++slot;}
    auto result=std::to_string(std::min(value,6000u));for(auto c:suffix){if(!c)break;result+=c;}return result;
}
CelFolder::CelFolder(CelFolder&& other)noexcept{*this=std::move(other);}
CelFolder& CelFolder::operator=(CelFolder&& other)noexcept{
    if(this==&other)return *this;
    root_=std::move(other.root_);encoding_=other.encoding_;index_=std::move(other.index_);dirty_=std::exchange(other.dirty_,false);created_=other.created_;closed_=std::exchange(other.closed_,true);write_suppressed_=other.write_suppressed_;return *this;
}
void CelFolder::publish(std::stop_token stop){
    poll(stop);const auto target=root_/"cfinfo.cf6";const auto status=std::filesystem::symlink_status(target);
    if(std::filesystem::is_symlink(status))throw std::invalid_argument("CF06 metadata symlink is unsupported");
    if(std::filesystem::exists(status)&&!std::filesystem::is_regular_file(status))throw std::runtime_error("CF06 metadata target requires a regular file");
    if(std::filesystem::exists(status)&&readonly(target)){dirty_=false;write_suppressed_=true;return;}
    const auto bytes=save_native_cel_folder_index(index_,stop,encoding_);poll(stop);write_file_atomic(target,bytes);dirty_=false;
}
CelFolder CelFolder::open(const Path& directory,std::span<const std::string> image_extensions,std::stop_token stop){
    CelFolderBudget budget;return open(directory,image_extensions,budget,stop);
}
CelFolder CelFolder::open(const Path& directory,std::span<const std::string> image_extensions,CelFolderBudget& budget,std::stop_token stop){return open(directory,image_extensions,budget,stop,{});}
CelFolder CelFolder::open(const Path& directory,std::span<const std::string> image_extensions,CelFolderBudget& budget,std::stop_token stop,NativePreferenceEncoding encoding,bool read_only){
    native_preference_detail::validate_encoding(encoding);
    poll(stop);if(directory.empty()||directory.native().size()>4096||std::ranges::find(directory.native(),Path::value_type{})!=directory.native().end())throw std::invalid_argument("Invalid CelFolder directory");
    CelFolder result;result.encoding_=encoding;result.root_=std::filesystem::canonical(directory);if(!std::filesystem::is_directory(result.root_))throw std::invalid_argument("CelFolder root is not a directory");
    Ledger ledger{stop,budget};std::set<std::string> extensions;
    if(image_extensions.size()>cel_folder_max_records)throw std::length_error("Too many CelFolder image extensions");
    for(const auto& ext:image_extensions){poll(stop);component(ext);if(ext.find('.')!=std::string::npos)throw std::invalid_argument("CelFolder image extension requires no dot");ledger.allocate(ext.size()+sizeof(std::string)*3);extensions.insert(lower(ext));}
    const auto metadata=result.root_/"cfinfo.cf6";const auto status=std::filesystem::symlink_status(metadata);if(std::filesystem::is_symlink(status))throw std::invalid_argument("CF06 metadata symlink is unsupported");
    result.created_=!std::filesystem::exists(status);if(!result.created_){const auto bytes=file_bytes(metadata,ledger);const auto raw=decode_cel_folder_metadata(bytes,stop);result.index_=project(raw,ledger,encoding);}
    std::set<std::string> names;std::vector<CelFolderEntry> retained;ledger.allocate(result.index_.entries.size()*sizeof(CelFolderEntry));retained.reserve(result.index_.entries.size());
    for(auto& entry:result.index_.entries){ledger.visit(1);component(entry.filename,encoding);if(std::filesystem::exists(result.root_/batch_text_detail::host_path(entry.filename,encoding))){ledger.allocate(entry.filename.size()+sizeof(std::string)*3);names.insert(entry.filename);retained.push_back(std::move(entry));}else result.dirty_=true;}
    result.index_.entries=std::move(retained);std::size_t visited{};
    for(const auto& child:std::filesystem::directory_iterator(result.root_)){
        ledger.visit(1);if(visited++==cel_folder_max_records)throw std::length_error("CelFolder directory exceeds100000 entries");const auto state=child.symlink_status();if(std::filesystem::is_symlink(state))continue;
        if(!std::filesystem::is_regular_file(state)||hidden(child.path()))continue;const auto name=batch_text_detail::native_path(child.path().filename(),encoding);component(name,encoding);const auto ext=extension(name);
        if(ext=="lnk"||ext=="tsf")continue;if(ext=="cel"||ext=="dga"){if(!cel_probe(child.path(),ext,ledger))continue;}else if(!extensions.contains(ext))continue;
        ledger.visit(name.size());if(names.contains(name))continue;if(result.index_.entries.size()==cel_folder_max_records)throw std::length_error("CelFolder index exceeds100000 entries");
        const auto position=result.index_.entries.empty()?0:increment(result.index_.entries.back().position);
        const auto number=cel_folder_number(name,encoding);ledger.allocate(sizeof(CelFolderEntry)+name.size()*2+number.size()+sizeof(std::string)*3);
        if(!result.index_.entries.empty()&&result.index_.field_70<=position)result.index_.field_70=increment(position);
        result.index_.entries.push_back({name,number,position});names.insert(name);result.dirty_=true;
    }
    poll(stop);if(read_only)result.dirty_=false;else if(result.created_)result.publish(stop);return result;
}
void CelFolder::set_position(std::string_view filename,std::int32_t position){
    if(closed_)throw std::logic_error("CelFolder is closed");
    if(position<0||position==INT32_MAX)throw std::out_of_range("Invalid film position");
    auto& entries=index_.entries;
    auto found=std::ranges::find(entries,filename,&CelFolderEntry::filename);
    if(found==entries.end())throw std::invalid_argument("Image missing from CelFolder");
    if(std::ranges::any_of(entries,[&](const auto& item){return &item!=&*found&&item.position==position;}))
        throw std::invalid_argument("Film position is occupied");
    if(found->position==position)return;
    found->position=position;
    std::stable_sort(entries.begin(),entries.end(),[](const auto& a,const auto& b){return a.position<b.position;});
    index_.field_70=std::max(index_.field_70,position+1);
    dirty_=true;
}
void CelFolder::close(std::stop_token stop){if(closed_)return;poll(stop);if(dirty_)publish(stop);closed_=true;}
}
