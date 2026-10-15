/**
 * @file PropertyDecodeWorkspace.hpp
 * @brief Retained admission for decoded values in one private source lifetime.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#pragma once
#include <functional>
#include <memory>
#include <optional>
#include "graph/core/PropertyDecodeAllocationScope.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/query/execution/PrepaidMemoryResource.hpp"
#include "graph/query/execution/StandardContainerHeapBytes.hpp"
#include "graph/utils/OwnedPmrAllocator.hpp"

namespace graph::storage::detail {
// Declare before source values, and destroy every decoded value before this
// workspace. This covers decoder payloads, not all reader metadata/name slots.
class PropertyDecodeWorkspace final {
public:
    explicit PropertyDecodeWorkspace(const std::shared_ptr<std::pmr::memory_resource> &memory,
            const std::function<void()> *checkpoint = nullptr)
        : budget_(std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(memory)),
          charge_(budget_.get()), checkpoint_(checkpoint) {
        if (auto prepaid = std::dynamic_pointer_cast<query::execution::detail::PrepaidMemoryResource>(memory))
            prepaid_charge_.emplace(std::move(prepaid), 0U);
        if (budget_ || prepaid_charge_) {
            allocation_owner_.emplace(memory);
            scope_.emplace(this, &admit, checkpoint ? &check : nullptr);
        }
    }
    PropertyDecodeWorkspace(const PropertyDecodeWorkspace &) = delete;
    PropertyDecodeWorkspace &operator=(const PropertyDecodeWorkspace &) = delete;
private:
    static void admit(void *context, size_t bytes) {
        if (bytes == 0U) return;
        auto &self = *static_cast<PropertyDecodeWorkspace *>(context);
        if (self.prepaid_charge_)
            self.prepaid_charge_->set(query::execution::detail::addStandardHeapBytes(self.prepaid_charge_->bytes(), bytes));
        else self.charge_.set(query::execution::detail::addStandardHeapBytes(self.charge_.bytes(), bytes));
    }
    static void check(void *context) {
        const auto *checkpoint = static_cast<PropertyDecodeWorkspace *>(context)->checkpoint_;
        if (checkpoint && *checkpoint) (*checkpoint)();
    }
    std::shared_ptr<query::execution::MaterializedByteBudget> budget_;
    query::execution::MaterializedByteReservation charge_;
    const std::function<void()> *checkpoint_;
    std::optional<query::execution::detail::PrepaidMemoryResource::ExternalReservation> prepaid_charge_;
    std::optional<utils::OwnedAllocationScope> allocation_owner_;
    std::optional<property_decode::AllocationScope> scope_;
};
} // namespace graph::storage::detail
