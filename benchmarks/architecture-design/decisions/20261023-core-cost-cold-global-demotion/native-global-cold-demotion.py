from pathlib import Path
p=Path('/workspace/zyx/src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp');s=p.read_text();a=s.index('bool RelocatableMetadataStore::prepareHybridPropertyColumns');b=s.index('Superblock RelocatableMetadataStore::',a+10);c=s[a:b]
c=c.replace('''replacement.token, batch.generation(), request.memory, replacement.erase ? nullptr : &value);''','''replacement.token, batch.generation(), request.memory, replacement.erase ? nullptr : &value, false,
            RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, bool(preparationSource));''',1)
c=c.replace('''replacement.token, batch.generation(), request.memory, value);
            if (insertion) deltas''','''replacement.token, batch.generation(), request.memory, value, bool(preparationSource));
            if (insertion) deltas''',1)
needle='''    std::optional<HybridPropertyColumnWritePreparation> compatible;'''
pre='''    bool coldPlansMerged = false;
    utils::OwnedVector<const HybridPropertyColumnDelta *> demotions{
        utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(request.memory)};
    if (preparationSource && sourceGroup && sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
        request.field_lifecycle) {
        std::sort(changes.begin(), changes.end(), [](auto *left, auto *right) {
            return left->columnReference() < right->columnReference();
        });
        merged.reserve(changes.size());
        for (size_t begin = 0; begin < changes.size();) {
            size_t end = begin + 1;
            while (end < changes.size() && changes[end]->columnReference() == changes[begin]->columnReference()) ++end;
            merged.push_back(HybridPropertyColumnDelta::mergeExistingColumnChanges(
                std::span(changes).subspan(begin, end - begin), request.memory,
                RSS_HYBRID_EXTENSION_MAX_RECORD_BYTES, request.cancellation, true));
            begin = end;
        }
        changes.clear();
        for (const auto &delta : merged) {
            changes.push_back(&delta);
            if (delta.action() == HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD) demotions.push_back(&delta);
        }
        coldPlansMerged = true;
    }
'''
c=c.replace(needle,pre+needle,1)
# Preserve existing non-cold behavior; the private cold path is already merged.
start=c.index('    if (!coldChanges.empty()) {');c=c[:start]+c[start:].replace('    if (!coldChanges.empty()) {','    if (!coldChanges.empty() || !demotions.empty()) {',1)
# Wrap the three existing merge blocks without changing their default behavior.
scan=c.index('    if (!coldChanges.empty() || !demotions.empty()) {')
while True:
 try: begin=c.index('        std::sort(changes.begin(), changes.end()',scan)
 except ValueError: break
 end=c.index('        changes.clear(); for (const auto &delta : merged) changes.push_back(&delta);',begin)+len('        changes.clear(); for (const auto &delta : merged) changes.push_back(&delta);')
 # Only lifecycle merge blocks, all occur before extension handling.
 if begin>c.index('    if (!inlineChanges.empty()) {'):break
 c=c[:begin]+'        if (!coldPlansMerged) {\n'+c[begin:end]+'\n        }'+c[end:];scan=end+len('        if (!coldPlansMerged) {\n')+len('\n        }')
# The source matrix remains authoritative for original cold locations, while
# final hot identity excludes every globally demoted token.
needle='''        std::sort(coldChanges.begin(), coldChanges.end(), [](auto *left, auto *right) {
            return left->owner.entity_id < right->owner.entity_id ||
                (left->owner == right->owner && left->token < right->token);
        });'''
replacement='''        auto finalHot = matrix;
        uint64_t coldOwners = 0, structuralOwners = 0;
        for (const auto *cell : coldChanges) coldOwners |= UINT64_C(1) << hybridPropertySlotFor(cell->owner);
        utils::OwnedVector<const HybridPropertyColumnDelta *> compatibleHot{
            utils::OwnedPmrAllocator<const HybridPropertyColumnDelta *>(request.memory)};
        for (const auto *plan : changes)
            if (plan->action() != HybridPropertyColumnDelta::Action::HCD_DEMOTE_FIELD) compatibleHot.push_back(plan);
        for (const auto *plan : demotions) {
            finalHot.eraseColumn(plan->sourceColumn().identity.token);
            structuralOwners |= plan->sourceColumn().presence;
            for (unsigned slot = 0; slot < 64; ++slot)
                if (plan->demotedCells()[slot].kind != HybridPropertyColumnDelta::CellKind::HCC_MISSING)
                    structuralOwners |= UINT64_C(1) << slot;
        }
        coldOwners |= structuralOwners;'''
assert needle in c;c=c.replace(needle,replacement,1)
needle='''        for (size_t begin = 0; begin < coldChanges.size();) {
            check(); const auto owner = coldChanges[begin]->owner; size_t end = begin + 1;
            while (end < coldChanges.size() && coldChanges[end]->owner == owner) ++end;'''
replacement='''        for (unsigned slot = 0; slot < 64; ++slot) {
            const auto ownerBit = UINT64_C(1) << slot;
            if (!(coldOwners & ownerBit)) continue;
            check(); const EntityPropertyKey owner{sourceGroup->owner_kind, (sourceGroup->group_id - 1U) * 64U + slot + 1U};'''
assert needle in c;c=c.replace(needle,replacement,1)
# Scope these edits to final-root production only.
a1=c.index('        for (unsigned slot = 0; slot < 64; ++slot) {\n            const auto ownerBit');b1=c.index('        std::sort(rows.begin()',a1);part=c[a1:b1]
part=part.replace('''matrix.tokens.begin(), matrix.tokens.begin() + matrix.column_count''','''finalHot.tokens.begin(), finalHot.tokens.begin() + finalHot.column_count''')
part=part.replace('''            scalars.reserve(end - begin); bool changed = false;
            for (size_t i = begin; i < end; ++i) {
                check(); const auto &cell = *coldChanges[i];''','''            scalars.reserve(replacements.size()); bool changed = bool(structuralOwners & ownerBit);
            for (const auto &cell : replacements) {
                if (cell.owner != owner || std::binary_search(finalHot.tokens.begin(),
                    finalHot.tokens.begin() + finalHot.column_count, cell.token)) continue;
                check();''')
part=part.replace('''            if (!changed) { begin = end; continue; }''','''            if (!changed || (entries.empty() && !(matrix.cold_rows.owners & ownerBit))) continue;''')
part=part.replace('''            begin = end;
''','')
c=c[:a1]+part+c[b1:]
c=c.replace('''        if (!blocks.empty()) lifecycle.emplace(HybridPropertyDemotionPreparation::prepareColdBlocks(batch,
            context, blocks, request.memory, changes, &sourceValues, request.cancellation, largeFields));''','''        if (!blocks.empty() || !demotions.empty()) lifecycle.emplace(HybridPropertyDemotionPreparation::prepareColdBlocks(batch,
            context, blocks, request.memory, compatibleHot, &sourceValues, request.cancellation, largeFields, demotions));''',1)
s=s[:a]+c+s[b:];p.write_text(s)
