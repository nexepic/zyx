from pathlib import Path
r=Path('/workspace/zyx');p=r/'src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp';s=p.read_text()
old='''        return preparationSource ? context.findGroup(owner, admission, reference, true, true) :
            context.groupRecord(owner, admission, reference);''';new='''        // This coordinator consumes complete cold field/liveness plans. The
        // generic groupRecord guard remains for unadapted mutation consumers.
        return context.findGroup(owner, admission, reference, true, true);''';assert old in s;s=s.replace(old,new,1)
old='''        if (std::ranges::find(request.pending_owner_additions, replacement.owner) != request.pending_owner_additions.end()) {
            require(!replacement.erase''';new='''        if (std::ranges::find(request.pending_owner_additions, replacement.owner) != request.pending_owner_additions.end()) {
            if (sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
                !context.columnMembership(*sourceGroup).ordinal(replacement.token)) {
                require(!replacement.erase, "pending cold fields cannot erase an absent source value");
                coldChanges.push_back(&replacement); continue;
            }
            require(!replacement.erase''';assert old in s;s=s.replace(old,new,1)
s=s.replace('replacement.token, batch.generation(), request.memory, value);\n            if (insertion)', 'replacement.token, batch.generation(), request.memory, value,\n                sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER);\n            if (insertion)',1)
s=s.replace('RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, bool(preparationSource));','RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, bool(preparationSource) ||\n                sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER);',2)
s=s.replace('const bool coldDeletion = preparationSource && sourceGroup &&','const bool coldDeletion = sourceGroup &&',1)
s=s.replace('''    if (preparationSource && sourceGroup && sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
        request.field_lifecycle)''','''    const bool coldPending = sourceGroup && sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
        !request.pending_owner_additions.empty();
    if (sourceGroup && sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
        request.field_lifecycle)''',1)
old='RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, request.cancellation, true));';new='RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, request.cancellation, true, coldPending));';assert old in s;s=s.replace(old,new,1)
s=s.replace('''    if (!coldChanges.empty() || !demotions.empty() || coldDeletion) {
        require(preparationSource && !multipleGroups &&
            request.pending_owner_additions.empty() && inlineChanges.empty(),
            "cold field preparation requires one source group without pending liveness");''','''    if (!coldChanges.empty() || !demotions.empty() || coldDeletion || coldPending) {
        require(sourceGroup && sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
            !multipleGroups && inlineChanges.empty(), "cold field preparation requires one authenticated source group");''',1)
s=s.replace('''        coldOwners |= structuralOwners | deletedOwners;''','''        uint64_t pendingOwners = 0;
        if (coldPending) for (const auto owner : request.pending_owner_additions)
            pendingOwners |= UINT64_C(1) << hybridPropertySlotFor(owner);
        coldOwners |= structuralOwners | deletedOwners | pendingOwners;''',1)
s=s.replace('''if (!blocks.empty() || !demotions.empty() || coldDeletion) lifecycle.emplace''','''if (!blocks.empty() || !demotions.empty() || coldDeletion || coldPending) lifecycle.emplace''',1)
s=s.replace('''            coldDeletion ? request.owner_deletions : std::span<const EntityPropertyKey>{}));''','''            coldDeletion ? request.owner_deletions : std::span<const EntityPropertyKey>{},
            coldPending ? request.pending_owner_additions : std::span<const EntityPropertyKey>{}));''',1)
p.write_text(s)
p=r/'src/storage/relocatable/metadata/RelocatableMetadataStoreGraphMutation.cpp';s=p.read_text();old='''        std::array<EntityPropertyKey, 64> pendingNativeOwners{};''';new='''        // Only the sealed graph/field coordinator consumes this cold-aware
        // source view. Generic mutation group lookup stays guarded elsewhere.
        const auto topologyGroup = [&](EntityPropertyKey owner, const std::function<void(size_t)> &admission,
            SlottedSlotReference *reference = nullptr) {
            return topologyFields->findGroup(owner, admission, reference, true, true);
        };
        std::array<EntityPropertyKey, 64> pendingNativeOwners{};''';assert old in s;s=s.replace(old,new,1);assert s.count('topologyFields->groupRecord(')==7;s=s.replace('topologyFields->groupRecord(', 'topologyGroup(');p.write_text(s)
