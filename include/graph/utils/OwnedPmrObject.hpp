#pragma once

#include <cstddef>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <utility>

namespace graph::utils {
// Opt-in allocation for private, normally aligned polymorphic objects returned
// through ordinary unique_ptr interfaces. The prefix survives object teardown,
// retaining the allocation domain until the final upstream deallocation.
// Without an owner, ordinary new/delete storage has permanent upstream lifetime.
class OwnedPmrObject {
public:
    static void *operator new(size_t bytes, std::shared_ptr<std::pmr::memory_resource> owner) {
        if (bytes > (std::numeric_limits<size_t>::max)() - sizeof(Header)) throw std::bad_alloc();
        auto *resource = owner ? owner.get() : std::pmr::new_delete_resource();
        const size_t total = sizeof(Header) + bytes;
        void *storage = resource->allocate(total, alignof(Header));
        auto *header = ::new (storage) Header{std::move(owner), resource, total};
        return header + 1;
    }
    static void operator delete(void *pointer) noexcept { release(pointer); }
    // Invoked if the derived constructor throws after allocation succeeds.
    static void operator delete(void *pointer, std::shared_ptr<std::pmr::memory_resource>) noexcept { release(pointer); }
protected:
    OwnedPmrObject() = default;
    ~OwnedPmrObject() = default;
private:
    struct alignas(std::max_align_t) Header {
        std::shared_ptr<std::pmr::memory_resource> owner;
        std::pmr::memory_resource *resource;
        size_t total;
    };
    static void release(void *pointer) noexcept {
        if (!pointer) return;
        auto *header = static_cast<Header *>(pointer) - 1;
        auto owner = std::move(header->owner);
        auto *resource = header->resource;
        const size_t total = header->total;
        header->~Header();
        resource->deallocate(header, total, alignof(Header));
    }
};
} // namespace graph::utils
