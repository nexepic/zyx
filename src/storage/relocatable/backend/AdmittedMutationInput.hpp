/**
 * @file AdmittedMutationInput.hpp
 * @brief Pre-admitted private normalization input for native mutation staging.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once

#include <memory>
#include "graph/storage/MutationHeapBytes.hpp"
#include "graph/utils/OwnedPmrObject.hpp"

namespace graph::storage::detail {
class AdmittedMutationInput final : public utils::OwnedPmrObject {
public:
    static std::unique_ptr<AdmittedMutationInput> copy(
            std::shared_ptr<query::execution::MaterializedByteBudget> budget,
            const LogicalGraphMutationBatch &batch, const std::vector<LogicalGraphPropertyPatchRequest> &patches,
            const std::function<void()> &checkpoint = {}) {
        if (!budget) throw std::invalid_argument("private mutation copy requires its owning budget");
        const auto bytes = mutationInputCopyBytes(batch, patches, budget->availableWithParent(), checkpoint);
        return std::unique_ptr<AdmittedMutationInput>(new (budget) AdmittedMutationInput(budget, batch, patches, bytes, checkpoint));
    }
    void transferNestedTo(query::execution::MaterializedByteReservation &receiver) {
        if (transferred_) throw std::logic_error("private input payload was already transferred");
        auto nested = charge_.splitOff(copy_bytes_.nested);
        try { receiver.absorb(std::move(nested)); }
        catch (...) { charge_.absorb(std::move(nested)); throw; }
        transferred_ = true;
    }
    [[nodiscard]] const MutationInputCopyBytes &copyBytes() const noexcept { return copy_bytes_; }
private:
    static query::execution::MaterializedByteReservation admit(
            query::execution::MaterializedByteBudget &budget, size_t bytes) {
        query::execution::MaterializedByteReservation charge(&budget); charge.set(bytes); return charge;
    }
    AdmittedMutationInput(std::shared_ptr<query::execution::MaterializedByteBudget> budget,
            const LogicalGraphMutationBatch &source, const std::vector<LogicalGraphPropertyPatchRequest> &sourcePatches,
            MutationInputCopyBytes bytes, const std::function<void()> &checkpoint)
        : budget_(std::move(budget)), charge_(admit(*budget_, bytes.total())), copy_bytes_(bytes) {
        const auto clone = [&]<class T>(std::vector<T> &destination, const std::vector<T> &input) {
            destination.reserve(input.size());
            size_t untilCheckpoint = 256U;
            for (const auto &entry : input) {
                destination.push_back(entry);
                if (--untilCheckpoint == 0U) {
                    if (checkpoint) checkpoint();
                    untilCheckpoint = 256U;
                }
            }
        };
        clone(batch.node_creates, source.node_creates);
        clone(batch.edge_creates, source.edge_creates);
        clone(batch.property_replacements, source.property_replacements);
        clone(batch.node_label_replacements, source.node_label_replacements);
        clone(batch.edge_deletes, source.edge_deletes);
        clone(batch.node_deletes, source.node_deletes);
        clone(patches, sourcePatches);
        if (checkpoint) checkpoint();
    }
    // Data must be destroyed before reservations and their strong budget owner.
    std::shared_ptr<query::execution::MaterializedByteBudget> budget_;
    query::execution::MaterializedByteReservation charge_;
    MutationInputCopyBytes copy_bytes_;
    bool transferred_ = false;
public:
    // Consumers may rebase/move/erase fields; growth requires separate admission.
    LogicalGraphMutationBatch batch;
    std::vector<LogicalGraphPropertyPatchRequest> patches;
};
} // namespace graph::storage::detail
