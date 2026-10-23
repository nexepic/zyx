from pathlib import Path
p=Path('include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp');s=p.read_text()
a=s.index('        using hybrid_property_detail::require;',s.index('static HybridPropertyDemotionPreparation prepareColdBlock'))
s=s[:a]+'''        return prepareColdBlockImpl(batch, context, anchor, coldReference, changes, memory, values,
            cancellation, HybridPropertyDemotionPreparation(memory));
    }
    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlockWithHot(MetadataWriteBatch &batch,
        const HybridPropertyReadContext &context, std::span<const HybridPropertyColumnDelta *const> hotChanges,
        EntityPropertyKey anchor, SlottedSlotReference coldReference, std::span<const HybridColdRowInput> changes,
        std::shared_ptr<std::pmr::memory_resource> memory, const LargeValueStore *values = nullptr,
        const CancellationCheckpoint *cancellation = nullptr) {
        // The original constructor authenticates every hot plan's database,
        // source/target generation, group and actual source column bytes.
        HybridPropertyDemotionPreparation prepared(batch, context.snapshot_, context.fields_, hotChanges, memory, cancellation);
        return prepareColdBlockImpl(batch, context, anchor, coldReference, changes, memory, values,
            cancellation, std::move(prepared));
    }
private:
    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlockImpl(MetadataWriteBatch &batch,
        const HybridPropertyReadContext &context, EntityPropertyKey anchor, SlottedSlotReference coldReference,
        std::span<const HybridColdRowInput> changes, const std::shared_ptr<std::pmr::memory_resource> &memory,
        const LargeValueStore *values, const CancellationCheckpoint *cancellation, HybridPropertyDemotionPreparation result) {
'''+s[a:]
s=s.replace('        HybridPropertyDemotionPreparation result(memory); result.group_ = *source;','''        if (result.group_.group_id) {
            require(result.group_.owner_kind == source->owner_kind && result.group_.group_id == source->group_id &&
                result.group_.mode == source->mode && result.group_.live == source->live &&
                result.group_.columns == source->columns && result.group_.column_membership == source->column_membership,
                "cold/hot merge changes source group identity, liveness or hot ownership");
        } else result.group_ = *source;
        const auto previousPrivateGroup = result.group_;''',1)
s=s.replace('        auto matrix = matrixSource;', '''        // Reuse existing hot COW images. Never create a second independent
        // source copy that could overwrite hot values/presence on publication.
        result.pages_.reserve(result.pages_.size() + 2);
        const auto imageFor = [&](PageId id) -> HybridPropertyPageImage & {
            for (auto &page : result.pages_) if (page.pageId() == id) return page;
            result.pages_.push_back(context.fields_.cloneForMutation(context.snapshot_, id, batch.generation(), memory));
            return result.pages_.back();
        };
        (void)imageFor(groupReference.page_id); (void)imageFor(coldReference.page_id);
        auto matrix = imageFor(groupReference.page_id).readColumnMembership(source->column_membership,
            previousPrivateGroup, batch.generation());
        require(matrix.cold_fields == matrixSource.cold_fields && matrix.cold_rows == matrixSource.cold_rows &&
            matrix.tokens == matrixSource.tokens && matrix.locators == matrixSource.locators,
            "compatible hot preparation changed cold binding or global hot identity");''',1)
s=s.replace('context.withColdBlock(*source, matrix, coldReference,', 'context.withColdBlock(*source, matrixSource, coldReference,',1)
a=s.index('        result.pages_.reserve(groupReference.page_id == coldReference.page_id ? 1 : 2);');b=s.index('        const auto bytes =',a)
s=s[:a]+'''        auto &coldImage = imageFor(coldReference.page_id);
'''+s[b:]
s=s.replace('        auto &groupImage = result.pages_.front();\n        groupImage.replaceColumnMembership(source->column_membership, matrix, *source, result.group_, batch.generation());','        auto &groupImage = imageFor(groupReference.page_id);\n        groupImage.replaceColumnMembership(source->column_membership, matrix, previousPrivateGroup, result.group_, batch.generation());',1)
s=s.replace('    using LargeFieldResolver =', 'public:\n    using LargeFieldResolver =',1)
p.write_text(s)
