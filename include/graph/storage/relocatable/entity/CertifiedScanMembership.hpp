#pragma once

#include "graph/storage/relocatable/entity/ScanReadProof.hpp"
#include "graph/storage/relocatable/entity/StoredScanMembership.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"

namespace graph::storage {
    // Complete, version-bound supply for the existing scan consumer. This is
    // distinct from the raw storage stream, which grants no read authority.
    class CertifiedScanMembership final {
    public:
        using Visitor = std::function<bool(std::span<const uint64_t>, size_t, const ScanBatchReadGrant &)>;
        [[nodiscard]] static bool visit(const VerifiedScanRoot &verified,
            const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
            const MetadataSnapshotPin &snapshot,
            const std::shared_ptr<query::execution::MaterializedByteBudget> &workspace,
            size_t maximumRecords, size_t maximumBatchRows, std::span<const uint64_t> requiredLabels,
            const Visitor &visitor, const CancellationCheckpoint *cancellation = nullptr) {
            if (!maximumRecords || !maximumBatchRows || maximumBatchRows > ScanBatchReadGrant::MAXIMUM_ROWS ||
                !io || !workspace || workspace->used() > 1024U * 1024U ||
                workspace->available() > 1024U * 1024U - workspace->used() || !visitor)
                throw std::invalid_argument("certified scan requires bounded source, rows and visitor");
            uint64_t previousLabel = 0;
            for (const auto token : requiredLabels) {
                if (!token || token <= previousLabel)
                    throw std::invalid_argument("certified scan labels must be ordered, unique and nonzero");
                previousLabel = token;
            }
            const VerifiedScanRoot root(verified);
            const auto check = [&] {
                if (cancellation) cancellation->check();
                if (!root.matches(metadata, *io, snapshot))
                    throw FormatException(FormatError::FERR_INVALID_STATE, "certified scan lost its verified root");
            };
            check();
            // Restore the caller's allocation domain for consuming property
            // batches. Membership buffers remain owned by this finite source.
            const auto consumerOwner = utils::OwnedAllocationScope::owner();
            utils::OwnedAllocationScope owner(workspace);
            utils::OwnedHeapReservation frontier(workspace, 16U * 1024U +
                ScanBatchReadGrant::MAXIMUM_ROWS * sizeof(uint64_t));
            std::array<uint64_t, ScanBatchReadGrant::MAXIMUM_ROWS> ids{};
            size_t selected = 0, examined = 0, totalExamined = 0;
            bool keepGoing = true;
            const auto flush = [&] {
                if (!examined) return true;
                check();
                const auto batch = std::span(ids).first(selected);
                const ScanBatchReadGrant grant(root, snapshot, batch, workspace);
                {
                    utils::OwnedAllocationScope consumer(consumerOwner);
                    keepGoing = visitor(batch, examined, grant);
                }
                check(); selected = examined = 0;
                return keepGoing;
            };
            static_cast<void>(scan_candidate_detail::streamStoredMembership(io, metadata, snapshot, workspace, SIZE_MAX,
                [&](const ScanBlockMembership &membership) {
                    check();
                    const uint64_t selectedMask = membership.labels_from_primary && !requiredLabels.empty() ?
                        selectPrimaryLabels(io, metadata, snapshot, workspace, membership, requiredLabels, cancellation) :
                        membership.select(requiredLabels);
                    uint64_t live = membership.live;
                    while (live && totalExamined < maximumRecords) {
                        const auto bit = static_cast<unsigned>(std::countr_zero(live));
                        live &= live - 1U;
                        const uint64_t id = (membership.group_id - 1U) * 64U + 1U + bit;
                        if (selectedMask & (UINT64_C(1) << bit)) ids[selected++] = id;
                        ++examined; ++totalExamined;
                        if (examined == maximumBatchRows && !flush()) return false;
                    }
                    return totalExamined < maximumRecords;
                }, cancellation));
            check();
            if (keepGoing) static_cast<void>(flush());
            return keepGoing;
        }
    private:
        [[nodiscard]] static uint64_t selectPrimaryLabels(const std::shared_ptr<StorageIO> &io,
            RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
            const std::shared_ptr<query::execution::MaterializedByteBudget> &workspace,
            const ScanBlockMembership &membership, std::span<const uint64_t> requiredLabels,
            const CancellationCheckpoint *cancellation) {
            utils::OwnedHeapReservation frontier(workspace, 16U * 1024U +
                64U * (sizeof(uint64_t) + sizeof(std::optional<NodeEntityRecord>)));
            EntityPageStore entities(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            SlottedPagePool pages(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            EntityLabelSetStore labels(io, metadata, pages, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            std::array<uint64_t, 64> ids{}; size_t count = 0;
            uint64_t live = membership.live;
            while (live) {
                const auto bit = static_cast<unsigned>(std::countr_zero(live)); live &= live - 1U;
                ids[count++] = (membership.group_id - 1U) * 64U + 1U + bit;
            }
            utils::OwnedHeapReservation nodeBytes(workspace);
            const auto nodes = entities.readNodesSorted(snapshot, std::span(ids).first(count),
                [&](size_t bytes) { nodeBytes.set(bytes); });
            hybrid_property_detail::require(nodes.size() == count, "primary label fallback changed its Node cardinality");
            uint64_t selected = 0;
            for (size_t i = 0; i < count; ++i) {
                if (cancellation) cancellation->check();
                hybrid_property_detail::require(bool(nodes[i]), "primary label fallback lost a certified live Node");
                utils::OwnedHeapReservation labelBytes(workspace);
                const auto actual = labels.readNodeLabels(snapshot, *nodes[i], [&](size_t bytes) { labelBytes.set(bytes); });
                if (std::includes(actual.begin(), actual.end(), requiredLabels.begin(), requiredLabels.end()))
                    selected |= UINT64_C(1) << ((ids[i] - 1U) % 64U);
            }
            debug::PerfTrace::addValue("native_graph.read.primary_label_fallback_groups", 1);
            return selected;
        }
    };
}
