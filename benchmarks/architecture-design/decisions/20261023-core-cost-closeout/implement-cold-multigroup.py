from pathlib import Path
p=Path('include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp')
s=p.read_text().replace('utils::OwnedVector<HybridPropertyGroupDirectoryMap::Key> erased_groups;','utils::OwnedVector<HybridPropertyGroupDirectoryMap::Key> erased_groups;\n        utils::OwnedVector<HybridPropertyGroupDirectoryMap::Entry> groups;')
s=s.replace('erased_groups(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Key>(memory)) {}','erased_groups(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Key>(memory)),\n              groups(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Entry>(memory)) {}')
s=s.replace('    auto &erasedGroupsFor(EntityPropertyOwnerKind kind) {','    auto &groupsFor(EntityPropertyOwnerKind kind) {\n        return kind == group_.owner_kind ? groups_ : other_.groups;\n    }\n    auto &erasedGroupsFor(EntityPropertyOwnerKind kind) {')
start=s.index('    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlocks(')
body=s.index('        using hybrid_property_detail::require;',start)
sig=s[start:body]
wrapper=sig+'''        return prepareColdBlocksImpl(batch, context, blocks, memory, hotChanges, values, cancellation,
            newRoots, demotions, deletedOwners, pendingOwners, HybridPropertyDemotionPreparation(memory));
    }
private:
'''
impl=sig.replace('prepareColdBlocks(', 'prepareColdBlocksImpl(').replace('hotChanges = {}','hotChanges').replace('values = nullptr','values').replace('cancellation = nullptr','cancellation').replace('newRoots = {}','newRoots').replace('demotions = {}','demotions').replace('deletedOwners = {}','deletedOwners').replace('pendingOwners = {})','pendingOwners, HybridPropertyDemotionPreparation result)')
s=s[:start]+wrapper+impl+s[body:]
s=s.replace('        HybridPropertyDemotionPreparation result(memory);\n        uint64_t pendingMask = 0;', '        const bool prepared = result.group_.group_id != 0;\n        uint64_t pendingMask = 0;',1)
s=s.replace('            result = preparePendingOwners(batch, context, context.snapshot_, context.fields_, hotChanges,\n                pendingOwners, memory, cancellation, {}, true);','            if (!prepared) result = preparePendingOwners(batch, context, context.snapshot_, context.fields_, hotChanges,\n                pendingOwners, memory, cancellation, {}, true);',1)
s=s.replace('        } else if (!hotChanges.empty()) result = HybridPropertyDemotionPreparation(', '        } else if (!prepared && !hotChanges.empty()) result = HybridPropertyDemotionPreparation(',1)
s=s.replace('    // Restricted same-batch preparation of one affected existing cold block.', 'public:\n    // Restricted same-batch preparation of one affected existing cold block.',1)
s=s.replace('std::span<const HybridPropertyGroupDirectoryMap::Entry>{};','std::span<const HybridPropertyGroupDirectoryMap::Entry>(other_.groups);',1)
p.write_text(s)
