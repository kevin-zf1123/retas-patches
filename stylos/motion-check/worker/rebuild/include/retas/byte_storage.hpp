#pragma once
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <new>
#include <type_traits>
#include <utility>
namespace retas {
struct ByteStoragePolicy {
    bool enabled{};
    std::uint64_t resident_limit{0x7fffffffu};
    std::size_t minimum_allocation{8192};
    std::filesystem::path directory;
};
struct ByteStorageUsage {std::uint64_t resident{},backed{},allocations{};};
// Configuration affects future allocations. Existing buffers retain their
// backing until their allocator releases them. The directory must exist.
void configure_byte_storage(ByteStoragePolicy);
ByteStorageUsage byte_storage_usage();
std::pair<std::uint64_t,std::uint64_t> physical_memory_status();
std::uint64_t native_vm_memory_budget(std::int32_t mode,std::int32_t percent,
    std::uint64_t total,std::uint64_t available,std::uint32_t automatic_percent=50);
namespace byte_storage_detail {
void* allocate(std::size_t);
void deallocate(void*) noexcept;
}
// Stateless allocator: moves/swaps preserve vector's no-throw guarantees.
// Small buffers and unconfigured embedding applications use the normal heap.
template<class T>struct ByteAllocator {
    using value_type=T;
    using is_always_equal=std::true_type;
    using propagate_on_container_move_assignment=std::true_type;
    using propagate_on_container_swap=std::true_type;
    ByteAllocator() noexcept=default;
    template<class U>ByteAllocator(const ByteAllocator<U>&) noexcept{}
    T* allocate(std::size_t count){
        if(count>std::numeric_limits<std::size_t>::max()/sizeof(T))throw std::bad_array_new_length();
        static_assert(alignof(T)<=alignof(std::max_align_t));auto* storage=byte_storage_detail::allocate(count*sizeof(T));
        // A byte-array lifetime starts implicit objects in mapped storage too.
        return reinterpret_cast<T*>(::new(storage) std::byte[count*sizeof(T)]);
    }
    void deallocate(T* pointer,std::size_t) noexcept{byte_storage_detail::deallocate(pointer);}
    template<class U>bool operator==(const ByteAllocator<U>&) const noexcept{return true;}
};
}
