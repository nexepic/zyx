from pathlib import Path
p=Path('/workspace/zyx/include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp');s=p.read_text()
s=s.replace('std::span<const EntityPropertyKey> deletedOwners = {}) {','std::span<const EntityPropertyKey> deletedOwners = {},\n        std::span<const EntityPropertyKey> pendingOwners = {}) {',1)
s=s.replace('context.snapshot_.superblock() == batch.current() && deletedOwners.size() <= 64 &&','context.snapshot_.superblock() == batch.current() && deletedOwners.size() <= 64 && pendingOwners.size() <= 64 &&',1)
s=s.replace('(!blocks.empty() || !demotions.empty() || !deletedOwners.empty())','(!blocks.empty() || !demotions.empty() || !deletedOwners.empty() || !pendingOwners.empty())',1)
s=s.replace('size_t count = demotions.empty() && deletedOwners.empty() ? hotChanges.size() : 0;','size_t count = demotions.empty() && deletedOwners.empty() && pendingOwners.empty() ? hotChanges.size() : 0;',1)
old='''        HybridPropertyDemotionPreparation result(memory);
        if (!hotChanges.empty()) result = HybridPropertyDemotionPreparation(batch, context.snapshot_, context.fields_,
            hotChanges, memory, cancellation);
        utils::OwnedVector<uint32_t> removedTokens'''
new='''        HybridPropertyDemotionPreparation result(memory);
        uint64_t pendingMask = 0;
        if (!pendingOwners.empty()) {
            result = preparePendingOwners(batch, context, context.snapshot_, context.fields_, hotChanges,
                pendingOwners, memory, cancellation, {}, true);
            for (const auto owner : pendingOwners) {
                require(owner.owner_kind == result.group_.owner_kind && hybridPropertyGroupFor(owner) == result.group_.group_id &&
                    std::ranges::find(deletedOwners, owner) == deletedOwners.end(),
                    "cold pending roots crossed groups or repeat a deleted owner");
                pendingMask |= UINT64_C(1) << hybridPropertySlotFor(owner);
            }
        } else if (!hotChanges.empty()) result = HybridPropertyDemotionPreparation(batch, context.snapshot_, context.fields_,
            hotChanges, memory, cancellation);
        utils::OwnedVector<uint32_t> removedTokens'''
assert old in s;s=s.replace(old,new,1)
s=s.replace('result.group_.columns == source.columns && result.group_.live == source.live,','result.group_.columns == source.columns && result.group_.live == (source.live | pendingMask),',1)
s=s.replace('plan->groupReference() == groupReference && !plan->effects().clear_live && !plan->effects().set_live &&','plan->groupReference() == groupReference && !plan->effects().clear_live && !(plan->effects().set_live & ~pendingMask) &&',1)
s=s.replace('cancellation, std::move(result), newRoots, removedTokens);','cancellation, std::move(result), newRoots, removedTokens, pendingMask);',1)
s=s.replace('result.group_.live == source->live, "cold deletion preparation changed liveness prematurely"','result.group_.live == (source->live | pendingMask), "cold deletion preparation changed liveness prematurely"',1)
s=s.replace('std::span<const uint32_t> removedTokens = {}) {','std::span<const uint32_t> removedTokens = {}, uint64_t pendingMask = 0) {',1)
s=s.replace('''            context.isLive(anchor, *source), "cold block preparation requires an actual live remainder owner");''','''            (context.isLive(anchor, *source) || (pendingMask & (UINT64_C(1) << hybridPropertySlotFor(anchor)))),
            "cold block preparation requires an actual live or declared pending remainder owner");
        require(!(source->live & pendingMask), "cold block pending mask repeats a source live owner");''',1)
s=s.replace('result.group_.mode == source->mode && result.group_.live == source->live &&','result.group_.mode == source->mode && result.group_.live == (source->live | pendingMask) &&',1)
s=s.replace('require((source->live & bit) && (!(matrix.cold_rows.owners & bit) || (affected & bit)),','require(((source->live | pendingMask) & bit) && (!(matrix.cold_rows.owners & bit) || (affected & bit)),',1)
p.write_text(s)
