// The native job caps total private commit. Track C++ allocations separately
// because Wine versions vary in how they enforce process-memory job limits.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <limits>
#include <new>

namespace {
struct Allocation { void* base; size_t charged; };
constinit std::atomic<size_t> used{};
constinit std::atomic<size_t> budget{SIZE_MAX};
bool reserve(size_t charged) noexcept{
    auto total=used.load(std::memory_order_relaxed);
    for(;;){
        const size_t limit=budget.load(std::memory_order_relaxed);
        if(total>limit||charged>limit-total)return false;
        if(used.compare_exchange_weak(total,total+charged,std::memory_order_relaxed))return true;
    }
}
void* allocate(size_t bytes,size_t alignment){
    alignment=alignment<alignof(Allocation)?alignof(Allocation):alignment;
    if(bytes>SIZE_MAX-alignment-sizeof(Allocation))throw std::bad_alloc();
    const size_t charged=(bytes?bytes:1)+alignment+sizeof(Allocation);
    if(!reserve(charged))throw std::bad_alloc();
    void* base=std::malloc(charged);
    if(!base){used.fetch_sub(charged);throw std::bad_alloc();}
    const auto aligned=(reinterpret_cast<uintptr_t>(base)+sizeof(Allocation)+alignment-1)&~uintptr_t(alignment-1);
    auto* record=reinterpret_cast<Allocation*>(aligned)-1;
    *record={base,charged};return reinterpret_cast<void*>(aligned);
}
void release(void* p) noexcept{
    if(p){auto record=*(static_cast<Allocation*>(p)-1);std::free(record.base);used.fetch_sub(record.charged,std::memory_order_relaxed);}
}
}
extern "C" int retas_reserve_codec(size_t n){return reserve(n);}
extern "C" void retas_release_codec(size_t n){used.fetch_sub(n,std::memory_order_relaxed);}
void limit_renderer_memory(unsigned mib){
    const size_t maximum=size_t(mib)<<20;
    PROCESS_MEMORY_COUNTERS_EX stats{};stats.cb=sizeof(stats);
    size_t external=32u<<20;
    if(GetProcessMemoryInfo(GetCurrentProcess(),reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&stats),sizeof(stats))&&stats.PrivateUsage>used.load())
        external+=stats.PrivateUsage-used.load();
    budget.store(maximum>external?maximum-external:0);
    HANDLE job=CreateJobObjectW(nullptr,nullptr);
    if(job){
        JOBOBJECT_EXTENDED_LIMIT_INFORMATION info{};
        info.BasicLimitInformation.LimitFlags=JOB_OBJECT_LIMIT_PROCESS_MEMORY;
        info.ProcessMemoryLimit=maximum;
        if(!SetInformationJobObject(job,JobObjectExtendedLimitInformation,&info,sizeof(info))||
           !AssignProcessToJobObject(job,GetCurrentProcess()))CloseHandle(job);
        // On success the kernel owns this limit for our entire process lifetime.
    }
}
void* operator new(size_t n){return allocate(n,__STDCPP_DEFAULT_NEW_ALIGNMENT__);}
void* operator new[](size_t n){return ::operator new(n);}
void* operator new(size_t n,std::align_val_t a){return allocate(n,size_t(a));}
void* operator new[](size_t n,std::align_val_t a){return allocate(n,size_t(a));}
void operator delete(void* p) noexcept{release(p);}
void operator delete[](void* p) noexcept{release(p);}
void operator delete(void* p,size_t) noexcept{release(p);}
void operator delete[](void* p,size_t) noexcept{release(p);}
void operator delete(void* p,std::align_val_t) noexcept{release(p);}
void operator delete[](void* p,std::align_val_t) noexcept{release(p);}
void operator delete(void* p,size_t,std::align_val_t) noexcept{release(p);}
void operator delete[](void* p,size_t,std::align_val_t) noexcept{release(p);}
