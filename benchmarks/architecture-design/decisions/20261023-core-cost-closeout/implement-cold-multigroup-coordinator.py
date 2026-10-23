from pathlib import Path
p=Path('src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp');s=p.read_text()
a=s.index('    const bool coldDeletion = sourceGroup &&')
s=s[:a]+'''    struct ColdSourceGroup { HybridPropertyGroupRecord source; SlottedSlotReference reference; };
    utils::OwnedVector<ColdSourceGroup> coldSourceGroups{utils::OwnedPmrAllocator<ColdSourceGroup>(request.memory)};
    const auto collectGroup = [&](EntityPropertyKey owner) {
        SlottedSlotReference reference;
        const auto group = sourceGroupFor(owner, std::ref(admit), &reference); boundary.set(0);
        require(bool(group), "native batch lost an authenticated source group");
        const auto found = std::ranges::find_if(coldSourceGroups, [&](const auto &entry) {
            return entry.source.owner_kind == group->owner_kind && entry.source.group_id == group->group_id;
        });
        if (found == coldSourceGroups.end()) coldSourceGroups.push_back({*group, reference});
        else require(found->source == *group && found->reference == reference, "native batch source group fences disagree");
    };
    for (const auto &cell : replacements) collectGroup(cell.owner);
    for (const auto owner : request.owner_deletions) collectGroup(owner);
    for (const auto owner : request.pending_owner_additions) collectGroup(owner);
    const bool anyColdGroup = std::ranges::any_of(coldSourceGroups, [](const auto &entry) {
        return entry.source.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER;
    });
    require(!anyColdGroup || std::ranges::all_of(coldSourceGroups, [](const auto &entry) {
        return entry.source.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER;
    }), "native batch cannot mix unadapted and cold group lifecycles");
    multipleGroups = coldSourceGroups.size() > 1;
    const bool coldDeletion = anyColdGroup && !request.owner_deletions.empty();
'''+s[s.index('    if (coldDeletion) {',a):]
s=s.replace('''        const auto matrix = context.columnMembership(*sourceGroup);
        // Fold removals''','''        // Fold removals''',1)
s=s.replace('''        deltas.reserve(deltas.size() + matrix.column_count);
        for (const auto owner : request.owner_deletions) {
            require(owner.owner_kind == sourceGroup->owner_kind && hybridPropertyGroupFor(owner) == sourceGroup->group_id,
                "cold owner deletion exceeds one authenticated group");''','''        for (const auto owner : request.owner_deletions) {
            const auto group = sourceGroupFor(owner, std::ref(admit)); boundary.set(0);
            require(bool(group), "cold owner deletion lost its source group");
            const auto matrix = context.columnMembership(*group);''',1)
s=s.replace('const bool coldPending = sourceGroup && sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&','const bool coldPending = anyColdGroup &&',1)
s=s.replace('''        require(sourceGroup && sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
            !multipleGroups && inlineChanges.empty(), "cold field preparation requires one authenticated source group");''','''        require(anyColdGroup && inlineChanges.empty(), "cold field preparation requires authenticated cold source groups");''',1)
