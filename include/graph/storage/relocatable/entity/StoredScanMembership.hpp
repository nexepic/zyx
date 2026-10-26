#pragma once

#include <bit>
#include <algorithm>
#include <functional>
#include <limits>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/entity/ScanMembershipDirectory.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldPageStore.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"

namespace graph::storage::scan_candidate_detail {
    struct StoredMembershipCounts {
        uint64_t nodes = 0, groups = 0;
        bool exhausted = false;
    };

    // Authenticates stored routing/bytes only. This diagnostic/storage stream
    // grants no primary-liveness certificate and no completion capability.
    // The visitor borrows one fixed 64-ID record synchronously. Prefix stops
    // do not certify descriptor totals; exceptions never replay a prefix.
    [[nodiscard]] inline StoredMembershipCounts streamStoredMembership(
        const std::shared_ptr<StorageIO> &io, RelocatableMetadataStore &metadata,
        const MetadataSnapshotPin &snapshot,
        const std::shared_ptr<query::execution::MaterializedByteBudget> &memory,
        size_t maximumGroups, const std::function<bool(const ScanBlockMembership &)> &visitor,
        const CancellationCheckpoint *cancellation = nullptr) {
#if !ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
        static_cast<void>(io); static_cast<void>(metadata); static_cast<void>(snapshot);
        static_cast<void>(memory); static_cast<void>(maximumGroups); static_cast<void>(visitor);
        static_cast<void>(cancellation);
        throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "stored scan membership requires its isolated format");
#else
        constexpr size_t limit = 1024U * 1024U;
        if (!io || !metadata.usesDataStorage(io) || !metadata.ownsSnapshot(snapshot) || !memory ||
            memory->used() > limit || memory->available() > limit - memory->used() || !maximumGroups || !visitor)
            throw std::invalid_argument("stored membership requires its pinned file, visitor and bounded source account");
        const auto check = [&] {
            if (cancellation) cancellation->check();
            if (!metadata.ownsSnapshot(snapshot))
                throw std::invalid_argument("stored membership stream lost its original snapshot");
        };
        check();
        utils::OwnedAllocationScope owner(memory);
        utils::OwnedHeapReservation frontier(memory, sizeof(ScanBlockMembership) + sizeof(MetadataPageFile) +
            sizeof(SlottedPagePool) + sizeof(BTreePageStore) + sizeof(HybridPropertyFieldPageStore) + 16U * 1024U);
        const auto &root = snapshot.superblock();
        std::optional<HybridPropertyDescriptor> descriptor;
        if (root.hybrid_property_descriptor_root.isValid()) {
            MetadataPageFile pages(io, root);
            descriptor = pages.readHybridPropertyDescriptor(root.hybrid_property_descriptor_root, root).descriptor;
        }
        constexpr auto ordinal = static_cast<size_t>(HybridPropertyRootOrdinal::HPRO_NODE_SCAN);
        const auto indexId = hybridPropertyIndexId(HybridPropertyRootOrdinal::HPRO_NODE_SCAN);
        const bool hasRoot = descriptor && descriptor->roots[ordinal].page_id.isValid();
        const auto require = [](bool valid, const char *message) {
            if (!valid) throw FormatException(FormatError::FERR_INVALID_STATE, message);
        };
        require(hasRoot == metadata.findIndexRootOptional(snapshot, indexId).has_value(),
            "stored membership has inconsistent descriptor/Index Root ownership");
        if (!hasRoot) {
            require(!descriptor || (!descriptor->scan_live_node_count && !descriptor->scan_membership_group_count),
                "stored membership counts name an absent directory");
            return {0, 0, true};
        }
        require(descriptor->scan_live_node_count && descriptor->scan_membership_group_count,
            "stored membership root has empty descriptor counts");
        SlottedPagePool pages(io, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore trees(pages, metadata);
        HybridPropertyFieldPageStore fields(io, metadata);
        const auto binding = ScanMembershipDirectoryCodec::bind(*descriptor, snapshot);
        ScanMembershipDirectoryMap map(indexId, trees, metadata, binding);
        utils::OwnedHeapReservation boundary(memory);
        const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        StoredMembershipCounts counts;
        uint64_t previous = 0;
        std::optional<HybridPropertyFieldPage> retainedPage;
        PageId retainedId{};
        uint64_t retainedEpoch = UINT64_MAX;
        const bool exhausted = map.visitAll(snapshot, (std::min)(maximumGroups, (std::numeric_limits<size_t>::max)() - 1U), [&](const auto &entry) {
            check();
            const uint64_t group = ScanMembershipDirectoryCodec::decodeKey(entry.key);
            require(group > previous, "stored membership groups are not strictly ordered");
            previous = group;
            const auto slot = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
            const auto epoch = io->dataMutationEpoch();
            if (!retainedPage || retainedId != slot.page_id || epoch == UINT64_MAX || retainedEpoch != epoch) {
                retainedPage.reset();
                retainedPage.emplace(fields.read(snapshot, slot.page_id, BufferPoolReadHint::BPRH_SEQUENTIAL));
                retainedId = slot.page_id;
                retainedEpoch = io->dataMutationEpoch() == epoch ? epoch : UINT64_MAX;
                debug::PerfTrace::addValue("native_graph.scan_membership.page_acquisitions", 1);
            } else debug::PerfTrace::addValue("native_graph.scan_membership.page_reuses", 1);
            const auto membership = retainedPage->view().readScanMembership(slot, root.database_id, group,
                descriptor->semantic_generation);
            const uint64_t live = std::popcount(membership.live);
            require(counts.nodes <= UINT64_MAX - live && counts.groups != UINT64_MAX,
                "stored membership counts overflowed");
            counts.nodes += live; ++counts.groups;
            const bool keepGoing = visitor(membership);
            check();
            return keepGoing;
        }, std::ref(admit), cancellation);
        if (exhausted) {
            require(counts.nodes == descriptor->scan_live_node_count &&
                counts.groups == descriptor->scan_membership_group_count,
                "stored membership totals disagree with its descriptor");
        }
        counts.exhausted = exhausted;
        return counts;
#endif
    }
}
