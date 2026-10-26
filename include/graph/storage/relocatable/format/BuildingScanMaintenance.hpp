#pragma once

#include <memory>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/CancellationCheckpoint.hpp"
#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {
    // Private BUILDING-image maintenance authority. It cannot clear BUILDING,
    // replace logical field roots, or authorize an ordinary database write.
    struct BuildingScanMaintenanceRequest final {
        DatabaseId expected_database_id{};
        Generation expected_generation{};
        PhysicalPageRef expected_descriptor{};
        // Optional exact pool for one COW evacuation; an invalid slot retains
        // the ordinary single-empty-pool reclamation operation.
        PhysicalSlotId evacuate_pool{};
        std::shared_ptr<query::execution::MaterializedByteBudget> memory;
        const CancellationCheckpoint *cancellation = nullptr;
        BuildingScanMaintenanceRequest(const BuildingScanMaintenanceRequest &) = default;
        BuildingScanMaintenanceRequest(BuildingScanMaintenanceRequest &&) noexcept = default;
        BuildingScanMaintenanceRequest &operator=(const BuildingScanMaintenanceRequest &) = default;
        BuildingScanMaintenanceRequest &operator=(BuildingScanMaintenanceRequest &&) noexcept = default;
    private:
        friend class ScanBuildingImageBuilder;
        friend class BuildingScanMembershipTestAccess;
        BuildingScanMaintenanceRequest() = default;
    };
}
