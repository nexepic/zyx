from pathlib import Path
p=Path('include/graph/storage/relocatable/property/HybridPropertyReadContext.hpp');s=p.read_text();pos=s.index('    // Reference marking deliberately')
s=s[:pos]+'''    enum class OwnershipStepKind : uint8_t { HOSK_LEGACY_OWNER, HOSK_COLD_OWNER, HOSK_BOUNDARY };
    struct OwnershipStep { EntityPropertyKey owner; OwnershipStepKind kind; };
    // Reference-free ranges still consume one work unit and advance a stable
    // logical cursor. Never walk arbitrary numbers of empty groups under one
    // owner budget, and never use a boundary to skip an ordinary ROW owner.
    [[nodiscard]] std::optional<OwnershipStep> nextOwnershipStepAfter(EntityPropertyOwnerKind kind, uint64_t after,
        const std::function<void(size_t)> &admission = {}) const {
        const auto legacy = nextOwnerAfter(kind, after, admission);
        std::optional<OwnershipStep> result;
        if (legacy) result = OwnershipStep{*legacy, OwnershipStepKind::HOSK_LEGACY_OWNER};
        if (after == RSS_HYBRID_MAX_ENTITY_ID || !hasRoot(kind, 3)) return result;
        const auto binding = bind(kind, 3);
        HybridPropertyGroupDirectoryMap groups(binding.root.index_id, trees_, metadata_, binding);
        const auto first = HybridPropertyDirectoryCodec::groupKey(hybridPropertyGroupFor({kind, after + 1U}));
        static_cast<void>(groups.visitFrom(snapshot_, first, 2, [&](const auto &entry) {
            check();
            const auto id = HybridPropertyDirectoryCodec::decodeGroupKey(entry.key);
            const auto reference = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
            const auto page = fields_.read(snapshot_, reference.page_id);
            const auto group = page.view().readGroup(reference, kind, id, descriptor_->semantic_generation);
            require(group.live != 0, "ownership group has no live boundary");
            const uint64_t baseId = (id - 1U) * 64U;
            const uint64_t last = baseId + 64U - std::countl_zero(group.live);
            if (last <= after) return true;
            // findGroup authenticates format, actual Entity Map and competing
            // ROW at the range boundary; selected reference owners are checked
            // again before their reference is passed to the maintenance sink.
            const auto authenticated = findGroup({kind, last}, admission, nullptr, true, true);
            require(authenticated && *authenticated == group, "ownership group changed within its snapshot");
            if (!group.usesColumnMembership())
                require(!group.memberships || hasRoot(kind, 0), "legacy ownership group has no selector directory");
            OwnershipStep candidate{{kind, last}, OwnershipStepKind::HOSK_BOUNDARY};
            if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
                withColumnMembership(group, [&](const auto &matrix) {
                    uint64_t remaining = matrix.cold_rows.owners;
                    while (remaining) {
                        check(); const auto slot = unsigned(std::countr_zero(remaining));
                        const auto root = matrix.cold_rows.roots[slot];
                        withColdBlock(group, matrix, root, [&](const auto &block, const auto &) {
                            remaining &= ~block.owners;
                            for (unsigned i = 0; i < 64; ++i) {
                                const auto row = block.row(i);
                                const auto ownerId = baseId + i + 1U;
                                if (row && row->large_value_reference && ownerId > after &&
                                    (candidate.kind == OwnershipStepKind::HOSK_BOUNDARY || ownerId < candidate.owner.entity_id))
                                    candidate = {{kind, ownerId}, OwnershipStepKind::HOSK_COLD_OWNER};
                            }
                        });
                    }
                });
            }
            if (!result || candidate.owner.entity_id < result->owner.entity_id) result = candidate;
            return false;
        }, admission, cancellation_));
        return result;
    }
''' +s[pos:];p.write_text(s)
