#include "retas/document.hpp"
#include "../core/native_storage.hpp"
#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <system_error>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include "../core/windows_path.hpp"
#include "../core/batch_windows.hpp"
#else
#include <dirent.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace retas {
Bytes read_file(const std::filesystem::path& path){
#ifdef _WIN32
    std::ifstream in(std::filesystem::path(windows_filesystem::api_path(path)),std::ios::binary|std::ios::ate);
#else
    std::ifstream in(path,std::ios::binary|std::ios::ate);
#endif
    if(!in)throw std::runtime_error("Cannot open file");
    auto size=in.tellg();
    if(size<0 || size>std::streamoff(512ull*1024*1024))throw std::runtime_error("File exceeds supported limit");
    Bytes data(static_cast<std::size_t>(size));in.seekg(0);
    if(!in.read(reinterpret_cast<char*>(data.data()),static_cast<std::streamsize>(data.size())))throw std::runtime_error("Short file read");
    return data;
}
void storage_detail::write_checked(const std::filesystem::path& path,std::span<const std::uint8_t> data,const std::function<void()>& before_replace){
#ifdef _WIN32
    const auto target=windows_filesystem::api_path(path);
    auto dir=path.parent_path();if(dir.empty())dir=L".";
    wchar_t temp[MAX_PATH];
    if(!GetTempFileNameW(dir.c_str(),L"pmr",0,temp))throw std::system_error(GetLastError(),std::system_category(),"Create save temporary");
    HANDLE file=CreateFileW(temp,GENERIC_WRITE,0,nullptr,TRUNCATE_EXISTING,FILE_ATTRIBUTE_NORMAL,nullptr);
    if(file==INVALID_HANDLE_VALUE){DeleteFileW(temp);throw std::system_error(GetLastError(),std::system_category(),"Open save temporary");}
    try {
        std::size_t done=0;
        while(done<data.size()){
            DWORD written=0;const auto count=DWORD(std::min<std::size_t>(data.size()-done,1<<20));
            if(!WriteFile(file,data.data()+done,count,&written,nullptr)||!written)throw std::runtime_error("Save write failed");
            done+=written;
        }
        if(!FlushFileBuffers(file))throw std::runtime_error("Save flush failed");
        if(!CloseHandle(file)){file=INVALID_HANDLE_VALUE;throw std::runtime_error("Save close failed");}file=INVALID_HANDLE_VALUE;
        if(before_replace)before_replace();
        if(!MoveFileExW(temp,target.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH))throw std::system_error(GetLastError(),std::system_category(),"Replace save target");
    } catch(...) {if(file!=INVALID_HANDLE_VALUE)CloseHandle(file);DeleteFileW(temp);throw;}
#else
    std::string name=path.string()+".pmr-XXXXXX";std::vector<char> temp(name.begin(),name.end());temp.push_back(0);
    int fd=mkstemp(temp.data());if(fd<0)throw std::system_error(errno,std::generic_category(),"Create save temporary");
    try {
        std::size_t done=0;
        while(done<data.size()){auto n=::write(fd,data.data()+done,data.size()-done);if(n<0&&errno==EINTR)continue;if(n<=0)throw std::system_error(errno,std::generic_category(),"Save write");done+=std::size_t(n);}
        if(fsync(fd)!=0)throw std::system_error(errno,std::generic_category(),"Save flush");
        if(close(fd)!=0){fd=-1;throw std::system_error(errno,std::generic_category(),"Save close");}fd=-1;
        if(before_replace)before_replace();
        if(rename(temp.data(),path.c_str())!=0)throw std::system_error(errno,std::generic_category(),"Replace save target");
    }catch(...){if(fd>=0)close(fd);unlink(temp.data());throw;}
#endif
}
void storage_detail::write_new_checked(const std::filesystem::path& path,std::span<const std::uint8_t> bytes,const std::function<void()>& guard){
    const auto parent=path.has_parent_path()?path.parent_path():std::filesystem::path(".");
#ifdef _WIN32
    auto directory=batch_windows::directory(parent);const auto identity=batch_windows::state(directory.value);
    batch_windows::FileStage stage(parent,{});batch_windows::write(stage.file.value,bytes,{});
    const auto held=batch_windows::state(stage.file.value);if(guard)guard();
    if(!batch_windows::same_identity(identity,batch_windows::state(parent))||!batch_windows::same_generation(held,batch_windows::state(stage.path)))throw std::logic_error("Backup directory or stage changed before publication");
    if(!CreateHardLinkW(batch_windows::api_path(path).c_str(),batch_windows::api_path(stage.path).c_str(),nullptr)){const auto code=GetLastError();if(code==ERROR_FILE_EXISTS||code==ERROR_ALREADY_EXISTS)throw std::system_error(std::make_error_code(std::errc::file_exists),"Publish backup");batch_windows::error("Publish backup",code);}
    if(!batch_windows::same_identity(identity,batch_windows::state(parent))||!batch_windows::same_generation(held,batch_windows::state(path)))throw std::logic_error("Backup publication identity changed");
#else
    struct Descriptor{int fd{-1};~Descriptor(){if(fd>=0)::close(fd);}};
    Descriptor directory;directory.fd=::open(parent.c_str(),O_RDONLY|O_DIRECTORY|O_NOFOLLOW);if(directory.fd<0)throw std::system_error(errno,std::generic_category(),"Open backup directory");
    struct stat identity{};if(::fstat(directory.fd,&identity)!=0)throw std::system_error(errno,std::generic_category(),"Identify backup directory");
    const auto same=[](const struct stat& a,const struct stat& b){return a.st_dev==b.st_dev&&a.st_ino==b.st_ino;};
    const int duplicate=::dup(directory.fd);if(duplicate<0)throw std::system_error(errno,std::generic_category(),"Backup directory stream");DIR* entries=::fdopendir(duplicate);if(!entries){const auto error=errno;::close(duplicate);throw std::system_error(error,std::generic_category(),"Backup directory stream");}
    std::array<char,80> temporary{};Descriptor file;
    struct Cleanup{int directory;Descriptor& file;DIR* entries;const char* name;bool published{};
        ~Cleanup(){struct stat held{};if(file.fd>=0&&::fstat(file.fd,&held)==0){const auto remove=[&](const char* p){struct stat current{};if(::fstatat(directory,p,&current,AT_SYMLINK_NOFOLLOW)==0&&S_ISREG(current.st_mode)&&current.st_dev==held.st_dev&&current.st_ino==held.st_ino)::unlinkat(directory,p,0);};remove(name);if(!published){::rewinddir(entries);while(const auto* e=::readdir(entries)){if(std::strcmp(e->d_name,".")&&std::strcmp(e->d_name,".."))remove(e->d_name);}}}::closedir(entries);}
    }cleanup{directory.fd,file,entries,temporary.data(),false};
    for(int attempt=0;attempt<64;++attempt){std::array<unsigned,4> random{};::arc4random_buf(random.data(),sizeof(random));std::snprintf(temporary.data(),temporary.size(),".paintman-backup-%08x%08x%08x%08x",random[0],random[1],random[2],random[3]);file.fd=::openat(directory.fd,temporary.data(),O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW,0600);if(file.fd>=0)break;if(errno!=EEXIST)throw std::system_error(errno,std::generic_category(),"Create backup stage");}
    if(file.fd<0)throw std::runtime_error("Backup stage namespace exhausted");
    for(std::size_t done=0;done<bytes.size();){const auto n=::write(file.fd,bytes.data()+done,std::min<std::size_t>(bytes.size()-done,1<<20));if(n<0&&errno==EINTR)continue;if(n<=0)throw std::system_error(errno,std::generic_category(),"Write backup stage");done+=std::size_t(n);}
    if(::fsync(file.fd)!=0)throw std::system_error(errno,std::generic_category(),"Sync backup stage");
    struct stat held{};if(::fstat(file.fd,&held)!=0)throw std::system_error(errno,std::generic_category(),"Identify backup stage");if(guard)guard();
    struct stat named{},parent_now{};if(::lstat(parent.c_str(),&parent_now)!=0||!S_ISDIR(parent_now.st_mode)||!same(identity,parent_now)||::fstatat(directory.fd,temporary.data(),&named,AT_SYMLINK_NOFOLLOW)!=0||!S_ISREG(named.st_mode)||!same(held,named)||held.st_size<0||std::uint64_t(held.st_size)!=bytes.size()||named.st_size!=held.st_size)throw std::logic_error("Backup directory or stage changed before publication");
    const auto target=path.filename().string();if(::linkat(directory.fd,temporary.data(),directory.fd,target.c_str(),0)!=0)throw std::system_error(errno,std::generic_category(),"Publish new backup");
    struct stat published{};if(::lstat(parent.c_str(),&parent_now)!=0||!S_ISDIR(parent_now.st_mode)||!same(identity,parent_now)||::fstatat(directory.fd,target.c_str(),&published,AT_SYMLINK_NOFOLLOW)!=0||!S_ISREG(published.st_mode)||!same(held,published))throw std::logic_error("Backup publication identity changed");cleanup.published=true;
#endif
}
void write_file_atomic(const std::filesystem::path& path,std::span<const std::uint8_t> data){storage_detail::write_checked(path,data,{});}
}
