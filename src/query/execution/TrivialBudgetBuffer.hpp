#pragma once

#include <cstddef>
#include <cstring>
#include <exception>
#include <iterator>
#include <limits>
#include <memory>
#include <memory_resource>
#include <span>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace graph::query::execution::detail {
// Internal flat metadata/cell storage. Unlike debug STL vectors, an empty
// buffer has no allocator proxy and release never allocates. Only trivial
// implicit-lifetime elements are supported; owning payloads use their own
// containers. The resource outlives this buffer and admits old/new overlap.
template<class T>
class TrivialBudgetBuffer final {
    static_assert(std::is_trivially_copyable_v<T> && std::is_trivially_destructible_v<T>);
    static_assert(std::is_nothrow_copy_constructible_v<T> && std::is_nothrow_default_constructible_v<T>);
public:
    explicit TrivialBudgetBuffer(std::pmr::memory_resource *resource) : resource_(resource) {
        if (!resource) throw std::invalid_argument("flat buffer requires an allocation resource");
    }
    ~TrivialBudgetBuffer() { release(); }
    TrivialBudgetBuffer(const TrivialBudgetBuffer &) = delete;
    TrivialBudgetBuffer &operator=(const TrivialBudgetBuffer &) = delete;
    [[nodiscard]] size_t size() const noexcept { return size_; }
    [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
    [[nodiscard]] bool empty() const noexcept { return size_ == 0U; }
    [[nodiscard]] T *data() noexcept { return data_; }
    [[nodiscard]] const T *data() const noexcept { return data_; }
    T *begin() noexcept { return data_; }
    const T *begin() const noexcept { return data_; }
    T *end() noexcept { return size_ ? data_ + size_ : data_; }
    const T *end() const noexcept { return size_ ? data_ + size_ : data_; }
    T &at(size_t index) { if (index >= size_) throw std::out_of_range("flat buffer index"); return data_[index]; }
    const T &at(size_t index) const { if (index >= size_) throw std::out_of_range("flat buffer index"); return data_[index]; }
    T &operator[](size_t index) noexcept { return data_[index]; }
    const T &operator[](size_t index) const noexcept { return data_[index]; }
    operator std::span<T>() noexcept { return {data_, size_}; }
    operator std::span<const T>() const noexcept { return {data_, size_}; }
    void reserve(size_t requested) {
        if (requested <= capacity_) return;
        if (requested > (std::numeric_limits<size_t>::max)() / sizeof(T))
            throw std::length_error("flat buffer allocation overflowed");
        auto *next = static_cast<T *>(resource_->allocate(requested * sizeof(T), alignof(T)));
        // memcpy starts the lifetime of these implicit-lifetime objects (C++20).
        if (size_) std::memcpy(next, data_, size_ * sizeof(T));
        if (data_) resource_->deallocate(data_, capacity_ * sizeof(T), alignof(T));
        data_ = next; capacity_ = requested;
    }
    void push_back(T value) {
        if (size_ == capacity_) {
            if (size_ == (std::numeric_limits<size_t>::max)()) throw std::length_error("flat buffer size overflowed");
            reserve(size_ + 1U);
        }
        std::construct_at(data_ + size_, value); ++size_;
    }
    void resize(size_t requested) {
        reserve(requested);
        while (size_ < requested) { std::construct_at(data_ + size_, T{}); ++size_; }
        size_ = requested;
    }
    template<std::contiguous_iterator Iterator>
    void assign(Iterator first, Iterator last) {
        if (first == last) { clear(); return; }
        const auto distance = last - first;
        if (distance < 0) throw std::invalid_argument("flat buffer range is reversed");
        const size_t count = static_cast<size_t>(distance);
        reserve(count);
        if (count) std::memmove(data_, std::to_address(first), count * sizeof(T));
        size_ = count;
    }
    void clear() noexcept { size_ = 0U; }
    void release() noexcept {
        if (data_) resource_->deallocate(data_, capacity_ * sizeof(T), alignof(T));
        data_ = nullptr; size_ = capacity_ = 0U;
    }
    void swap(TrivialBudgetBuffer &other) noexcept {
        // All live callers exchange buffers from the same task-local resource.
        if (resource_ != other.resource_) std::terminate();
        std::swap(data_, other.data_); std::swap(size_, other.size_); std::swap(capacity_, other.capacity_);
    }
private:
    std::pmr::memory_resource *resource_;
    T *data_ = nullptr;
    size_t size_ = 0U, capacity_ = 0U;
};
}
