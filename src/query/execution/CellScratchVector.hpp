#pragma once
#include <array>
#include <cstdint>
#include <optional>
#include <stdexcept>
#include <type_traits>
#include <vector>
#include "graph/utils/StandardContainerHeapBytes.hpp"

namespace graph::query::execution::detail {
// Task-local ordinal/key-view scratch. A single entry stays on the stack like
// the existing vector object itself. The materialized working set still counts
// that inline entry; only iterator proxies and heap backing are avoided.
// Callers admit workingSetUpper(count) before reserve(), then never grow beyond
// that reservation. Payloads/owners are deliberately excluded from this type.
template<class T> class CellScratchVector final {
    static_assert(std::is_trivially_copyable_v<T> && std::is_nothrow_default_constructible_v<T>);
public:
    CellScratchVector() = default;
    CellScratchVector(const CellScratchVector &) = delete;
    CellScratchVector &operator=(const CellScratchVector &) = delete;
    CellScratchVector(CellScratchVector &&) = delete;
    CellScratchVector &operator=(CellScratchVector &&) = delete;
    static size_t allocationUpper(size_t count) {
        if (count <= 1U) return 0U;
        if (count > SIZE_MAX / sizeof(T)) throw std::overflow_error("Property cell scratch overflowed");
        return utils::detail::addStandardHeapBytes(utils::detail::standardContainerProxyBytes(),
            utils::detail::standardAllocatorRequestBytes(count * sizeof(T), alignof(T)));
    }
    static size_t workingSetUpper(size_t count) {
        return count <= 1U ? count * sizeof(T) : allocationUpper(count);
    }
    void reserve(size_t count) {
        if (capacity_ != 0U || external_) throw std::logic_error("Property cell scratch is already reserved");
        if (count > 1U) {
            external_.emplace();
            try { external_->reserve(count); }
            catch (...) { external_.reset(); throw; }
        }
        capacity_ = count;
    }
    void clear() noexcept { if (external_) external_->clear(); else size_ = 0U; }
    void push_back(T value) {
        if (size() >= capacity_) throw std::length_error("Property cell scratch exceeded its admitted capacity");
        if (external_) {
            if (external_->size() == external_->capacity())
                throw std::length_error("Property cell scratch exceeded its admitted capacity");
            external_->push_back(value);
        } else {
            if (size_ != 0U) throw std::length_error("Property cell inline scratch exceeded one entry");
            inline_[size_++] = value;
        }
    }
    size_t size() const noexcept { return external_ ? external_->size() : size_; }
    bool empty() const noexcept { return size() == 0U; }
    size_t heapBytes() const {
        return external_ ? utils::detail::standardVectorHeapBufferBytes(*external_) : 0U;
    }
    T *data() noexcept { return external_ ? external_->data() : inline_.data(); }
    const T *data() const noexcept { return external_ ? external_->data() : inline_.data(); }
    T &operator[](size_t index) noexcept { return data()[index]; }
    const T &operator[](size_t index) const noexcept { return data()[index]; }
    T *begin() noexcept { return data(); }
    T *end() noexcept { return data() + size(); }
    const T *begin() const noexcept { return data(); }
    const T *end() const noexcept { return data() + size(); }
private:
    std::array<T, 1> inline_{};
    std::optional<std::vector<T>> external_;
    size_t size_ = 0U;
    size_t capacity_ = 0U;
};
}
