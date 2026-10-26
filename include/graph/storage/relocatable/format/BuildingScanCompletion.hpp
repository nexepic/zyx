#pragma once

#include <functional>
#include "graph/storage/relocatable/format/BuildingScanMaintenance.hpp"

namespace graph::storage {
    // A single-use producer certificate. Only the offline builder can mint it
    // after a fresh complete integrity scan; readers and raw membership streams
    // cannot authorize completion.
    class BuildingScanCompletionRequest final {
        friend class ScanBuildingImageBuilder;
        friend class RelocatableMetadataStore;
        BuildingScanCompletionRequest() = default;
        DatabaseId database{};
        Generation generation{}, source_integrity_generation{};
        PhysicalPageRef descriptor{};
        uint64_t io_epoch = UINT64_MAX, source_bytes = 0, maximum_total_bytes = 0;
        std::shared_ptr<query::execution::MaterializedByteBudget> memory;
        const CancellationCheckpoint *cancellation = nullptr;
        // Installed exclusively by the builder, checked under the writer barrier
        // immediately before publishing. This does not expose a caller skip flag.
        std::function<void()> require_stable_files;
    };
}
