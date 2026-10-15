/**
 * @file OverlayPayloadBudget.hpp
 * @brief Pre-admitted retained and replacement buffers for transaction overlays.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include "graph/storage/MutationHeapBytes.hpp"
#include <memory_resource>
#include <type_traits>

namespace graph::storage::detail {
template<bool Copy, class Payload>
size_t overlayPayloadHeapBytes(const Payload &owner, size_t maximum) {
    const std::function<void()> checkpoint;
    WriteCopySizer meter(maximum, checkpoint);
    if constexpr (std::is_same_v<Payload, LogicalGraphNode>) {
        if constexpr (Copy) meter.strings(owner.labels); else meter.observedStrings(owner.labels);
    } else {
        if constexpr (Copy) meter.add(writeStringCopyHeapUpper(owner.type));
        else meter.add(query::execution::detail::dynamicStringBufferBytes(owner.type));
    }
    if constexpr (Copy) meter.properties(owner.properties); else meter.observedProperties(owner.properties);
    return meter.bytes();
}

// The overlay's strong memory owner outlives these charges and its undo tables.
// Non-budget legacy providers retain their existing allocation contract.
class OverlayPayloadBudget final {
public:
    explicit OverlayPayloadBudget(std::pmr::memory_resource *memory, size_t bytes)
        : memory_(memory), budget_(dynamic_cast<query::execution::MaterializedByteBudget *>(memory)),
          retained_(budget_) { retained_.set(bytes); }
    template<class Payload>
    OverlayPayloadBudget(std::pmr::memory_resource *memory, const Payload &source)
        : OverlayPayloadBudget(memory, overlayPayloadHeapBytes<true>(source, available(memory))) {}
    [[nodiscard]] std::pmr::memory_resource *memory() const noexcept { return memory_; }
    void swap(OverlayPayloadBudget &other) noexcept {
        using std::swap; swap(memory_, other.memory_); swap(budget_, other.budget_); swap(retained_, other.retained_);
    }
    OverlayPayloadBudget(const OverlayPayloadBudget &) = delete;
    OverlayPayloadBudget &operator=(const OverlayPayloadBudget &) = delete;

    template<class Payload>
    void replaceProperties(Payload &owner, std::span<const LogicalGraphWriteProperty> source) {
        const std::function<void()> checkpoint;
        WriteCopySizer meter(headroom(), checkpoint);
        meter.add(writeVectorCopyHeapBytes<LogicalGraphPropertyEntry>(source.size()));
        for (const auto &field : source) { meter.add(writeStringCopyHeapUpper(field.key)); meter.property(field.value); }
        query::execution::MaterializedByteReservation incoming(budget_); incoming.set(meter.bytes());
        {
            std::pmr::vector<const LogicalGraphWriteProperty *> ordered(memory_);
            ordered.reserve(source.size());
            for (const auto &field : source) ordered.push_back(&field);
            std::ranges::sort(ordered, [](const auto *left, const auto *right) { return left->key < right->key; });
            for (size_t i = 0U; i < ordered.size(); ++i)
                if (ordered[i]->key.empty() || (i != 0U && ordered[i-1U]->key == ordered[i]->key))
                    throw std::invalid_argument("logical graph transaction overlay requires nonempty unique Property keys");
            std::vector<LogicalGraphPropertyEntry> output; output.reserve(source.size());
            for (const auto *field : ordered) output.emplace_back(field->key, field->value);
            owner.properties.swap(output);
        } // Old fields and PMR workspace die before either credit is refunded.
        retained_.absorb(std::move(incoming)); trim(owner);
    }
    void replaceLabels(LogicalGraphNode &owner, const std::vector<std::string> &source) {
        const std::function<void()> checkpoint;
        WriteCopySizer meter(headroom(), checkpoint); meter.strings(source);
        query::execution::MaterializedByteReservation incoming(budget_); incoming.set(meter.bytes());
        {
            std::pmr::vector<const std::string *> ordered(memory_); ordered.reserve(source.size());
            for (const auto &label : source) ordered.push_back(&label);
            std::ranges::sort(ordered, [](const auto *left, const auto *right) { return *left < *right; });
            for (size_t i = 0U; i < ordered.size(); ++i)
                if (ordered[i]->empty() || (i != 0U && *ordered[i-1U] == *ordered[i]))
                    throw std::invalid_argument("logical graph transaction overlay requires nonempty unique labels");
            std::vector<std::string> output; output.reserve(source.size());
            for (const auto *label : ordered) output.emplace_back(*label);
            owner.labels.swap(output);
        }
        retained_.absorb(std::move(incoming)); trim(owner);
    }
    void replaceType(LogicalGraphEdge &owner, const std::string &source) {
        query::execution::MaterializedByteReservation incoming(budget_); incoming.set(writeStringCopyHeapUpper(source));
        { std::string output(source); owner.type.swap(output); }
        retained_.absorb(std::move(incoming)); trim(owner);
    }
private:
    static size_t available(std::pmr::memory_resource *memory) {
        const auto *budget = dynamic_cast<query::execution::MaterializedByteBudget *>(memory);
        return budget ? budget->availableWithParent() : (std::numeric_limits<size_t>::max)();
    }
    [[nodiscard]] size_t headroom() const { return available(memory_); }
    template<class Payload> void trim(const Payload &owner) {
        // This may only shrink a pre-admitted total, never charge post hoc.
        retained_.set(overlayPayloadHeapBytes<false>(owner, retained_.bytes()));
    }
    std::pmr::memory_resource *memory_;
    query::execution::MaterializedByteBudget *budget_;
    query::execution::MaterializedByteReservation retained_;
};
} // namespace graph::storage::detail
