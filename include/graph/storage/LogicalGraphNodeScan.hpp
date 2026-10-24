#pragma once

#include "graph/storage/LogicalGraphRead.hpp"

namespace graph::storage {
    // Optional complete-scan capability. Other snapshot implementations retain
    // their existing scan path and do not need to manufacture certifications.
    class LogicalGraphNodeScanProvider {
    public:
        virtual ~LogicalGraphNodeScanProvider() = default;
        // Sources and their borrows are valid only during the batch visitor.
        // Requested IDs must be a strictly sorted subset of the certified batch.
        using NodeScanPropertySource = std::function<void(std::span<const uint64_t>,
            const LogicalGraphReadSnapshot::PropertyCellSink &, const PropertyColumnBatchReadSinks &,
            const std::function<void(size_t)> &)>;
        // examined includes candidates rejected by the label predicate.
        using NodeScanBatchVisitor = std::function<bool(std::span<const uint64_t>, size_t,
            const NodeScanPropertySource &)>;
        // nullopt is unsupported, before any visitor call; false is consumer stop.
        // Admission precedes source access. Consumer exceptions propagate without replay.
        [[nodiscard]] virtual std::optional<bool> tryVisitNodeScanBatches(
            size_t maximumRecords, size_t maximumBatchRows, const std::vector<std::string> &labels,
            std::span<const std::string_view> keys, const NodeScanBatchVisitor &visitor,
            const std::function<void(size_t)> &admitBatch) const = 0;
    };
}
