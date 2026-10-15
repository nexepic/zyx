/**
 * @file MutationHeapBytes.hpp
 * @brief Allocation-request bounds for private logical mutation copies.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <algorithm>
#include <functional>
#include <span>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/query/execution/RetainedPropertyBytes.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"

namespace graph::storage::detail {
// Fresh copies normalize standard-library table storage before private adoption.
// In particular MSVC hash copies use the visible bucket vector size, discarding
// legacy hidden bucket capacity. Requests remain separate from malloc/RSS costs.
inline size_t writeStringCopyHeapUpper(const std::string &text) {
    using namespace query::execution::detail;
    const size_t proxy = standardContainerProxyBytes();
#if defined(_MSVC_STL_VERSION)
    // xstring::_Construct uses _Calculate_growth(count, 15, max_size()) for
    // char strings; non-SSO copy capacity rounds count up with mask 15.
    if (text.size() <= 15U) return proxy;
    const size_t chars = addPropertyBufferBytes(text.size() | size_t{15U}, 1U);
    return addPropertyBufferBytes(proxy, standardAllocatorRequestBytes(chars, alignof(char)));
#elif defined(__GLIBCXX__) && _GLIBCXX_USE_CXX11_ABI
    // basic_string::_M_construct(forward_iterator_tag) copies exactly length
    // chars outside its 15-char local buffer; source reserve capacity is ignored.
    return text.size() <= 15U ? proxy : addPropertyBufferBytes(proxy,
        standardAllocatorRequestBytes(addPropertyBufferBytes(text.size(), 1U), alignof(char)));
#else
    // Preserve the observed-capacity upper bound for other supported STL builds.
    return dynamicStringBufferBytes(text);
#endif
}

template<class T>
inline size_t writeVectorCopyHeapBytes(size_t count) {
    using namespace query::execution::detail;
    // The supported C++20 libstdc++/libc++/MSVC count/copy constructors allocate
    // exactly count slots, unlike growth reserve. Element heap storage is extra.
    return addPropertyBufferBytes(standardContainerProxyBytes(),
        standardAllocatorRequestBytes(multiplyPropertyBufferBytes(count, sizeof(T)), alignof(T)));
}

class WriteCopySizer final {
public:
    explicit WriteCopySizer(size_t maximum, const std::function<void()> &checkpoint)
        : maximum_(maximum), checkpoint_(checkpoint) {}
    void add(size_t bytes) {
        if (bytes > maximum_ - bytes_) throw query::execution::MaterializedByteLimitExceeded();
        bytes_ += bytes;
    }
    void tick() {
        if (--until_checkpoint_ == 0U) {
            if (checkpoint_) checkpoint_();
            until_checkpoint_ = 256U;
        }
    }
    void property(const PropertyValue &value) { propertyImpl<true>(value, 0U); }
    void observedProperty(const PropertyValue &value) { propertyImpl<false>(value, 0U); }
    void strings(std::span<const std::string> values) {
        add(writeVectorCopyHeapBytes<std::string>(values.size()));
        for (const auto &text : values) { tick(); add(writeStringCopyHeapUpper(text)); }
    }
    void observedStrings(const std::vector<std::string> &values) {
        add(query::execution::detail::standardVectorHeapBufferBytes(values));
        for (const auto &text : values) { tick(); add(query::execution::detail::dynamicStringBufferBytes(text)); }
    }
    template<class Range> void properties(const Range &values) {
        add(writeVectorCopyHeapBytes<typename Range::value_type>(values.size()));
        for (const auto &field : values) { tick(); add(writeStringCopyHeapUpper(field.key)); property(field.value); }
    }
    template<class Entry> void observedProperties(const std::vector<Entry> &values) {
        add(query::execution::detail::standardVectorHeapBufferBytes(values));
        for (const auto &field : values) {
            tick(); add(query::execution::detail::dynamicStringBufferBytes(field.key)); observedProperty(field.value);
        }
    }
    [[nodiscard]] size_t bytes() const noexcept { return bytes_; }
private:
    template<bool Copy> void propertyImpl(const PropertyValue &value, size_t depth) {
        if (depth > 64U) throw std::length_error("private Property copy nesting is too deep");
        tick();
        const auto &variant = value.getVariant();
        if (const auto *text = std::get_if<std::string>(&variant)) {
            if constexpr (Copy) add(writeStringCopyHeapUpper(*text));
            else add(query::execution::detail::dynamicStringBufferBytes(*text));
        } else if (const auto *bytes = std::get_if<PropertyValue::Bytes>(&variant)) {
            if constexpr (Copy) add(writeVectorCopyHeapBytes<std::byte>(bytes->size()));
            else add(query::execution::detail::standardVectorHeapBufferBytes(*bytes));
        } else if (const auto *list = std::get_if<std::vector<PropertyValue>>(&variant)) {
            if constexpr (Copy) add(writeVectorCopyHeapBytes<PropertyValue>(list->size()));
            else add(query::execution::detail::standardVectorHeapBufferBytes(*list));
            for (const auto &child : *list) propertyImpl<Copy>(child, depth + 1U);
        } else if (const auto *map = std::get_if<PropertyValue::MapType>(&variant)) {
            add(query::execution::detail::propertyMapStructuralHeapBufferBytes(*map));
            for (const auto &[key, child] : *map) {
                if constexpr (Copy) add(writeStringCopyHeapUpper(key));
                else add(query::execution::detail::dynamicStringBufferBytes(key));
                propertyImpl<Copy>(child, depth + 1U);
            }
        }
    }
    size_t maximum_, bytes_ = 0U, until_checkpoint_ = 256U;
    const std::function<void()> &checkpoint_;
};

// Both public input batches and internal PMR slot batches expose these fields.
// Only nested standard buffers are counted here; their parent slots have owners.
template<bool Copy, class Batch, class Patches>
size_t mutationNestedHeapBytes(const Batch &batch, const Patches &patches, size_t maximum,
        const std::function<void()> &checkpoint = {}) {
    WriteCopySizer meter(maximum, checkpoint);
    const auto strings = [&](const auto &values) {
        if constexpr (Copy) meter.strings(values); else meter.observedStrings(values);
    };
    const auto properties = [&](const auto &values) {
        if constexpr (Copy) meter.properties(values); else meter.observedProperties(values);
    };
    if (checkpoint) checkpoint();
    for (const auto &node : batch.node_creates) { meter.tick(); strings(node.labels); properties(node.properties); }
    for (const auto &edge : batch.edge_creates) {
        meter.tick();
        if constexpr (Copy) meter.add(writeStringCopyHeapUpper(edge.type));
        else meter.add(query::execution::detail::dynamicStringBufferBytes(edge.type));
        properties(edge.properties);
    }
    for (const auto &request : batch.property_replacements) { meter.tick(); properties(request.properties); }
    for (const auto &request : batch.node_label_replacements) { meter.tick(); strings(request.labels); }
    for (const auto &patch : patches) { meter.tick(); properties(patch.sets); strings(patch.removals); }
    if (checkpoint) checkpoint();
    return meter.bytes();
}

// MSVC's moving hash constructor allocates one default empty table before
// swapping contents (pinned xhash::_Hash(_Hash&&), minimum eight buckets).
// Other supported STLs leave empty, allocation-free map/vector/string sources.
inline size_t movedPropertyHeaderHeapUpper(const PropertyValue &value) {
    using namespace query::execution::detail;
#if defined(_MSVC_STL_VERSION)
    if (std::holds_alternative<PropertyValue::MapType>(value.getVariant())) {
        using Entry = PropertyValue::MapType::value_type;
        constexpr size_t node = sizeof(Entry) + 2U * sizeof(void *) + alignof(Entry) - 1U;
        return addPropertyBufferBytes(2U * standardContainerProxyBytes(),
            addPropertyBufferBytes(standardAllocatorRequestBytes(node, alignof(Entry)),
                standardAllocatorRequestBytes(16U * sizeof(void *), alignof(void *))));
    }
#endif
    const auto &variant = value.getVariant();
    return std::holds_alternative<std::string>(variant) || std::holds_alternative<PropertyValue::Bytes>(variant) ||
        std::holds_alternative<std::vector<PropertyValue>>(variant) ? standardContainerProxyBytes() : 0U;
}

struct MutationInputCopyBytes {
    size_t outer = 0U, nested = 0U, moved_from_proxies = 0U;
    [[nodiscard]] size_t total() const {
        return query::execution::detail::addPropertyBufferBytes(outer,
            query::execution::detail::addPropertyBufferBytes(nested, moved_from_proxies));
    }
};

inline MutationInputCopyBytes mutationInputCopyBytes(const LogicalGraphMutationBatch &batch,
        std::span<const LogicalGraphPropertyPatchRequest> patches, size_t maximum,
        const std::function<void()> &checkpoint = {}) {
    using namespace query::execution::detail;
    if (batch.creation_identity_plan)
        throw std::invalid_argument("private mutation input cannot import a creation identity plan");
    if (checkpoint) checkpoint();
    WriteCopySizer outer(maximum, checkpoint);
    outer.add(writeVectorCopyHeapBytes<LogicalGraphNodeCreateRequest>(batch.node_creates.size()));
    outer.add(writeVectorCopyHeapBytes<LogicalGraphBatchEdgeCreateRequest>(batch.edge_creates.size()));
    outer.add(writeVectorCopyHeapBytes<LogicalGraphPropertyMutationRequest>(batch.property_replacements.size()));
    outer.add(writeVectorCopyHeapBytes<LogicalGraphNodeLabelMutationRequest>(batch.node_label_replacements.size()));
    outer.add(writeVectorCopyHeapBytes<LogicalGraphEdgeDeleteRequest>(batch.edge_deletes.size()));
    outer.add(writeVectorCopyHeapBytes<LogicalGraphNodeDeleteRequest>(batch.node_deletes.size()));
    outer.add(writeVectorCopyHeapBytes<LogicalGraphPropertyPatchRequest>(patches.size()));
    WriteCopySizer nested(maximum - outer.bytes(), checkpoint);
    size_t movableHeaders = 0U;
    const auto headers = [&](size_t count) { movableHeaders = addPropertyBufferBytes(movableHeaders, count); };
    for (const auto &node : batch.node_creates) {
        nested.tick(); nested.strings(node.labels); nested.properties(node.properties); headers(2U);
    }
    for (const auto &edge : batch.edge_creates) {
        nested.tick(); nested.add(writeStringCopyHeapUpper(edge.type)); nested.properties(edge.properties); headers(2U);
    }
    for (const auto &request : batch.property_replacements) { nested.properties(request.properties); headers(1U); }
    for (const auto &request : batch.node_label_replacements) { nested.strings(request.labels); headers(1U); }
    for (const auto &patch : patches) { nested.properties(patch.sets); nested.strings(patch.removals); headers(2U); }
    MutationInputCopyBytes result{outer.bytes(), nested.bytes(),
        multiplyPropertyBufferBytes(movableHeaders, standardContainerProxyBytes())};
    if (result.total() > maximum) throw query::execution::MaterializedByteLimitExceeded();
    if (checkpoint) checkpoint();
    return result;
}

} // namespace graph::storage::detail
