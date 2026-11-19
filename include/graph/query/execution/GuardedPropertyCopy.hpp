#pragma once
#include "graph/query/execution/ExecutionBindingContainers.hpp"
#include "graph/core/GuardedPropertyCopy.hpp"
#include "graph/core/PropertyHeapRequestBytes.hpp"
#include "graph/core/PropertyDecodeAllocationScope.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/utils/StandardContainerHeapBytes.hpp"
namespace graph::query::execution::detail {
using graph::property_copy::copyPropertyValueImpl;
inline auto *guardedProcedureCopyScope() noexcept {
    auto *scope = property_decode::AllocationScope::current();
    return scope && scope->guardedCopies() ? scope : nullptr;
}
template<class Checkpoint>
std::string copyPropertyStringGuarded(const std::string &source, Checkpoint &&checkpoint) {
    if (auto *scope = guardedProcedureCopyScope()) scope->admit(utils::detail::addStandardHeapBytes(
        utils::detail::standardFreshStringHeapUpper(source.size()), 2U * utils::detail::standardContainerProxyBytes()));
    return graph::property_copy::copyPropertyStringGuarded(source, checkpoint);
}
template<class Checkpoint>
std::vector<std::string> copyPropertyStringsGuarded(const std::vector<std::string> &source, Checkpoint checkpoint) {
    if (auto *scope = guardedProcedureCopyScope()) {
        size_t bytes = property_heap::detail::copiedVectorBytes<std::string>(source.size());
        bytes = utils::detail::addStandardHeapBytes(bytes, 2U * utils::detail::standardContainerProxyBytes());
        for (const auto &item : source) bytes = utils::detail::addStandardHeapBytes(bytes,
            utils::detail::addStandardHeapBytes(utils::detail::standardFreshStringHeapUpper(item.size()),
                2U * utils::detail::standardContainerProxyBytes()));
        scope->admit(bytes);
    }
    return graph::property_copy::copyPropertyStringsGuarded(source, checkpoint);
}
template<class Checkpoint>
PropertyValue copyPropertyValueGuarded(const PropertyValue &source, Checkpoint checkpoint, bool preserveMapLayout = false) {
    if (auto *scope = guardedProcedureCopyScope()) {
        const std::function<void()> check = std::ref(checkpoint);
        size_t bytes = storage::CanonicalPropertyValueCodec::guardedCopyHeapUpper(source, check);
        if (preserveMapLayout) bytes = utils::detail::addStandardHeapBytes(bytes, property_heap::copyRequestUpper(source));
        scope->admit(bytes);
    }
    return graph::property_copy::copyPropertyValueGuarded(source, checkpoint, preserveMapLayout);
}
template<class Checkpoint>
PropertyValue::MapType copyPropertyMapGuarded(const PropertyValue::MapType &source, Checkpoint checkpoint) {
    if (auto *scope = guardedProcedureCopyScope()) {
        size_t bytes = utils::detail::standardFreshUnorderedMapHeapUpper<std::string, PropertyValue>(
            graph::property_copy::freshMapReserveCount(source.size()));
        const std::function<void()> check = std::ref(checkpoint);
        for (const auto &[key, value] : source) {
            bytes = utils::detail::addStandardHeapBytes(bytes, utils::detail::standardFreshStringHeapUpper(key.size()));
            bytes = utils::detail::addStandardHeapBytes(bytes, storage::CanonicalPropertyValueCodec::guardedCopyHeapUpper(value, check));
        }
        scope->admit(bytes);
    }
    return graph::property_copy::copyPropertyMapGuarded(source, checkpoint);
}
} // namespace graph::query::execution::detail

namespace graph::query::execution::detail {
// Internal binding maps use compact node storage; public PropertyValue maps
// retain their wire/SDK representation. Conversion is explicit and guarded.
template<class Map,class Checkpoint>
    requires isExecutionBindingTree<Map>
PropertyValue::MapType copyPropertyMapGuarded(const Map &source,Checkpoint checkpoint) {
    checkpoint();
    if(auto *scope=guardedProcedureCopyScope()) {
        size_t bytes=utils::detail::standardFreshUnorderedMapHeapUpper<std::string,PropertyValue>(
            graph::property_copy::freshMapReserveCount(source.size()));
        const std::function<void()> check=std::ref(checkpoint);
        for(const auto &[key,value]:source) {
            bytes=utils::detail::addStandardHeapBytes(bytes,utils::detail::standardFreshStringHeapUpper(key.size()));
            bytes=utils::detail::addStandardHeapBytes(bytes,
                storage::CanonicalPropertyValueCodec::guardedCopyHeapUpper(value,check));
        }
        scope->admit(bytes);
    }
    PropertyValue::MapType result;
    result.reserve(graph::property_copy::freshMapReserveCount(source.size()));
    for(const auto &[key,value]:source) {
        checkpoint();
        result.emplace(graph::property_copy::copyPropertyStringGuarded(key,checkpoint),
            graph::property_copy::copyPropertyValueGuarded(value,checkpoint));
    }
    checkpoint();return result;
}

template<class Map,class Checkpoint>
ExecutionBindingMap<std::string,PropertyValue> copyBindingPropertyMapGuarded(const Map &source,Checkpoint checkpoint) {
    using Result = ExecutionBindingMap<std::string,PropertyValue>;
    checkpoint();
    if(auto *scope=guardedProcedureCopyScope()) {
        if(source.size() > (std::numeric_limits<size_t>::max)()/executionBindingNodeBytes<Result>())
            throw std::overflow_error("copied binding metadata overflowed");
        size_t bytes=source.size()*executionBindingNodeBytes<Result>();
        const std::function<void()> check=std::ref(checkpoint);
        for(const auto &[key,value]:source) {
            bytes=utils::detail::addStandardHeapBytes(bytes,utils::detail::standardFreshStringHeapUpper(key.size()));
            bytes=utils::detail::addStandardHeapBytes(bytes,
                storage::CanonicalPropertyValueCodec::guardedCopyHeapUpper(value,check));
        }
        scope->admit(bytes);
    }
    Result result;
    for(const auto &[key,value]:source) {
        checkpoint();
        result.emplace(graph::property_copy::copyPropertyStringGuarded(key,checkpoint),
            graph::property_copy::copyPropertyValueGuarded(value,checkpoint));
    }
    checkpoint();return result;
}
} // namespace graph::query::execution::detail