a=s.index('        const auto matrix = [&] {',s.index('    if (!coldChanges.empty()'))
b=s.index('        auto finalHot = matrix;',a)
prefix='''        require(changes.size() <= 64U, "cold batch exceeds its original column-plan bound");
        utils::OwnedVector<const HybridPropertyColumnDelta *> compatibleHot{
            utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(request.memory)};
        for (const auto *plan : changes)
            if (plan->action() != HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD) compatibleHot.push_back(plan);
        if (coldPending) lifecycle.emplace(HybridPropertyDemotionPreparation::preparePendingOwners(batch, context,
            source, fields, compatibleHot, request.pending_owner_additions, request.memory, request.cancellation, {}, true));
        else if (!compatibleHot.empty()) lifecycle.emplace(HybridPropertyDemotionPreparation(batch, source, fields,
            compatibleHot, request.memory, request.cancellation, {}, false, {}, true));
        else { lifecycle.emplace(HybridPropertyDemotionPreparation(request.memory)); lifecycle->group_ = *sourceGroup; }
        utils::OwnedVector<EntityPropertyKey> affectedOwners{utils::OwnedPmrAllocator<EntityPropertyKey>(request.memory)};
        const auto affect = [&](EntityPropertyKey owner) {
            if (std::ranges::find(affectedOwners, owner) == affectedOwners.end()) {
                require(affectedOwners.size() < 64U, "cold batch exceeds64 actual changed or migrated owners");
                affectedOwners.push_back(owner);
            }
        };
        for (const auto &cell : replacements) affect(cell.owner);
        size_t emittedMemberships = 0;
        for (const auto &work : coldSourceGroups) {
        const auto sourceGroup = std::optional(work.source);
        const auto sameGroup = [&](EntityPropertyKey owner) { return owner.owner_kind == work.source.owner_kind &&
            hybridPropertyGroupFor(owner) == work.source.group_id; };
        const auto matrix = context.columnMembership(work.source);
        utils::OwnedVector<const HybridPropertyColumnDelta *> groupDemotions{
            utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(request.memory)};
        for (const auto *plan : demotions) if (sameGroup(plan->owner())) {
            groupDemotions.push_back(plan);
            for (const auto &cell : plan->demotedCells()) if (cell.kind != HybridPropertyColumnDelta::CellKind::HCC_MISSING)
                require(++emittedMemberships <= 4096U, "cold batch exceeds4096 emitted demotion memberships");
        }
        utils::OwnedVector<EntityPropertyKey> groupDeleted{utils::OwnedPmrAllocator<EntityPropertyKey>(request.memory)},
            groupPending{utils::OwnedPmrAllocator<EntityPropertyKey>(request.memory)};
        for (const auto owner : request.owner_deletions) if (sameGroup(owner)) groupDeleted.push_back(owner);
        for (const auto owner : request.pending_owner_additions) if (sameGroup(owner)) groupPending.push_back(owner);
'''
s=s[:a]+prefix+s[b:]
s=s.replace('for (const auto *cell : coldChanges) coldOwners |=','for (const auto *cell : coldChanges) if (sameGroup(cell->owner)) coldOwners |=',1)
a=s.index('        utils::OwnedVector<const HybridPropertyColumnDelta *> compatibleHot{',s.index('        auto finalHot = matrix;'))
b=s.index('        for (const auto *plan : demotions) {',a)
s=s[:a]+s[b:]
s=s.replace('        for (const auto *plan : demotions) {\n            finalHot.eraseColumn','        for (const auto *plan : groupDemotions) {\n            finalHot.eraseColumn',1)
s=s.replace('if (coldDeletion) for (const auto owner : request.owner_deletions)','for (const auto owner : groupDeleted)',1).replace('if (coldPending) for (const auto owner : request.pending_owner_additions)','for (const auto owner : groupPending)',1)
s=s.replace('''        coldOwners |= structuralOwners | deletedOwners | pendingOwners;
        struct PreparedRow''','''        coldOwners |= structuralOwners | deletedOwners | pendingOwners;
        for (unsigned slot = 0; slot < 64; ++slot) if (coldOwners & (UINT64_C(1) << slot))
            affect({work.source.owner_kind, (work.source.group_id - 1U) * 64U + slot + 1U});
        struct PreparedRow''',1)
a=s.index('        if (!blocks.empty() || !demotions.empty() || coldDeletion || coldPending) lifecycle.emplace(')
b=s.index('    } else if (onlyCompatibleColumns)',a)
s=s[:a]+'''        if (!blocks.empty() || !groupDemotions.empty() || !groupDeleted.empty() || !groupPending.empty()) {
            HybridPropertyDemotionPreparation local(request.memory);
            local.pages_ = std::move(lifecycle->pages_);
            const auto image = std::ranges::find_if(local.pages_, [&](const auto &page) { return page.pageId() == work.reference.page_id; });
            local.group_ = image == local.pages_.end() ? work.source : image->readGroup(work.reference,
                work.source.owner_kind, work.source.group_id, batch.generation());
            local = HybridPropertyDemotionPreparation::prepareColdBlocksImpl(batch, context, blocks, request.memory,
                {}, &sourceValues, request.cancellation, largeFields, groupDemotions, groupDeleted, groupPending, std::move(local));
            lifecycle->pages_ = std::move(local.pages_);
            auto &upserts = lifecycle->groupsFor(work.source.owner_kind);
            upserts.insert(upserts.end(), local.groups_.begin(), local.groups_.end());
            auto &erased = lifecycle->erasedGroupsFor(work.source.owner_kind);
            erased.insert(erased.end(), local.erased_groups_.begin(), local.erased_groups_.end());
            if (lifecycle->group_.owner_kind == work.source.owner_kind && lifecycle->group_.group_id == work.source.group_id) {
                lifecycle->group_ = local.group_; lifecycle->group_erased_ = local.group_erased_;
            }
        }
        }
        for (const auto kind : {EntityPropertyOwnerKind::EPOK_NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
            auto &upserts = lifecycle->groupsFor(kind); auto &erased = lifecycle->erasedGroupsFor(kind);
            std::ranges::sort(upserts, {}, &HybridPropertyGroupDirectoryMap::Entry::key); std::ranges::sort(erased);
        }
'''+s[b:]
p.write_text(s)
