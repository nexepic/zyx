from pathlib import Path
r=Path('/workspace/zyx');p=r/'include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp';s=p.read_text()
s=s.replace('std::span<const HybridPropertyColumnDelta *const> demotions = {}) {','std::span<const HybridPropertyColumnDelta *const> demotions = {},\n        std::span<const EntityPropertyKey> deletedOwners = {}) {',1)
s=s.replace('require((!blocks.empty() || !demotions.empty()) && blocks.size() <= 64 && demotions.size() <= 64 && hotChanges.size() <= 64U - demotions.size(),','require(memory && context.snapshot_.superblock() == batch.current() && deletedOwners.size() <= 64 &&\n            (!blocks.empty() || !demotions.empty() || !deletedOwners.empty()) &&\n            blocks.size() <= 64 && demotions.size() <= 64 && hotChanges.size() <= 64U - demotions.size(),',1)
s=s.replace('size_t count = demotions.empty() ? hotChanges.size() : 0; uint64_t changed = 0;','size_t count = demotions.empty() && deletedOwners.empty() ? hotChanges.size() : 0; uint64_t changed = 0;',1)
old='''        return result;
    }
    // Restricted same-batch preparation'''
new='''        if (!deletedOwners.empty()) {
            // Remove liveness only after every hot/cold membership has been
            // removed in these same private images. No implicit field clearing
            // may hide an omitted removal or a stale source Entity/ROW witness.
            utils::OwnedAllocationScope scope(memory);
            utils::OwnedHeapReservation boundary(memory);
            const SelectedPropertyReadAdmission admit = [&](size_t bytes) { boundary.set(bytes); };
            const auto check = [&] { context.check(); if (cancellation) cancellation->check(); };
            uint64_t deleted = 0; SlottedSlotReference sourceReference;
            const auto source = context.findGroup(deletedOwners.front(), admit, &sourceReference, true, true);
            boundary.set(0);
            require(source && source->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER,
                "cold deletion requires its actual original source group");
            for (const auto owner : deletedOwners) {
                check(); require(owner.isValid() && owner.owner_kind == source->owner_kind &&
                    hybridPropertyGroupFor(owner) == source->group_id,
                    "cold deletion crossed its authenticated source group");
                const auto bit = UINT64_C(1) << hybridPropertySlotFor(owner);
                require((source->live & bit) && !(deleted & bit) &&
                    context.findGroup(owner, admit, nullptr, true, true) == source,
                    "cold deletion has a dead, repeated or foreign source owner");
                context.requireNoLegacySelector(owner, admit); boundary.set(0); deleted |= bit;
            }
            if (!result.group_.group_id) result.group_ = *source;
            require(result.group_.owner_kind == source->owner_kind && result.group_.group_id == source->group_id &&
                result.group_.live == source->live, "cold deletion preparation changed liveness prematurely");
            result.pages_.reserve(result.pages_.size() + 2U);
            const auto image = [&](PageId id) -> HybridPropertyPageImage & {
                for (auto &page : result.pages_) if (page.pageId() == id) return page;
                result.pages_.push_back(context.fields_.cloneForMutation(context.snapshot_, id, batch.generation(), memory));
                return result.pages_.back();
            };
            auto reference = sourceReference;
            if (!result.groups_.empty()) {
                require(result.groups_.size() == 1 && result.groups_.front().key ==
                    HybridPropertyDirectoryCodec::groupKey(source->group_id), "cold deletion has unrelated group relocation");
                reference = HybridPropertyDirectoryCodec::decodeLocator(result.groups_.front().value);
            }
            const auto previous = result.group_;
            require(image(reference.page_id).readGroup(reference, previous.owner_kind, previous.group_id,
                batch.generation()) == previous, "cold deletion private group bytes changed");
            auto matrix = image(previous.column_membership.page_id).readColumnMembership(previous.column_membership,
                previous, batch.generation());
            require(!(matrix.cold_rows.owners & deleted), "cold deletion omitted an actual cold root removal");
            for (unsigned i = 0; i < matrix.column_count; ++i)
                require(!(matrix.presence(matrix.tokens[i]) & deleted), "cold deletion omitted an actual hot membership removal");
            result.group_.live &= ~deleted; result.group_.semantic_generation = batch.generation();
            if (result.group_.live) {
                matrix.live = result.group_.live; matrix.semantic_generation = batch.generation();
                image(previous.column_membership.page_id).replaceColumnMembership(previous.column_membership, matrix,
                    previous, result.group_, batch.generation());
                image(reference.page_id).replaceGroup(reference, result.group_, batch.generation());
            } else {
                require(!result.group_.columns && !result.group_.memberships && !matrix.totalMemberships(),
                    "final cold group deletion retains fields or columns");
                image(previous.column_membership.page_id).erase(previous.column_membership);
                image(reference.page_id).erase(reference);
                result.groups_.clear(); result.group_erased_ = true;
                result.erased_groups_.push_back(HybridPropertyDirectoryCodec::groupKey(source->group_id));
            }
            check();
        }
        return result;
    }
    // Restricted same-batch preparation'''
