/**
 * @file PropertyHeapRequestBytes.hpp
 * @brief Bounds for standard-allocator PropertyValue copies and retained data.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <type_traits>
#include "graph/core/PropertyTypes.hpp"
#include "graph/utils/StandardContainerHeapBytes.hpp"
#include "graph/utils/StringBufferBytes.hpp"

namespace graph::property_heap {
// Borrowed immutable input for a list copy. Scalars are inline; string/value
// references must stay valid and unchanged through both admission and copying.
class CopySource final {
public:
    CopySource() = default;
    CopySource(bool value) : value_(value) {}
    template<std::integral T> requires (!std::same_as<T, bool>)
    CopySource(T value) : value_(static_cast<int64_t>(value)) {}
    template<std::floating_point T> CopySource(T value) : value_(static_cast<double>(value)) {}
    CopySource(const std::string &value) : value_(&value) {}
    CopySource(const PropertyValue &value) : value_(&value) {}
    CopySource(std::string &&) = delete;
    CopySource(PropertyValue &&) = delete;
    [[nodiscard]] size_t copyRequestUpper() const;
    void appendTo(std::vector<PropertyValue> &output) const {
        std::visit([&](const auto &value) {
            using Type = std::decay_t<decltype(value)>;
            if constexpr (std::is_pointer_v<Type>) output.emplace_back(*value);
            else output.emplace_back(value);
        }, value_);
    }
private:
    std::variant<std::monostate, bool, int64_t, double, const std::string *, const PropertyValue *> value_;
};
namespace detail {
template<class T>
size_t copiedVectorBytes(size_t count) {
    if (count > (std::numeric_limits<size_t>::max)()/sizeof(T))
        throw std::overflow_error("Property vector request overflowed");
    return utils::detail::addStandardHeapBytes(utils::detail::standardContainerProxyBytes(),
        utils::detail::standardAllocatorRequestBytes(count * sizeof(T), alignof(T)));
}
template<bool Copy>
size_t valueBytes(const PropertyValue &value, size_t depth) {
    if (depth > 64U) throw std::length_error("Property heap admission exceeds maximum nesting depth");
    using namespace utils::detail;
    const auto &variant = value.getVariant();
    if (const auto *text = std::get_if<std::string>(&variant))
        return Copy ? standardFreshStringHeapUpper(text->size()) : dynamicStringBufferBytes(*text);
    if (const auto *bytes = std::get_if<PropertyValue::Bytes>(&variant))
        return Copy ? copiedVectorBytes<std::byte>(bytes->size()) : standardVectorHeapBufferBytes(*bytes);
    size_t result = 0U;
    if (const auto *list = std::get_if<std::vector<PropertyValue>>(&variant)) {
        result = Copy ? copiedVectorBytes<PropertyValue>(list->size()) : standardVectorHeapBufferBytes(*list);
        for (const auto &child : *list) result = addStandardHeapBytes(result, valueBytes<Copy>(child, depth + 1U));
    } else if (const auto *map = std::get_if<PropertyValue::MapType>(&variant)) {
        // Standard map copies preserve visible bucket storage on the audited
        // C++20 STLs; payload strings/vectors normalize their copy capacities.
        result = standardUnorderedMapStructuralHeapBytes(*map);
        for (const auto &[key, child] : *map) {
            result = addStandardHeapBytes(result, Copy ? standardFreshStringHeapUpper(key.size()) : dynamicStringBufferBytes(key));
            result = addStandardHeapBytes(result, valueBytes<Copy>(child, depth + 1U));
        }
    }
    return result;
}
} // namespace detail
inline size_t copyRequestUpper(const PropertyValue &value) { return detail::valueBytes<true>(value, 0U); }
inline size_t retainedRequestUpper(const PropertyValue &value) { return detail::valueBytes<false>(value, 0U); }
inline size_t CopySource::copyRequestUpper() const {
    return std::visit([](const auto &value) -> size_t {
        using Type = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Type, const std::string *>)
            return utils::detail::standardFreshStringHeapUpper(value->size());
        else if constexpr (std::is_same_v<Type, const PropertyValue *>) return property_heap::copyRequestUpper(*value);
        else return 0U;
    }, value_);
}

} // namespace graph::property_heap
