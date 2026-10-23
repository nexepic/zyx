from pathlib import Path
p=Path('include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp');s=p.read_text();a=s.index('    // Restricted same-batch preparation')
s=s[:a]+'''    struct ColdBlockChange {
        EntityPropertyKey anchor;
        SlottedSlotReference reference;
        std::span<const HybridColdRowInput> rows;
    };
    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlocks(MetadataWriteBatch &batch,
        const HybridPropertyReadContext &context, std::span<const ColdBlockChange> blocks,
        std::shared_ptr<std::pmr::memory_resource> memory,
        std::span<const HybridPropertyColumnDelta *const> hotChanges = {},
        const LargeValueStore *values = nullptr, const CancellationCheckpoint *cancellation = nullptr) {
        using hybrid_property_detail::require;
        require(!blocks.empty() && blocks.size() <= 64 && hotChanges.size() <= 64,
            "cold batch requires bounded blocks and hot plans");
        size_t count = hotChanges.size(); uint64_t changed = 0;
        for (size_t i = 0; i < blocks.size(); ++i) {
            const auto &block = blocks[i];
            require(block.anchor.isValid() && block.reference.isValid() && !block.rows.empty() &&
                block.rows.size() <= 64U - count && block.anchor.owner_kind == blocks.front().anchor.owner_kind &&
                hybridPropertyGroupFor(block.anchor) == hybridPropertyGroupFor(blocks.front().anchor),
                "cold batch changes exceed one group or original64change bound"); count += block.rows.size();
            for (size_t j = 0; j < i; ++j)
                require(block.reference != blocks[j].reference, "cold batch repeats a source block");
            for (const auto &row : block.rows) {
                require(row.owner_slot < 64, "cold batch owner slot exceeds its group");
                const auto bit = UINT64_C(1) << row.owner_slot;
                require(!(changed & bit), "cold batch repeats an owner root"); changed |= bit;
            }
        }
        HybridPropertyDemotionPreparation result(memory);
        if (!hotChanges.empty()) result = HybridPropertyDemotionPreparation(batch, context.snapshot_, context.fields_,
            hotChanges, memory, cancellation);
        for (const auto &block : blocks)
            result = prepareColdBlockImpl(batch, context, block.anchor, block.reference, block.rows, memory, values,
                cancellation, std::move(result));
        return result;
    }
'''+s[a:]
s=s.replace('        SlottedSlotReference groupReference;\n        const auto source = context.findGroup(anchor, admit, &groupReference, true, true);','        SlottedSlotReference sourceGroupReference;\n        const auto source = context.findGroup(anchor, admit, &sourceGroupReference, true, true);',1)
needle='        if (result.group_.group_id) {';s=s.replace(needle,'''        auto groupReference = sourceGroupReference;
        if (!result.groups_.empty()) {
            require(result.groups_.size() == 1 && result.groups_.front().key == HybridPropertyDirectoryCodec::groupKey(source->group_id),
                "cold batch private group relocation has a foreign key");
            groupReference = HybridPropertyDirectoryCodec::decodeLocator(result.groups_.front().value);
        }
'''+needle,1)
s=s.replace('result.group_.columns == source->columns && result.group_.column_membership == source->column_membership,','result.group_.columns == source->columns && result.group_.column_membership.page_id == groupReference.page_id,',1)
s=s.replace('context.fields_.read(context.snapshot_, groupReference.page_id);\n            require(page.view().readGroup(groupReference, source->owner_kind', 'context.fields_.read(context.snapshot_, sourceGroupReference.page_id);\n            require(page.view().readGroup(sourceGroupReference, source->owner_kind',1)
s=s.replace('auto matrix = imageFor(groupReference.page_id).readColumnMembership(source->column_membership,','require(imageFor(groupReference.page_id).readGroup(groupReference, source->owner_kind, source->group_id,\n            batch.generation()) == previousPrivateGroup, "cold batch lost its current private group");\n        auto matrix = imageFor(groupReference.page_id).readColumnMembership(previousPrivateGroup.column_membership,',1)
s=s.replace('require(matrix.cold_fields == matrixSource.cold_fields && matrix.cold_rows == matrixSource.cold_rows &&\n            matrix.tokens == matrixSource.tokens', 'require(matrix.tokens == matrixSource.tokens',1)
s=s.replace('if ((matrix.cold_rows.owners & (UINT64_C(1) << slot)) && matrix.cold_rows.roots[slot] == coldReference)\n                affected |= UINT64_C(1) << slot;', '''if ((matrixSource.cold_rows.owners & (UINT64_C(1) << slot)) && matrixSource.cold_rows.roots[slot] == coldReference) {
                require((matrix.cold_rows.owners & (UINT64_C(1) << slot)) && matrix.cold_rows.roots[slot] == coldReference,
                    "cold batch already modified this source owner"); affected |= UINT64_C(1) << slot;
            }''',1)
s=s.replace('imageFor(groupReference.page_id).readRecord(source->column_membership).size()', 'imageFor(groupReference.page_id).readRecord(previousPrivateGroup.column_membership).size()',1)
s=s.replace('groupImage.replaceColumnMembership(source->column_membership, matrix, previousPrivateGroup,', 'groupImage.replaceColumnMembership(previousPrivateGroup.column_membership, matrix, previousPrivateGroup,',1)
s=s.replace('imageFor(groupReference.page_id).erase(source->column_membership);','imageFor(groupReference.page_id).erase(previousPrivateGroup.column_membership);',1)
s=s.replace('            result.groups_.push_back({HybridPropertyDirectoryCodec::groupKey(source->group_id),\n                HybridPropertyDirectoryCodec::locatorValue(locator)});','''            const HybridPropertyGroupDirectoryMap::Entry entry{HybridPropertyDirectoryCodec::groupKey(source->group_id),
                HybridPropertyDirectoryCodec::locatorValue(locator)};
            if (result.groups_.empty()) result.groups_.push_back(entry); else result.groups_.front() = entry;''',1)
p.write_text(s)
