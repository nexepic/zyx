from pathlib import Path
p=Path('include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp');s=p.read_text()
s=s.replace('    utils::OwnedVector<HybridPropertyGroupDirectoryMap::Key> erased_groups_;','    utils::OwnedVector<HybridPropertyGroupDirectoryMap::Key> erased_groups_;\n    utils::OwnedVector<HybridPropertyGroupDirectoryMap::Entry> groups_;',1)
s=s.replace('          erased_groups_(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Key>(memory)), other_(memory) {}','          erased_groups_(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Key>(memory)),\n          groups_(utils::OwnedPmrAllocator<HybridPropertyGroupDirectoryMap::Entry>(memory)), other_(memory) {}',1)
s=s.replace('    // Growth requiring block/page relocation is refused for the owning layout\n    // coordinator to retry with its existing bounded placement planner.','    // Growth uses existing bounded private images and emits group-directory\n    // edits for the owning coordinator to publish in the same atomic batch.',1)
a=s.index('        auto &coldImage = imageFor(coldReference.page_id);');b=s.index('        require(previousFields <= matrix.cold_fields',a)
s=s[:a]+'''        const auto sourceBytes = imageFor(coldReference.page_id).readRecord(coldReference);
        const auto blocks = HybridPropertyColdRowBlock::repackBlocks(sourceBytes, source->owner_kind,
            source->group_id, batch.current().generation, batch.generation(), changes, memory, cancellation);
        // Reserve holders before allocating private IDs. No references into
        // pages_ survive growth. A failure discards this whole metadata batch.
        result.pages_.reserve(result.pages_.size() + blocks.size() + 1U); result.groups_.reserve(1);
        std::array<SlottedSlotReference, 64> replacements{};
        uint64_t replacementOwners = 0; uint32_t replacementFields = 0;
        const auto newPage = [&]() -> HybridPropertyPageImage & {
            const auto id = batch.allocateLogicalPageId();
            result.pages_.push_back(HybridPropertyPageImage::create(id, Generation(1), batch.generation(), memory));
            return result.pages_.back();
        };
        const auto capacityForOverwrite = [](const HybridPropertyPageImage &page, size_t oldBytes) {
            const auto stats = page.stats();
            return size_t(stats.contiguous_free_bytes) + stats.reclaimable_bytes + oldBytes;
        };
        const bool keepSlot = blocks.size() == 1 && blocks.front().size() <=
            capacityForOverwrite(imageFor(coldReference.page_id), sourceBytes.size());
        if (!keepSlot) imageFor(coldReference.page_id).erase(coldReference);
        std::optional<PageId> destination;
        for (const auto &bytes : blocks) {
            check(); const auto replacement = HybridPropertyColdRowBlock::decode(bytes, source->owner_kind, source->group_id,
                batch.generation(), memory, cancellation);
            require(!(replacementOwners & replacement.owners) && replacement.fields <= UINT32_MAX - replacementFields,
                "cold split duplicates owners or overflows field totals");
            replacementOwners |= replacement.owners; replacementFields += replacement.fields;
            SlottedSlotReference locator;
            if (keepSlot) {
                imageFor(coldReference.page_id).replaceColdRows(coldReference, bytes, source->owner_kind, source->group_id, cancellation);
                locator = coldReference;
            } else {
                // Keep growth records off original source pages, leaving room
                // for membership growth and avoiding unrelated source edits.
                if (!destination || imageFor(*destination).stats().contiguous_free_bytes < bytes.size() + RSS_SLOTTED_PAGE_SLOT_BYTES)
                    destination = newPage().pageId();
                locator = imageFor(*destination).appendColdRows(bytes, source->owner_kind, source->group_id, cancellation);
            }
            for (unsigned slot = 0; slot < 64; ++slot)
                if (replacement.owners & (UINT64_C(1) << slot)) replacements[slot] = locator;
        }
'''+s[b:]
s=s.replace('if (replacementOwners & bit) matrix.cold_rows.roots[slot] = coldReference;','if (replacementOwners & bit) matrix.cold_rows.roots[slot] = replacements[slot];',1)
a=s.index('        auto &groupImage = imageFor(groupReference.page_id);\n        groupImage.replaceColumnMembership');b=s.index('        check(); return result;',a)
s=s[:a]+'''        const auto oldMatrixBytes = imageFor(groupReference.page_id).readRecord(source->column_membership).size();
        if (matrix.wireSize() <= capacityForOverwrite(imageFor(groupReference.page_id), oldMatrixBytes)) {
            auto &groupImage = imageFor(groupReference.page_id);
            groupImage.replaceColumnMembership(source->column_membership, matrix, previousPrivateGroup, result.group_, batch.generation());
            groupImage.replaceGroup(groupReference, result.group_, batch.generation());
        } else {
            // Keep the compact group and membership co-located. Existing hot
            // column locators still point to their original COW source pages.
            imageFor(groupReference.page_id).erase(groupReference);
            imageFor(groupReference.page_id).erase(source->column_membership);
            auto &page = newPage();
            result.group_.column_membership = page.appendColumnMembership(matrix, result.group_, batch.generation());
            const auto locator = page.appendGroup(result.group_);
            result.groups_.push_back({HybridPropertyDirectoryCodec::groupKey(source->group_id),
                HybridPropertyDirectoryCodec::locatorValue(locator)});
        }
'''+s[b:]
a=s.index('    [[nodiscard]] std::span<const HybridPropertyGroupDirectoryMap::Key> erasedGroups(')
s=s[:a]+'''    [[nodiscard]] std::span<const HybridPropertyGroupDirectoryMap::Entry> groups(EntityPropertyOwnerKind kind) const & noexcept {
        return kind == group_.owner_kind ? std::span<const HybridPropertyGroupDirectoryMap::Entry>(groups_) :
            std::span<const HybridPropertyGroupDirectoryMap::Entry>{};
    }
    std::span<const HybridPropertyGroupDirectoryMap::Entry> groups(EntityPropertyOwnerKind) const && = delete;
'''+s[a:];p.write_text(s)
p=Path('src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp');s=p.read_text();old='prepareDirectory.template operator()<HybridPropertyGroupDirectoryMap>(3, {}, lifecycle->erasedGroups(kind));';assert old in s;s=s.replace(old,'prepareDirectory.template operator()<HybridPropertyGroupDirectoryMap>(3, lifecycle->groups(kind), lifecycle->erasedGroups(kind));',1);p.write_text(s)
