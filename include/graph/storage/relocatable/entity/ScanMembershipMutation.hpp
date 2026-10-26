#pragma once

#include "graph/storage/relocatable/entity/ScanMembershipSource.hpp"
#include "graph/storage/relocatable/entity/ScanMembershipDirectory.hpp"
#include <bit>

namespace graph::storage::scan_candidate_detail {
    enum class ScanNodeChangeKind : uint8_t { SNCK_CREATE = 1, SNCK_DELETE, SNCK_RELABEL };
    struct ScanNodeChange {
        uint64_t id = 0;
        ScanNodeChangeKind kind = ScanNodeChangeKind::SNCK_CREATE;
        std::span<const uint64_t> labels;
    };
    // Raw bounded preparation, not a publication capability. The sealed graph
    // coordinator must bind these changes to its actual Entity/LabelSet images
    // and publish the resulting membership in that same metadata batch.
    struct ScanMembershipMutationPlan {
        std::optional<ScanBlockMembership> before, after;
        int64_t live_node_delta = 0, group_delta = 0;
    };

    class ScanMembershipMutation final {
    public:
        [[nodiscard]] static ScanMembershipMutationPlan prepare(RelocatableMetadataStore &metadata,
            EntityPageStore &entities, EntityLabelSetStore &labels, const MetadataSnapshotPin &snapshot,
            uint64_t groupId, Generation nextGeneration, std::span<const ScanNodeChange> changes,
            const std::shared_ptr<query::execution::MaterializedByteBudget> &workspace,
            const CancellationCheckpoint *cancellation = nullptr) {
            static_cast<void>(ScanMembershipDirectoryCodec::key(groupId));
            if (!workspace || workspace->used() > 1024U * 1024U ||
                workspace->available() > 1024U * 1024U - workspace->used() ||
                !metadata.ownsSnapshot(snapshot) || !entities.usesMetadataStore(metadata) ||
                !labels.usesMetadataStore(metadata) || changes.empty() || changes.size() > 64U ||
                snapshot.superblock().generation.value() == UINT64_MAX ||
                nextGeneration.value() != snapshot.superblock().generation.value() + 1U)
                throw std::invalid_argument("scan membership mutation needs one fenced group and bounded workspace");
            const auto check = [&] {
                if (cancellation) cancellation->check();
                if (!metadata.ownsSnapshot(snapshot))
                    throw std::invalid_argument("scan membership preparation lost its source pin");
            };
            check();
            uint64_t previous = 0;
            for (const auto &change : changes) {
                if (!change.id || change.id <= previous || 1U + (change.id - 1U) / 64U != groupId ||
                    (change.kind != ScanNodeChangeKind::SNCK_CREATE && change.kind != ScanNodeChangeKind::SNCK_DELETE &&
                        change.kind != ScanNodeChangeKind::SNCK_RELABEL) ||
                    (change.kind == ScanNodeChangeKind::SNCK_DELETE && !change.labels.empty()))
                    throw std::invalid_argument("scan Node changes must be ordered, unique and in one group");
                previous = change.id;
                uint64_t token = 0;
                for (const auto next : change.labels) {
                    if (!next || next <= token)
                        throw std::invalid_argument("scan Node change requires complete canonical labels");
                    token = next;
                }
            }
            utils::OwnedAllocationScope owner(workspace);
            utils::OwnedHeapReservation frontier(workspace, sizeof(ScanMembershipMutationPlan) +
                64U * (sizeof(uint64_t) + sizeof(ScanNodeChange)) + 16U * 1024U);
            const size_t width = groupId == ScanMembershipDirectoryCodec::MAX_GROUP ? 63U : 64U;
            std::array<uint64_t, 64> ids{};
            const auto first = (groupId - 1U) * 64U + 1U;
            for (size_t i = 0; i < width; ++i) ids[i] = first + i;
            utils::OwnedHeapReservation nodeBytes(workspace);
            const auto nodes = entities.readNodesSorted(snapshot, std::span(ids).first(width),
                [&](size_t bytes) { nodeBytes.set(bytes); });
            require(nodes.size() == width, "scan mutation changed its primary source cardinality");
            ScanBlockMembership before, after;
            before.database_id = after.database_id = snapshot.superblock().database_id;
            before.group_id = after.group_id = groupId;
            before.semantic_generation = snapshot.superblock().generation;
            after.semantic_generation = nextGeneration;
            size_t changed = 0;
            for (size_t i = 0; i < width; ++i) {
                check();
                const ScanNodeChange *change = changed < changes.size() && changes[changed].id == ids[i] ?
                    &changes[changed++] : nullptr;
                const bool exists = bool(nodes[i]);
                if (change) require(change->kind == ScanNodeChangeKind::SNCK_CREATE ? !exists : exists,
                    "scan Node change disagrees with primary source liveness");
                const auto bit = UINT64_C(1) << i;
                utils::OwnedHeapReservation labelBytes(workspace);
                const auto actual = exists ? labels.readNodeLabels(snapshot, *nodes[i],
                    [&](size_t bytes) { labelBytes.set(bytes); }) : std::vector<uint64_t>{};
                check();
                if (exists) { before.live |= bit; appendLabels(before, bit, actual); }
                const bool remains = change ? change->kind != ScanNodeChangeKind::SNCK_DELETE : exists;
                if (remains) {
                    after.live |= bit;
                    appendLabels(after, bit, change ? change->labels : std::span<const uint64_t>(actual));
                }
            }
            require(changed == changes.size(), "scan Node change escaped its representable group");
            ScanMembershipMutationPlan result;
            if (before.live) { before.validate(); result.before = before; }
            if (after.live) { after.validate(); result.after = after; }
            result.live_node_delta = int64_t(std::popcount(after.live)) - int64_t(std::popcount(before.live));
            result.group_delta = int64_t(bool(after.live)) - int64_t(bool(before.live));
            check();
            return result;
        }
    private:
        static void require(bool value, const char *message) {
            if (!value) throw FormatException(FormatError::FERR_INVALID_STATE, message);
        }
        static void appendLabels(ScanBlockMembership &group, uint64_t bit, std::span<const uint64_t> labels) {
            if (group.labels_from_primary) return;
            for (const auto token : labels) {
                auto end = group.labels.begin() + group.label_count;
                const auto found = std::lower_bound(group.labels.begin(), end, token,
                    [](const auto &entry, uint64_t key) { return entry.token < key; });
                if (found == end || found->token != token) {
                    if (group.label_count == ScanBlockMembership::MAX_LABELS) {
                        group.labels_from_primary = true; group.label_count = 0; group.labels = {}; return;
                    }
                    std::move_backward(found, end, end + 1); *found = {token, bit}; ++group.label_count;
                } else found->members |= bit;
            }
        }
    };
}
