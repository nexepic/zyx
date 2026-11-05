#pragma once

#include <functional>
#include <limits>
#include <memory>
#include <map>
#include <set>
#include <stdexcept>
#include <type_traits>
#if defined(_MSVC_STL_VERSION)
#include <boost/container/map.hpp>
#include <boost/container/set.hpp>
#endif

namespace graph::query::execution::detail {

// Query-local bindings require stable value addresses: compiled expressions
// borrow them while other bindings are inserted. These node containers have
// an embedded empty tree header, avoiding an allocated hash sentinel, bucket
// array and iterator proxies for each unused row cache. Public result maps
// and PropertyValue maps retain their existing representation.
template<class Key, class Value, class Allocator>
using ExecutionBindingMapBase =
#if defined(_MSVC_STL_VERSION)
    boost::container::map<Key, Value, std::less<>, Allocator>;
#else
    std::map<Key, Value, std::less<>, std::conditional_t<std::is_void_v<Allocator>,
        std::allocator<std::pair<const Key, Value>>, Allocator>>;
#endif
template<class Key, class Allocator>
using ExecutionBindingSetBase =
#if defined(_MSVC_STL_VERSION)
    boost::container::set<Key, std::less<>, Allocator>;
#else
    std::set<Key, std::less<>, std::conditional_t<std::is_void_v<Allocator>, std::allocator<Key>, Allocator>>;
#endif

template<class Key, class Value, class Allocator = void>
class ExecutionBindingMap : public ExecutionBindingMapBase<Key, Value, Allocator> {
    using Base = ExecutionBindingMapBase<Key, Value, Allocator>;
public:
    using execution_binding_tree = void;
    using Base::Base;
    Value &at(const Key &key) {
        const auto found=this->find(key);
        if(found==this->end()) throw std::out_of_range("execution binding key not found");
        return found->second;
    }
    const Value &at(const Key &key) const {
        const auto found=this->find(key);
        if(found==this->end()) throw std::out_of_range("execution binding key not found");
        return found->second;
    }
    void reserve(size_t count) const {
        // A tree never relocates existing values. It has no bucket capacity to
        // preallocate; node admission happens before each actual insertion.
        if (count > this->max_size()) throw std::length_error("execution binding count overflowed");
    }
    friend bool operator==(const ExecutionBindingMap &a, const ExecutionBindingMap &b) {
        return static_cast<const Base &>(a) == static_cast<const Base &>(b);
    }
};

template<class Key, class Allocator = void>
class ExecutionBindingSet : public ExecutionBindingSetBase<Key, Allocator> {
    using Base = ExecutionBindingSetBase<Key, Allocator>;
public:
    using execution_binding_tree = void;
    using Base::Base;
    void reserve(size_t count) const {
        if (count > this->max_size()) throw std::length_error("execution binding count overflowed");
    }
    friend bool operator==(const ExecutionBindingSet &a, const ExecutionBindingSet &b) {
        return static_cast<const Base &>(a) == static_cast<const Base &>(b);
    }
};

template<class Container>
inline constexpr bool isExecutionBindingTree = requires { typename Container::execution_binding_tree; };

template<class Container>
[[nodiscard]] constexpr size_t executionBindingNodeBytes() {
    static_assert(isExecutionBindingTree<Container>);
#if defined(_MSVC_STL_VERSION)
    // The Boost allocator type exposes the complete allocated node. Boost is
    // used on MSVC because its standard tree allocates an empty sentinel and
    // debug proxy. String/payload heaps remain separate.
    using Node = typename std::allocator_traits<typename Container::stored_allocator_type>::value_type;
    static_assert(sizeof(Node) >= sizeof(typename Container::value_type));
    return sizeof(Node);
#elif defined(__GLIBCXX__) || defined(_LIBCPP_VERSION)
    // libstdc++/libc++ embed their empty header. Their red-black node has
    // three links plus color/padding; four pointers bound the header. Round
    // the value offset and complete node to both relevant alignments.
    using Entry = typename Container::value_type;
    constexpr size_t valueAlignment = alignof(Entry);
    constexpr size_t alignment = valueAlignment > alignof(void *) ? valueAlignment : alignof(void *);
    constexpr size_t valueOffset = (4U * sizeof(void *) + valueAlignment - 1U) / valueAlignment * valueAlignment;
    static_assert(sizeof(Entry) <= (std::numeric_limits<size_t>::max)() - valueOffset - alignment + 1U);
    return (valueOffset + sizeof(Entry) + alignment - 1U) / alignment * alignment;
#else
    static_assert(sizeof(Container) == 0U, "execution binding node accounting requires an audited STL");
#endif
}

template<class Container>
[[nodiscard]] size_t executionBindingStructuralBytes(const Container &container) {
    constexpr auto width = executionBindingNodeBytes<Container>();
    if (container.size() > (std::numeric_limits<size_t>::max)() / width)
        throw std::overflow_error("execution binding bytes overflowed");
    return container.size() * width;
}

template<class Container, class Predicate>
size_t eraseExecutionBindingsIf(Container &container, Predicate predicate) {
    size_t removed = 0U;
    for (auto entry = container.begin(); entry != container.end();) {
        if (predicate(*entry)) { entry = container.erase(entry); ++removed; }
        else ++entry;
    }
    return removed;
}

} // namespace graph::query::execution::detail
