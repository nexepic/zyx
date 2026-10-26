#pragma once

#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

namespace graph::storage {
    // Internal BUILDING-only preparation. There is intentionally no supplied
    // live/label bitmap, final-publication flag, or public-open capability.
    struct BuildingScanMembershipRequest {
        BuildingScanMembershipRequest(const BuildingScanMembershipRequest &) = default;
        BuildingScanMembershipRequest(BuildingScanMembershipRequest &&) noexcept = default;
        BuildingScanMembershipRequest &operator=(const BuildingScanMembershipRequest &) = default;
        BuildingScanMembershipRequest &operator=(BuildingScanMembershipRequest &&) noexcept = default;
        DatabaseId expected_database_id{};
        Generation expected_generation{};
        PhysicalPageRef expected_descriptor{};
        uint64_t group_id = 0;
        PhysicalSlotId first_slot{}, last_slot{};
        uint64_t field_pool_owner_id = 0;
        std::shared_ptr<query::execution::MaterializedByteBudget> memory;
        const CancellationCheckpoint *cancellation = nullptr;
    private:
        friend class ScanBuildingImageBuilder;
        friend class BuildingScanMembershipTestAccess;
        BuildingScanMembershipRequest() = default;
    };
}
