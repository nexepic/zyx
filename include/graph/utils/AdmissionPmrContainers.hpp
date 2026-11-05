#pragma once

#include <functional>
#include <map>
#include <memory_resource>
#include <set>
#include <stdexcept>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#if defined(_MSVC_STL_VERSION)
#include <boost/container/map.hpp>
#include <boost/container/set.hpp>
#include <boost/unordered_map.hpp>
#include <boost/unordered_set.hpp>
#if _ITERATOR_DEBUG_LEVEL > 0
#include <boost/container/vector.hpp>
#endif
#endif

namespace graph::utils {

// Boost.Unordered instantiates an allocator swap even for non-propagating
// PMR allocators. Keep PMR's resource identity/construct behavior while making
// that operation well-formed; container swap still requires equal resources.
template<class T>
class AdmissionHashAllocator {
public:
    using value_type = T;
    using is_always_equal = std::false_type;
    using propagate_on_container_swap = std::false_type;
    AdmissionHashAllocator() noexcept : memory_(std::pmr::get_default_resource()) {}
    AdmissionHashAllocator(std::pmr::memory_resource *memory) noexcept : memory_(memory) {}
    template<class U> AdmissionHashAllocator(const AdmissionHashAllocator<U> &other) noexcept : memory_(other.resource()) {}
    template<class U> struct rebind { using other = AdmissionHashAllocator<U>; };
    [[nodiscard]] T *allocate(size_t count) { return std::pmr::polymorphic_allocator<T>(memory_).allocate(count); }
    void deallocate(T *pointer,size_t count) { std::pmr::polymorphic_allocator<T>(memory_).deallocate(pointer,count); }
    template<class U,class... Args> void construct(U *pointer,Args &&...args) {
        std::pmr::polymorphic_allocator<T>(memory_).construct(pointer,std::forward<Args>(args)...);
    }
    template<class U> operator std::pmr::polymorphic_allocator<U>() const noexcept { return {memory_}; }
    [[nodiscard]] std::pmr::memory_resource *resource() const noexcept { return memory_; }
    [[nodiscard]] AdmissionHashAllocator select_on_container_copy_construction() const noexcept { return {}; }
    template<class U> bool operator==(const AdmissionHashAllocator<U> &other) const noexcept {
        return *memory_==*other.resource();
    }
    friend void swap(AdmissionHashAllocator &left,AdmissionHashAllocator &right) noexcept {
        std::swap(left.memory_,right.memory_);
    }
private:
    std::pmr::memory_resource *memory_;
};

// Internal containers for a resource that can reject any allocation. MSVC's
// debug vector allocates its iterator proxy in a noexcept constructor; MSVC
// hash/tree containers additionally allocate empty sentinels. These adapters
// keep construction empty and propagate admission failures during growth.
// Other STLs and release MSVC vectors retain their existing implementation.
#if defined(_MSVC_STL_VERSION) && _ITERATOR_DEBUG_LEVEL > 0
template<class T>
class AdmissionPmrVector : public boost::container::vector<T, std::pmr::polymorphic_allocator<T>> {
    using Base = boost::container::vector<T, std::pmr::polymorphic_allocator<T>>;
public:
    using Base::Base;
    using Base::operator=;
    // Keep the existing execution contract's standard exception category.
    // Boost.Container otherwise throws its own unrelated out_of_range type.
    T &at(typename Base::size_type index) {
        if(index>=this->size()) throw std::out_of_range("admitted vector index out of range");
        return (*this)[index];
    }
    const T &at(typename Base::size_type index) const {
        if(index>=this->size()) throw std::out_of_range("admitted vector index out of range");
        return (*this)[index];
    }
};
#else
template<class T>
using AdmissionPmrVector = std::pmr::vector<T>;
#endif

template<class Key, class Value, class Compare = std::less<Key>>
using AdmissionPmrMap =
#if defined(_MSVC_STL_VERSION)
    boost::container::map<Key, Value, Compare, std::pmr::polymorphic_allocator<std::pair<const Key, Value>>>;
#else
    std::pmr::map<Key, Value, Compare>;
#endif

template<class Key, class Compare = std::less<Key>>
using AdmissionPmrSet =
#if defined(_MSVC_STL_VERSION)
    boost::container::set<Key, Compare, std::pmr::polymorphic_allocator<Key>>;
#else
    std::pmr::set<Key, Compare>;
#endif

template<class Key, class Value, class Hash = std::hash<Key>, class Equal = std::equal_to<Key>>
using AdmissionPmrUnorderedMap =
#if defined(_MSVC_STL_VERSION)
    boost::unordered_map<Key, Value, Hash, Equal, AdmissionHashAllocator<std::pair<const Key, Value>>>;
#else
    std::pmr::unordered_map<Key, Value, Hash, Equal>;
#endif

template<class Key, class Hash = std::hash<Key>, class Equal = std::equal_to<Key>>
using AdmissionPmrUnorderedSet =
#if defined(_MSVC_STL_VERSION)
    boost::unordered_set<Key, Hash, Equal, AdmissionHashAllocator<Key>>;
#else
    std::pmr::unordered_set<Key, Hash, Equal>;
#endif

template<class Key, class Value, class Hash = std::hash<Key>, class Equal = std::equal_to<Key>>
using AdmissionPmrUnorderedMultiMap =
#if defined(_MSVC_STL_VERSION)
    boost::unordered_multimap<Key, Value, Hash, Equal, AdmissionHashAllocator<std::pair<const Key, Value>>>;
#else
    std::pmr::unordered_multimap<Key, Value, Hash, Equal>;
#endif

[[nodiscard]] inline std::pmr::string makeEmptyAdmissionPmrString(std::pmr::memory_resource *resource) {
    if (!resource) throw std::invalid_argument("admitted string requires an allocation resource");
    // Use the fallible pointer/count constructor, not the allocator-only
    // noexcept constructor. MSVC debug strings still have a real proxy fee;
    // a rejecting resource must be able to return that failure to the caller.
    return std::pmr::string("", size_t{0U}, resource);
}

} // namespace graph::utils