assert old in s;s=s.replace(old,new,1);p.write_text(s)
p=r/'src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp';s=p.read_text()
s=s.replace('request.owner_deletions.empty() && request.pending_owner_additions.empty()),','request.pending_owner_additions.empty()),',1)
s=s.replace('sourceGroup = context.groupRecord(request.owner_deletions.front(), std::ref(admit), &groupReference);','sourceGroup = sourceGroupFor(request.owner_deletions.front(), std::ref(admit), &groupReference);',1)
old='''    for (const auto &delta : deltas) changes.push_back(&delta);'''
new='''    const bool coldDeletion = preparationSource && sourceGroup &&
        sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER && !request.owner_deletions.empty();
    if (coldDeletion) {
        const auto matrix = context.columnMembership(*sourceGroup);
        // Fold removals immediately per column: at most64 full column plans,
        // never4096 simultaneous source-based images or borrowed temporaries.
        deltas.reserve(deltas.size() + matrix.column_count);
        for (const auto owner : request.owner_deletions) {
            require(owner.owner_kind == sourceGroup->owner_kind && hybridPropertyGroupFor(owner) == sourceGroup->group_id,
                "cold owner deletion exceeds one authenticated group");
            for (unsigned i = 0; i < matrix.column_count; ++i) {
                const auto token = matrix.tokens[i]; if (!matrix.contains(owner, token)) continue;
                auto removal = HybridPropertyColumnDelta::planExistingColumn(context, source, owner, token,
                    batch.generation(), request.memory, nullptr, false, RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, true);
                require(bool(removal), "cold owner deletion lost its original column member");
                const auto prior = std::find_if(deltas.begin(), deltas.end(), [&](const auto &plan) {
                    return plan.columnReference() == removal->columnReference();
                });
                if (prior == deltas.end()) deltas.push_back(std::move(*removal));
                else {
                    const std::array<const HybridPropertyColumnDelta *, 2> input{&*prior, &*removal};
                    *prior = HybridPropertyColumnDelta::mergeExistingColumnChanges(input, request.memory,
                        RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, request.cancellation, true);
                }
            }
        }
    }
    for (const auto &delta : deltas) changes.push_back(&delta);'''
assert old in s;s=s.replace(old,new,1)
s=s.replace('if (!coldChanges.empty() || !demotions.empty()) {\n        require(preparationSource && !multipleGroups && request.owner_deletions.empty() &&','if (!coldChanges.empty() || !demotions.empty() || coldDeletion) {\n        require(preparationSource && !multipleGroups &&',1)
s=s.replace('''        coldOwners |= structuralOwners;
        struct PreparedRow''','''        uint64_t deletedOwners = 0;
        if (coldDeletion) for (const auto owner : request.owner_deletions)
            deletedOwners |= UINT64_C(1) << hybridPropertySlotFor(owner);
        coldOwners |= structuralOwners | deletedOwners;
        struct PreparedRow''',1)
s=s.replace('''            if (!changed || (entries.empty() && !(matrix.cold_rows.owners & ownerBit))) continue;''','''            if (deletedOwners & ownerBit) { entries.clear(); changed = true; }
            if (!changed || (entries.empty() && !(matrix.cold_rows.owners & ownerBit))) continue;''',1)
s=s.replace('''        if (!blocks.empty() || !demotions.empty()) lifecycle.emplace(HybridPropertyDemotionPreparation::prepareColdBlocks(batch,
            context, blocks, request.memory, compatibleHot, &sourceValues, request.cancellation, largeFields, demotions));''','''        if (!blocks.empty() || !demotions.empty() || coldDeletion) lifecycle.emplace(HybridPropertyDemotionPreparation::prepareColdBlocks(batch,
            context, blocks, request.memory, compatibleHot, &sourceValues, request.cancellation, largeFields, demotions,
            coldDeletion ? request.owner_deletions : std::span<const EntityPropertyKey>{}));''',1)
p.write_text(s)
