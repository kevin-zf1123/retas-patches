#include "retas/byte_storage.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <memory>
#include <cstdlib>
#include <system_error>
#include <unordered_map>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <unistd.h>
#ifdef __APPLE__
#include <sys/sysctl.h>
#include <mach/mach.h>
#endif
#endif

namespace retas {
namespace {
template<class T> struct InventoryAllocator {
    using value_type=T;
    InventoryAllocator()=default;
    template<class U> InventoryAllocator(const InventoryAllocator<U>&) noexcept{}
    T* allocate(std::size_t n){if(n>SIZE_MAX/sizeof(T))throw std::bad_array_new_length();if(auto* p=std::malloc(n*sizeof(T)))return static_cast<T*>(p);throw std::bad_alloc();}
    void deallocate(T* p,std::size_t) noexcept{std::free(p);}
    template<class U> bool operator==(const InventoryAllocator<U>&)const noexcept{return true;}
};
class BackingAllocationFailure final : public std::bad_alloc {
public: const char* what() const noexcept override{return "Cannot allocate VM backing storage";}
};
struct Allocation {std::size_t bytes;bool backed;};
struct Pool {
    std::mutex mutex;
    std::shared_ptr<const ByteStoragePolicy> policy;
    ByteStorageUsage usage;
    std::atomic<bool> enabled{};
    std::atomic<std::size_t> tracked{};
    std::unordered_map<void*,Allocation,std::hash<void*>,std::equal_to<void*>,InventoryAllocator<std::pair<void* const,Allocation>>> allocations;
};
// Buffers can be owned by application statics destroyed after the UI runtime.
// Keep this small process inventory alive until the operating system exits.
Pool& pool(){static auto* state=new Pool;return *state;}
#ifdef _WIN32
void unmap(void* memory,std::size_t) noexcept{UnmapViewOfFile(memory);}
void* mapped_buffer(const std::filesystem::path& directory,std::size_t bytes){
    static std::atomic<std::uint64_t> serial{};HANDLE file=INVALID_HANDLE_VALUE;
    for(int tries=0;tries<100;++tries){const auto path=directory/(L"retas-vm-"+std::to_wstring(GetCurrentProcessId())+L"-"+std::to_wstring(GetTickCount64())+L"-"+std::to_wstring(serial++)+L".tmp");file=CreateFileW(path.c_str(),GENERIC_READ|GENERIC_WRITE,FILE_SHARE_READ|FILE_SHARE_WRITE|FILE_SHARE_DELETE,nullptr,CREATE_NEW,FILE_ATTRIBUTE_TEMPORARY|FILE_FLAG_DELETE_ON_CLOSE,nullptr);if(file!=INVALID_HANDLE_VALUE)break;if(GetLastError()!=ERROR_FILE_EXISTS)throw std::system_error(int(GetLastError()),std::system_category(),"Create preference VM backing");}
    if(file==INVALID_HANDLE_VALUE)throw std::runtime_error("Cannot create VM backing file");HANDLE mapping=nullptr;void* memory=nullptr;
    try{std::array<unsigned char,65536> zero{};for(std::size_t done=0;done<bytes;){DWORD written{};const auto amount=DWORD(std::min(bytes-done,zero.size()));if(!WriteFile(file,zero.data(),amount,&written,nullptr)||!written)throw std::system_error(int(GetLastError()),std::system_category(),"Reserve VM backing");done+=written;}
        mapping=CreateFileMappingW(file,nullptr,PAGE_READWRITE,DWORD(std::uint64_t(bytes)>>32),DWORD(bytes),nullptr);if(!mapping)throw std::system_error(int(GetLastError()),std::system_category(),"Map VM backing");memory=MapViewOfFile(mapping,FILE_MAP_ALL_ACCESS,0,0,bytes);if(!memory)throw std::system_error(int(GetLastError()),std::system_category(),"View VM backing");CloseHandle(mapping);CloseHandle(file);return memory;
    }catch(...){if(memory)UnmapViewOfFile(memory);if(mapping)CloseHandle(mapping);CloseHandle(file);throw;}
}
#else
void unmap(void* memory,std::size_t bytes) noexcept{munmap(memory,bytes);}
void* mapped_buffer(const std::filesystem::path& directory,std::size_t bytes){
    struct rlimit limit{};if(getrlimit(RLIMIT_FSIZE,&limit)!=0)throw std::system_error(errno,std::generic_category(),"Read VM file size limit");if(limit.rlim_cur!=RLIM_INFINITY&&bytes>limit.rlim_cur)throw std::length_error("VM backing exceeds the process file size limit");
    auto name=(directory/"retas-vm-XXXXXX").string();const int fd=mkstemp(name.data());if(fd<0)throw std::system_error(errno,std::generic_category(),"Create preference VM backing");
    // Unlink before writing: only this descriptor/mapping can reach the data.
    if(unlink(name.c_str())!=0){const int error=errno;close(fd);throw std::system_error(error,std::generic_category(),"Unlink VM backing");}
    fcntl(fd,F_SETFD,FD_CLOEXEC);
    void* memory=MAP_FAILED;
    try{
        // Materialize the file before mmap so ENOSPC is an allocation error,
        // rather than a later sparse-file page fault during a raster mutation.
        std::array<unsigned char,65536> zero{};
        for(std::size_t done=0;done<bytes;){const auto count=write(fd,zero.data(),std::min(bytes-done,zero.size()));if(count<0&&errno==EINTR)continue;if(count<=0)throw std::system_error(count<0?errno:EIO,std::generic_category(),"Reserve VM backing");done+=std::size_t(count);}
        memory=mmap(nullptr,bytes,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);if(memory==MAP_FAILED)throw std::system_error(errno,std::generic_category(),"Map VM backing");close(fd);return memory;
    }catch(...){if(memory!=MAP_FAILED)munmap(memory,bytes);close(fd);throw;}
}
#endif
}
void configure_byte_storage(ByteStoragePolicy policy){
    if(policy.enabled){if(policy.minimum_allocation==0)throw std::invalid_argument("VM minimum allocation must be positive");if(policy.directory.empty())policy.directory=std::filesystem::temp_directory_path();if(!std::filesystem::is_directory(policy.directory))throw std::invalid_argument("VM backing directory must exist");}
    auto replacement=std::make_shared<const ByteStoragePolicy>(std::move(policy));
    auto& state=pool();{std::lock_guard lock(state.mutex);state.policy.swap(replacement);state.enabled.store(state.policy->enabled,std::memory_order_release);}
}
ByteStorageUsage byte_storage_usage(){auto& state=pool();std::lock_guard lock(state.mutex);return state.usage;}
std::uint64_t native_vm_memory_budget(std::int32_t mode,std::int32_t percent,std::uint64_t total,std::uint64_t available,std::uint32_t automatic_percent){
    constexpr std::uint64_t cap=0x7fffffff;const auto scaled=[&](std::uint64_t value,std::uint32_t ratio,bool whole){if(!ratio)return std::uint64_t(0);const auto units=value/100;if(units>cap/ratio)return cap;const auto amount=units*ratio+(whole?0:(value%100)*ratio/100);return std::min(cap,amount);};
    if(mode==0)return std::max(scaled(total,std::min(automatic_percent,100u),false),scaled(available,std::min(automatic_percent,100u),false));
    if(mode==1)return scaled(total,std::uint32_t(std::clamp(percent,20,80)),true);return 0;
}
std::pair<std::uint64_t,std::uint64_t> physical_memory_status(){
#ifdef _WIN32
    MEMORYSTATUSEX status{};status.dwLength=sizeof(status);if(!GlobalMemoryStatusEx(&status))throw std::system_error(int(GetLastError()),std::system_category(),"Read physical memory");return {status.ullTotalPhys,status.ullAvailPhys};
#elif defined(__APPLE__)
    std::uint64_t total{};std::size_t size=sizeof(total);if(sysctlbyname("hw.memsize",&total,&size,nullptr,0)!=0)throw std::system_error(errno,std::generic_category(),"Read physical memory");vm_statistics64_data_t stats{};mach_msg_type_number_t count=HOST_VM_INFO64_COUNT;vm_size_t page{};const auto host=mach_host_self();const auto page_result=host_page_size(host,&page);const auto result=host_statistics64(host,HOST_VM_INFO64,reinterpret_cast<host_info64_t>(&stats),&count);mach_port_deallocate(mach_task_self(),host);if(result!=KERN_SUCCESS||page_result!=KERN_SUCCESS)throw std::runtime_error("Cannot read available memory");return {total,(std::uint64_t(stats.free_count)+stats.inactive_count)*page};
#else
    const auto page=sysconf(_SC_PAGESIZE),total=sysconf(_SC_PHYS_PAGES),available=sysconf(_SC_AVPHYS_PAGES);if(page<=0||total<=0||available<0)throw std::runtime_error("Cannot read physical memory");return {std::uint64_t(total)*page,std::uint64_t(available)*page};
#endif
}
namespace byte_storage_detail {
void* allocate(std::size_t bytes){
    auto& state=pool();if(!state.enabled.load(std::memory_order_acquire))return ::operator new(bytes);
    std::shared_ptr<const ByteStoragePolicy> policy;bool resident{},track{};
    {std::lock_guard lock(state.mutex);policy=state.policy;track=policy&&policy->enabled&&bytes>=policy->minimum_allocation;if(track){resident=state.usage.resident<=policy->resident_limit&&bytes<=policy->resident_limit-state.usage.resident;if(resident)state.usage.resident+=bytes;}}
    if(!track)return ::operator new(bytes);
    void* memory=nullptr;
    try{if(resident)memory=::operator new(bytes);else try{memory=mapped_buffer(policy->directory,bytes);}catch(const std::length_error&){throw;}catch(const std::bad_alloc&){throw;}catch(...){throw BackingAllocationFailure();}std::lock_guard lock(state.mutex);state.allocations.emplace(memory,Allocation{bytes,!resident});if(!resident)state.usage.backed+=bytes;++state.usage.allocations;state.tracked.fetch_add(1,std::memory_order_release);return memory;}
    catch(...){if(memory){if(resident)::operator delete(memory);else unmap(memory,bytes);}if(resident){std::lock_guard lock(state.mutex);state.usage.resident-=bytes;}throw;}
}
void deallocate(void* memory) noexcept{
    if(!memory)return;auto& state=pool();if(!state.tracked.load(std::memory_order_acquire)){::operator delete(memory);return;}
    Allocation allocation{};bool found{};
    {std::lock_guard lock(state.mutex);const auto it=state.allocations.find(memory);if(it!=state.allocations.end()){allocation=it->second;found=true;state.allocations.erase(it);if(allocation.backed)state.usage.backed-=allocation.bytes;else state.usage.resident-=allocation.bytes;--state.usage.allocations;state.tracked.fetch_sub(1,std::memory_order_release);}}
    if(found&&allocation.backed)unmap(memory,allocation.bytes);else ::operator delete(memory);
}
}
}
