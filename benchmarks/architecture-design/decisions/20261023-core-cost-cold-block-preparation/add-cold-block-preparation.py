from pathlib import Path
p=Path('include/graph/storage/relocatable/property/HybridPropertyPageImage.hpp');s=p.read_text();a=s.index('    [[nodiscard]] SlottedSlotReference appendExtension')
s=s[:a]+'''    void replaceColdRows(const SlottedSlotReference &reference, std::span<const std::byte> bytes,
        EntityPropertyOwnerKind kind, uint64_t group, const CancellationCheckpoint *cancellation = nullptr) {
        const auto previous = HybridPropertyColdRowBlock::decode(readRecord(reference), kind, group,
            visible_generation_, memory_, cancellation);
        const auto replacement = HybridPropertyColdRowBlock::decode(bytes, kind, group,
            visible_generation_, memory_, cancellation);
        require(replacement.semantic_generation >= previous.semantic_generation,
            "cold ROW replacement moves semantic identity backwards");
        overwriteRecord(reference, bytes);
    }
'''+s[a:];p.write_text(s)
p=Path('include/graph/storage/relocatable/metadata/HybridPropertyDemotionPreparation.hpp');s=p.read_text();a=s.index('    using LargeFieldResolver')
s=s[:a]+'''    // Restricted same-batch preparation of one affected existing cold block.
    // This does not open mode3 publication or authorize Entity Map changes.
    // Growth requiring block/page relocation is refused for the owning layout
    // coordinator to retry with its existing bounded placement planner.
    [[nodiscard]] static HybridPropertyDemotionPreparation prepareColdBlock(MetadataWriteBatch &batch,
        const HybridPropertyReadContext &context, EntityPropertyKey anchor, SlottedSlotReference coldReference,
        std::span<const HybridColdRowInput> changes, std::shared_ptr<std::pmr::memory_resource> memory,
        const LargeValueStore *values = nullptr, const CancellationCheckpoint *cancellation = nullptr) {
        using hybrid_property_detail::require;
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "cold block preparation is disabled");
        require(memory && !changes.empty() && changes.size() <= 64 && context.snapshot_.superblock() == batch.current(),
            "cold block preparation requires one source batch and bounded changes");
        utils::OwnedAllocationScope scope(memory);
        const auto check = [&] { context.check(); if (cancellation) cancellation->check(); };
        check(); utils::OwnedHeapReservation boundary(memory);
        const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        SlottedSlotReference groupReference;
        const auto source = context.findGroup(anchor, admit, &groupReference, true, true);
        boundary.set(0);
        require(source && source->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
            context.isLive(anchor, *source), "cold block preparation requires an actual live remainder owner");
        HybridPropertyDemotionPreparation result(memory); result.group_ = *source;
        // Own the source matrix before another read can reuse snapshot caches.
        const auto matrixSource = [&] {
            const auto page = context.fields_.read(context.snapshot_, groupReference.page_id);
            require(page.view().readGroup(groupReference, source->owner_kind, source->group_id,
                context.descriptor_->semantic_generation) == *source, "cold block source group changed");
            return page.view().readColumnMembership(source->column_membership, *source,
                context.descriptor_->semantic_generation);
        }();
        auto matrix = matrixSource;
        uint64_t affected = 0;
        for (unsigned slot = 0; slot < 64; ++slot)
            if ((matrix.cold_rows.owners & (UINT64_C(1) << slot)) && matrix.cold_rows.roots[slot] == coldReference)
                affected |= UINT64_C(1) << slot;
        require(affected != 0, "cold block source reference has no actual members");
        uint64_t touched = affected;
        for (const auto &row : changes) {
            require(row.owner_slot < 64, "cold change owner exceeds its group");
            const auto bit = UINT64_C(1) << row.owner_slot;
            require((source->live & bit) && (!(matrix.cold_rows.owners & bit) || (affected & bit)),
                "cold change targets a dead owner or another cold block"); touched |= bit;
        }
        for (unsigned slot = 0; slot < 64; ++slot) if (touched & (UINT64_C(1) << slot)) {
            check(); const EntityPropertyKey owner{source->owner_kind, (source->group_id - 1U) * 64U + slot + 1U};
            require(context.findGroup(owner, admit, nullptr, true, true) == source,
                "cold block owner Entity/ROW source changed");
            context.validateNoRemainderSelectors(owner, admit); boundary.set(0);
        }
        // Authenticate actual manifests and global hot-token exclusion before
        // minting replacement images. Unrelated blocks are never decoded.
        const auto inspect = [&](HybridColdRowInput row, Generation generation) {
            const EntityPropertyKey owner{source->owner_kind, (source->group_id - 1U) * 64U + row.owner_slot + 1U};
            HybridPropertyColdRowInspection inspected(context.snapshot_, row, owner, generation,
                std::span(matrix.tokens).first(matrix.column_count), memory, values, cancellation);
            if (!row.large_value_reference) for (size_t i = 0; i < inspected.count(); ++i) {
                const std::array token{inspected.token(i)};
                inspected.visitSelected(token, [](size_t, auto) {});
            }
        };
        uint32_t previousFields = 0;
        context.withColdBlock(*source, matrix, coldReference, [&](const auto &block, const auto &) {
            previousFields = block.fields;
            for (unsigned slot = 0; slot < 64; ++slot)
                if (const auto row = block.row(slot)) { check(); inspect(*row, block.semantic_generation); }
        });
        for (const auto &row : changes) if (!row.root.empty()) { check(); inspect(row, batch.generation()); }
        result.pages_.reserve(groupReference.page_id == coldReference.page_id ? 1 : 2);
        result.pages_.push_back(context.fields_.cloneForMutation(context.snapshot_, groupReference.page_id, batch.generation(), memory));
        if (groupReference.page_id != coldReference.page_id)
            result.pages_.push_back(context.fields_.cloneForMutation(context.snapshot_, coldReference.page_id, batch.generation(), memory));
        auto &coldImage = result.pages_.back();
        const auto bytes = HybridPropertyColdRowBlock::repack(coldImage.readRecord(coldReference), source->owner_kind,
            source->group_id, batch.current().generation, batch.generation(), changes, memory, cancellation);
        uint64_t replacementOwners = 0; uint32_t replacementFields = 0;
        if (!bytes.empty()) {
            const auto replacement = HybridPropertyColdRowBlock::decode(bytes, source->owner_kind, source->group_id,
                batch.generation(), memory, cancellation);
            replacementOwners = replacement.owners; replacementFields = replacement.fields;
            coldImage.replaceColdRows(coldReference, bytes, source->owner_kind, source->group_id, cancellation);
        } else coldImage.erase(coldReference);
        require(previousFields <= matrix.cold_fields &&
            replacementFields <= UINT32_MAX - (matrix.cold_fields - previousFields), "cold block field totals overflow");
        matrix.cold_fields = matrix.cold_fields - previousFields + replacementFields;
        matrix.cold_rows.owners = (matrix.cold_rows.owners & ~affected) | replacementOwners;
        for (unsigned slot = 0; slot < 64; ++slot) {
            const auto bit = UINT64_C(1) << slot;
            if (replacementOwners & bit) matrix.cold_rows.roots[slot] = coldReference;
            else if (affected & bit) matrix.cold_rows.roots[slot] = {};
        }
        matrix.semantic_generation = batch.generation();
        result.group_.semantic_generation = batch.generation(); result.group_.memberships = matrix.totalMemberships();
        matrix.validateInGroup(result.group_, batch.generation(), batch.generation());
        auto &groupImage = result.pages_.front();
        groupImage.replaceColumnMembership(source->column_membership, matrix, *source, result.group_, batch.generation());
        groupImage.replaceGroup(groupReference, result.group_, batch.generation());
        check(); return result;
    }
'''+s[a:];p.write_text(s)
