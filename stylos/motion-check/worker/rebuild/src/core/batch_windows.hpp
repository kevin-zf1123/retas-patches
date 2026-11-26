#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include "windows_path.hpp"
#include <windows.h>
#include <bcrypt.h>
#include <algorithm>
#include <array>
#include <cstring>
#include <filesystem>
#include <span>
#include <stop_token>
#include <system_error>
#include <utility>
#include <vector>

namespace retas::batch_windows {
using Path=std::filesystem::path;
using windows_filesystem::api_path;
[[noreturn]] inline void error(const char* message,DWORD code=::GetLastError()){throw std::system_error(int(code),std::system_category(),message);}
inline void poll(std::stop_token stop){if(stop.stop_requested())throw std::runtime_error("Batch filesystem operation cancelled");}
struct Handle {
    HANDLE value{INVALID_HANDLE_VALUE};
    Handle()=default;explicit Handle(HANDLE h):value(h){}
    Handle(const Handle&)=delete;Handle& operator=(const Handle&)=delete;
    Handle(Handle&& h)noexcept:value(std::exchange(h.value,INVALID_HANDLE_VALUE)){}
    Handle& operator=(Handle&& h)noexcept{if(this!=&h){if(value!=INVALID_HANDLE_VALUE)::CloseHandle(value);value=std::exchange(h.value,INVALID_HANDLE_VALUE);}return *this;}
    ~Handle(){if(value!=INVALID_HANDLE_VALUE)::CloseHandle(value);}
};
struct State {
    std::uint64_t st_dev{},st_ino{},inode_high{},modified{};
    std::int64_t st_size{};
    DWORD attributes{};
    bool directory{};
};
inline State state(HANDLE handle){
    FILE_ID_INFO id{};FILE_BASIC_INFO basic{};FILE_STANDARD_INFO standard{};
    if(::GetFileType(handle)!=FILE_TYPE_DISK)throw std::invalid_argument("Batch special file is unsupported");
    if(!::GetFileInformationByHandleEx(handle,FileIdInfo,&id,sizeof(id))||!::GetFileInformationByHandleEx(handle,FileBasicInfo,&basic,sizeof(basic))||!::GetFileInformationByHandleEx(handle,FileStandardInfo,&standard,sizeof(standard)))error("Identify Batch file");
    if(basic.FileAttributes&FILE_ATTRIBUTE_REPARSE_POINT)throw std::invalid_argument("Batch reparse point is unsupported");
    State result;result.st_dev=id.VolumeSerialNumber;std::memcpy(&result.st_ino,id.FileId.Identifier,8);std::memcpy(&result.inode_high,id.FileId.Identifier+8,8);
    result.modified=std::uint64_t(basic.LastWriteTime.QuadPart);result.st_size=standard.EndOfFile.QuadPart;result.attributes=basic.FileAttributes;result.directory=standard.Directory!=FALSE;return result;
}
inline bool same_identity(const State& a,const State& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino&&a.inode_high==b.inode_high&&a.directory==b.directory;}
inline bool same_generation(const State& a,const State& b){return same_identity(a,b)&&a.st_size==b.st_size&&a.modified==b.modified;}
inline Handle open(const Path& path,DWORD access,DWORD sharing=FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,DWORD disposition=OPEN_EXISTING){
    const auto native=api_path(path);Handle file(::CreateFileW(native.c_str(),access,sharing,nullptr,disposition,FILE_FLAG_OPEN_REPARSE_POINT|FILE_FLAG_BACKUP_SEMANTICS,nullptr));
    if(file.value==INVALID_HANDLE_VALUE)error("Open Batch file");return file;
}
inline State state(const Path& path){auto handle=open(path,FILE_READ_ATTRIBUTES);return state(handle.value);}
inline void component(const Path& name){
    const auto value=name.native();if(value.empty()||value==L"."||value==L"..")return;
    if(value.back()==L'.'||value.back()==L' '||value.find_first_of(L"<>:\"|?*")!=value.npos||std::any_of(value.begin(),value.end(),[](wchar_t c){return c<32;}))throw std::invalid_argument("Batch Windows filename is invalid or aliases another name");
    auto base=value.substr(0,value.find(L'.'));for(auto& c:base)if(c>=L'a'&&c<=L'z')c-=L'a'-L'A';
    if(base==L"CON"||base==L"PRN"||base==L"AUX"||base==L"NUL"||((base.starts_with(L"COM")||base.starts_with(L"LPT"))&&base.size()==4&&((base[3]>=L'1'&&base[3]<=L'9')||base[3]==L'\u00b9'||base[3]==L'\u00b2'||base[3]==L'\u00b3')))throw std::invalid_argument("Batch Windows device filename is unsupported");
}
inline void path_components(const Path& requested){
    const auto path=std::filesystem::absolute(requested).lexically_normal();Path current=path.root_path();
    for(const auto& c:path.relative_path()){component(c);current/=c;const auto attrs=::GetFileAttributesW(api_path(current).c_str());
        if(attrs==INVALID_FILE_ATTRIBUTES){const auto code=::GetLastError();if(code==ERROR_FILE_NOT_FOUND||code==ERROR_PATH_NOT_FOUND)continue;error("Inspect Batch path ancestor",code);}
        if(attrs&FILE_ATTRIBUTE_REPARSE_POINT)throw std::invalid_argument("Batch path ancestor reparse point is unsupported");}
}
inline DWORD read(HANDLE handle,std::span<std::uint8_t> buffer){DWORD count{};if(!::ReadFile(handle,buffer.data(),DWORD(buffer.size()),&count,nullptr))error("Read Batch file");return count;}
inline void write(HANDLE handle,std::span<const std::uint8_t> bytes,std::stop_token stop){
    std::size_t done{};while(done<bytes.size()){poll(stop);DWORD count{};if(!::WriteFile(handle,bytes.data()+done,DWORD(std::min<std::size_t>(bytes.size()-done,1<<20)),&count,nullptr))error("Write Batch stage");if(!count)throw std::runtime_error("Short Batch stage write");done+=count;}
    if(!::FlushFileBuffers(handle))error("Sync Batch stage");
}
inline std::wstring random_name(){std::array<unsigned char,16> random{};if(::BCryptGenRandom(nullptr,random.data(),ULONG(random.size()),BCRYPT_USE_SYSTEM_PREFERRED_RNG)<0)throw std::runtime_error("Batch random namespace unavailable");std::wstring name=L".paintman-batch-";for(auto c:random){name.push_back(L"0123456789abcdef"[c>>4]);name.push_back(L"0123456789abcdef"[c&15]);}return name;}
inline Path unused_name(const Path& parent){for(int n=0;n<64;++n){const auto path=parent/random_name();if(::GetFileAttributesW(api_path(path).c_str())!=INVALID_FILE_ATTRIBUTES)continue;const auto code=::GetLastError();if(code==ERROR_FILE_NOT_FOUND)return path;error("Inspect Batch temporary namespace",code);}throw std::runtime_error("Batch temporary namespace exhausted");}
struct FileStage {
    Path path;Handle file;bool active{true};
    FileStage(const Path& parent,std::stop_token stop){for(int n=0;n<64;++n){poll(stop);path=parent/random_name();file.value=::CreateFileW(api_path(path).c_str(),GENERIC_WRITE|FILE_READ_ATTRIBUTES|DELETE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(file.value!=INVALID_HANDLE_VALUE)return;const auto code=::GetLastError();if(code!=ERROR_FILE_EXISTS&&code!=ERROR_ALREADY_EXISTS)error("Create Batch output stage",code);}throw std::runtime_error("Batch output stage namespace exhausted");}
    ~FileStage(){if(active&&file.value!=INVALID_HANDLE_VALUE){FILE_DISPOSITION_INFO remove{TRUE};(void)::SetFileInformationByHandle(file.value,FileDispositionInfo,&remove,sizeof(remove));}}
};
inline Handle directory(const Path& path,DWORD access=FILE_READ_ATTRIBUTES){auto handle=open(path,access);if(!state(handle.value).directory)throw std::invalid_argument("Batch directory is not a directory");return handle;}
inline std::vector<std::uint8_t> rename_record(const Path& destination){
    const auto name=api_path(destination);const auto bytes=name.size()*sizeof(wchar_t);if(bytes>32767*sizeof(wchar_t))throw std::length_error("Batch rename path too long");
    std::vector<std::uint8_t> buffer(std::max(sizeof(FILE_RENAME_INFO),offsetof(FILE_RENAME_INFO,FileName)+bytes+sizeof(wchar_t)),0);auto* record=reinterpret_cast<FILE_RENAME_INFO*>(buffer.data());record->ReplaceIfExists=FALSE;record->RootDirectory=nullptr;record->FileNameLength=DWORD(bytes);std::memcpy(record->FileName,name.data(),bytes);return buffer;
}
inline Path handle_path(HANDLE handle){std::array<wchar_t,32768> name{};const auto n=::GetFinalPathNameByHandleW(handle,name.data(),DWORD(name.size()),FILE_NAME_NORMALIZED|VOLUME_NAME_DOS);if(!n||n>=name.size())error("Locate owned Batch stage");return Path(std::wstring_view(name.data(),n));}
inline bool rename(HANDLE handle,std::vector<std::uint8_t>& record)noexcept{return ::SetFileInformationByHandle(handle,FileRenameInfo,record.data(),DWORD(record.size()))!=FALSE;}
}
