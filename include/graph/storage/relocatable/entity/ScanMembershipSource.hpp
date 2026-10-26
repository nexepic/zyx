#pragma once

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSetStore.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeReadSession.hpp"
#include "graph/storage/relocatable/entity/ScanBlockMembership.hpp"

namespace graph::storage::scan_candidate_detail {
    class MembershipLabelLimitExceeded final : public std::runtime_error {
    public:
        explicit MembershipLabelLimitExceeded(uint64_t group)
            : std::runtime_error("scan membership group exceeds the supported label union"), group_id(group) {}
        const uint64_t group_id;
    };

    struct MembershipSourceCounts {
        uint64_t nodes = 0;
        uint64_t groups = 0;
    };

    // Offline construction source only: no certificate, directory root or
    // publication authority is returned. A consumer must keep its destination
    // BUILDING through complete integrity and whole-file space admission.
    // Primary Entity and complete LabelSet reads, including propertyless nodes,
    // are the sole source of membership facts; callers cannot supply bitmaps.
    // The workspace must share the caller's construction/query account. A
    // consumer retaining destination state must install its own target account;
    // the source allocation scope is only for bounded temporary construction.
    [[nodiscard]] inline MembershipSourceCounts streamPrimaryMembership(
        RelocatableMetadataStore &metadata, EntityPageStore &entities, EntityLabelSetStore &labels,
        const MetadataSnapshotPin &snapshot,
        const std::shared_ptr<query::execution::MaterializedByteBudget> &workspace,
        const std::function<void()> &checkpoint,
        const std::function<void(const ScanBlockMembership &)> &consume) {
        if (!workspace || workspace->used() > 1024U * 1024U ||
            workspace->available() > 1024U * 1024U - workspace->used() ||
            !checkpoint || !consume || !metadata.ownsSnapshot(snapshot) ||
            !entities.usesMetadataStore(metadata) || !labels.usesMetadataStore(metadata))
            throw std::invalid_argument("scan membership source requires one pinned store and bounded workspace");
        const auto check = [&] {
            checkpoint();
            if (!metadata.ownsSnapshot(snapshot))
                throw std::invalid_argument("scan membership source lost its snapshot pin");
        };
        check();
        query::execution::MaterializedByteReservation frame(workspace.get());
        // Include the fixed session/record frames and conservative synchronous
        // reader/callback frontier. Retained PMR buffers and complete labels
        // have their own actual-capacity admission below.
        frame.set(sizeof(ScanBlockMembership) + sizeof(NativeReadSession) + 16U * 1024U);
        utils::OwnedAllocationScope allocationOwner(workspace);
        NativeReadSession session(workspace);
        ScanBlockMembership group;
        group.database_id = snapshot.superblock().database_id;
        group.semantic_generation = snapshot.superblock().generation;
        MembershipSourceCounts counts;
        uint64_t previous = 0;
        const auto flush = [&] {
            if (!group.live) return;
            check();
            group.validate();
            consume(group); // synchronous borrow; exceptions never replay a prefix
            check();
            ++counts.groups;
        };
        const bool exhausted = entities.visitNodesUntil(snapshot, (std::numeric_limits<size_t>::max)(),
            [&](uint64_t id, const NodeEntityRecord &node) {
                check();
                if (!id || id <= previous)
                    throw FormatException(FormatError::FERR_INVALID_STATE, "scan membership source is not ID ordered");
                previous = id;
                const uint64_t groupId = 1U + (id - 1U) / 64U;
                if (groupId != group.group_id) {
                    flush();
                    group.group_id = groupId; group.live = 0; group.label_count = 0; group.labels_from_primary = false;
                }
                const uint64_t bit = UINT64_C(1) << ((id - 1U) % 64U);
                group.live |= bit;
                query::execution::MaterializedByteReservation labelBytes(workspace.get());
                const auto actual = labels.readNodeLabels(snapshot, node,
                    [&](size_t bytes) { labelBytes.set(bytes); });
                for (const auto token : actual) {
                    if (group.labels_from_primary) break;
                    auto end = group.labels.begin() + group.label_count;
                    const auto found = std::lower_bound(group.labels.begin(), end, token,
                        [](const ScanBlockMembership::Label &entry, uint64_t key) { return entry.token < key; });
                    if (found == end || found->token != token) {
                        if (group.label_count == ScanBlockMembership::MAX_LABELS) {
                            group.labels_from_primary = true; group.label_count = 0; group.labels = {};
                            break;
                        }
                        std::move_backward(found, end, end + 1);
                        *found = {token, bit}; ++group.label_count;
                    } else found->members |= bit;
                }
                ++counts.nodes;
                return true;
            }, &session);
        if (!exhausted)
            throw FormatException(FormatError::FERR_INVALID_STATE, "scan membership source did not exhaust its primary stream");
        flush();
        return counts;
    }
}
