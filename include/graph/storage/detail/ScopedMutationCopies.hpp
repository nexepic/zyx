/**
 * @file ScopedMutationCopies.hpp
 * @brief Pre-admission of standard mutation DTO copies in a borrowed scope.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include "graph/core/PropertyDecodeAllocationScope.hpp"
#include "graph/storage/MutationHeapBytes.hpp"

namespace graph::storage::detail {
inline void admitScopedMutationBytes(size_t bytes) {
    if (bytes) if (auto *scope = property_decode::AllocationScope::current()) scope->admit(bytes);
}

template<class Size>
void sizeScopedMutationCopy(Size &&size) {
    auto *scope = property_decode::AllocationScope::current();
    if (!scope) return;
    const auto checkpoint = [scope] { scope->checkpoint(); };
    const std::function<void()> notify = std::ref(checkpoint);
    WriteCopySizer meter((std::numeric_limits<size_t>::max)(), notify);
    size(meter);
    scope->admit(meter.bytes());
}

template<class Output, class Range>
void admitScopedPropertyCopies(const Range &properties) {
    sizeScopedMutationCopy([&](WriteCopySizer &meter) {
        meter.add(writeVectorCopyHeapBytes<Output>(properties.size()));
        for (const auto &property : properties) {
            meter.tick(); meter.add(writeStringCopyHeapUpper(property.key)); meter.property(property.value);
        }
    });
}

template<class Range>
void admitScopedPropertyPayloadCopies(const Range &properties) {
    sizeScopedMutationCopy([&](WriteCopySizer &meter) {
        for (const auto &property : properties) {
            meter.tick(); meter.add(writeStringCopyHeapUpper(property.key)); meter.property(property.value);
        }
    });
}

inline void admitScopedMutationBatchCopy(const LogicalGraphMutationBatch &batch, bool propertyPayloads = true) {
    sizeScopedMutationCopy([&](WriteCopySizer &meter) {
        meter.add(writeVectorCopyHeapBytes<LogicalGraphNodeCreateRequest>(batch.node_creates.size()));
        meter.add(writeVectorCopyHeapBytes<LogicalGraphBatchEdgeCreateRequest>(batch.edge_creates.size()));
        meter.add(writeVectorCopyHeapBytes<LogicalGraphPropertyMutationRequest>(batch.property_replacements.size()));
        meter.add(writeVectorCopyHeapBytes<LogicalGraphNodeLabelMutationRequest>(batch.node_label_replacements.size()));
        meter.add(writeVectorCopyHeapBytes<LogicalGraphNodeDeleteRequest>(batch.node_deletes.size()));
        meter.add(writeVectorCopyHeapBytes<LogicalGraphEdgeDeleteRequest>(batch.edge_deletes.size()));
        for (const auto &node : batch.node_creates) { meter.tick(); meter.strings(node.labels); meter.properties(node.properties); }
        for (const auto &edge : batch.edge_creates) {
            meter.tick(); meter.add(writeStringCopyHeapUpper(edge.type)); meter.properties(edge.properties);
        }
        if (propertyPayloads) for (const auto &replacement : batch.property_replacements) { meter.tick(); meter.properties(replacement.properties); }
        for (const auto &labels : batch.node_label_replacements) { meter.tick(); meter.strings(labels.labels); }
        if (batch.creation_identity_plan) {
            const auto &plan = *batch.creation_identity_plan;
            meter.add(writeVectorCopyHeapBytes<LogicalGraphCreationIdentityRange>(plan.node_id_ranges.size()));
            meter.add(writeVectorCopyHeapBytes<uint64_t>(plan.node_ids.size()));
            meter.add(writeVectorCopyHeapBytes<LogicalGraphCreationIdentityRange>(plan.edge_id_ranges.size()));
            meter.add(writeVectorCopyHeapBytes<uint64_t>(plan.edge_ids.size()));
        }
    });
}
} // namespace graph::storage::detail
