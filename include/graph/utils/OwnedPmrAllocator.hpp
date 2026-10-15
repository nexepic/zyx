#pragma once

#include <algorithm>
#include <cstddef>
#include <limits>
#include <memory>
#include <memory_resource>
#include <new>
#include <type_traits>
#include <utility>
#include <vector>

namespace graph::utils {
// Thread-bound, explicitly scoped allocation ownership for codec construction
// and cache copies. Existing containers retain the allocator they captured.
class OwnedAllocationScope final {
public:
    explicit OwnedAllocationScope(std::shared_ptr<std::pmr::memory_resource> owner) noexcept
        : owner_(std::move(owner)), previous_(active_) { active_ = this; }
    ~OwnedAllocationScope() { active_ = previous_; }
    OwnedAllocationScope(const OwnedAllocationScope &) = delete;
    OwnedAllocationScope &operator=(const OwnedAllocationScope &) = delete;
    static bool active() noexcept { return active_ != nullptr; }
    static std::shared_ptr<std::pmr::memory_resource> owner() noexcept {
        return active_ ? active_->owner_ : std::shared_ptr<std::pmr::memory_resource>{};
    }
private:
    std::shared_ptr<std::pmr::memory_resource> owner_;
    OwnedAllocationScope *previous_;
    inline static thread_local OwnedAllocationScope *active_ = nullptr;
};

// Unlike polymorphic_allocator's borrowed pointer, this allocator keeps the
// allocation domain alive through escaped containers and shared control blocks.
template<class T> class OwnedPmrAllocator {
public:
    using value_type = T;
    using propagate_on_container_copy_assignment = std::true_type;
    using propagate_on_container_move_assignment = std::true_type;
    using propagate_on_container_swap = std::true_type;
    OwnedPmrAllocator() noexcept : OwnedPmrAllocator(OwnedAllocationScope::owner()) {}
    explicit OwnedPmrAllocator(std::shared_ptr<std::pmr::memory_resource> owner) noexcept
        : owner_(std::move(owner)), resource_(owner_ ? owner_.get() : std::pmr::get_default_resource()) {}
    OwnedPmrAllocator(const OwnedPmrAllocator &) noexcept = default;
    OwnedPmrAllocator &operator=(const OwnedPmrAllocator &) noexcept = default;
    // An emptied source container remains reusable after its destination dies.
    OwnedPmrAllocator(OwnedPmrAllocator &&other) noexcept : owner_(other.owner_), resource_(other.resource_) {}
    OwnedPmrAllocator &operator=(OwnedPmrAllocator &&other) noexcept {
        owner_ = other.owner_; resource_ = other.resource_; return *this;
    }
    template<class U> OwnedPmrAllocator(const OwnedPmrAllocator<U> &other) noexcept : owner_(other.owner()), resource_(other.resource()) {}
    [[nodiscard]] OwnedPmrAllocator select_on_container_copy_construction() const noexcept {
        return OwnedAllocationScope::active() ? OwnedPmrAllocator(OwnedAllocationScope::owner()) : *this;
    }
    [[nodiscard]] T *allocate(size_t count) {
        if (count > (std::numeric_limits<size_t>::max)() / sizeof(T)) throw std::bad_array_new_length();
        return static_cast<T *>(resource()->allocate(count * sizeof(T), alignof(T)));
    }
    void deallocate(T *pointer, size_t count) noexcept { resource()->deallocate(pointer, count * sizeof(T), alignof(T)); }
    [[nodiscard]] const std::shared_ptr<std::pmr::memory_resource> &owner() const noexcept { return owner_; }
    [[nodiscard]] std::pmr::memory_resource *resource() const noexcept {
        return resource_;
    }
    template<class U> bool operator==(const OwnedPmrAllocator<U> &other) const noexcept {
        return resource()->is_equal(*other.resource());
    }
private:
    std::shared_ptr<std::pmr::memory_resource> owner_;
    std::pmr::memory_resource *resource_;
};

// Preserve content comparisons with ordinary byte/vector consumers. Allocator
// template arguments make this overload visible through argument lookup.
template<class T, class OtherAllocator>
requires (!std::is_same_v<OtherAllocator, OwnedPmrAllocator<T>>)
bool operator==(const std::vector<T, OwnedPmrAllocator<T>> &left, const std::vector<T, OtherAllocator> &right) {
    return left.size() == right.size() && std::equal(left.begin(), left.end(), right.begin());
}
} // namespace graph::utils
