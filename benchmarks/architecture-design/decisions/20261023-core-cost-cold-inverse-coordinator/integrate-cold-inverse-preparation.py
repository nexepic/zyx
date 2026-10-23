from pathlib import Path
r=Path('/workspace/zyx');p=r/'include/graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp';s=p.read_text();a=s.index('        [[nodiscard]] Superblock publishHybridPropertyRows');b=s.index('\n\t\t[[nodiscard]] MetadataWriteGuard',a);c=s[a:b].replace('''const MetadataPublishOptions &options);''','''const MetadataPublishOptions &options, HybridPropertyReadContext *preparationSource = nullptr);''');s=s[:a]+c+s[b:];p.write_text(s)
p=r/'src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp';s=p.read_text();a=s.index('Superblock RelocatableMetadataStore::publishHybridPropertyRows');c=s[a:];c=c.replace('''const MetadataPublishOptions &options) {''','''const MetadataPublishOptions &options, HybridPropertyReadContext *preparationSource) {''',1)
old='''    HybridPropertyReadContext context(io_, sourcePages, *this, source, conversion.cancellation);
    // Retain the original public mutation guard until cold inverse directory,
    // ownership and retirement installation are complete. Read-only projection
    // alone must not enable publication of this new mode.
    static_cast<void>(context.groupRecord({conversion.owner_kind,
        (conversion.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + 1U}));'''
new='''    HybridPropertyReadContext durableSource(io_, sourcePages, *this, source, conversion.cancellation);
    auto &context = preparationSource ? *preparationSource : durableSource;
    require(!preparationSource || (context.usesSnapshot(source) && &context.metadata_ == this &&
        context.source_io_ == io_ && !conversion.publish_candidate && context.descriptor_),
        "private inverse requires its exact source pin/file and cannot publish");
    if (preparationSource) descriptor = *context.descriptor_;
    // Normal publication retains its original guard until complete cold inverse
    // installation/lifecycle acceptance. Private candidates reach only planning.
    if (!preparationSource) static_cast<void>(context.groupRecord({conversion.owner_kind,
        (conversion.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + 1U}));''';assert old in c;c=c.replace(old,new,1)
c=c.replace('''group.mode != HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP''','''!group.usesColumnMembership()''').replace('''group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP''','''group.usesColumnMembership()''')
c=c.replace('''    references.reserve(group.columns + 2U);''','''    references.reserve(group.columns + 2U + (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER ? 64U : 0U));
    std::optional<HybridPropertyColumnMembership> sourceMatrix;
    std::array<const PropertyCollectionRoot *, 64> projected{};
    for (const auto &row : projection.rows()) projected[hybridPropertySlotFor({conversion.owner_kind, row.owner_entity_id})] = &row;''',1)
c=c.replace('''        static_cast<void>(matrixPage.view().readColumnMembership(group.column_membership, group, descriptor.semantic_generation));''','''        sourceMatrix = matrixPage.view().readColumnMembership(group.column_membership, group, descriptor.semantic_generation);''',1)
needle='''    size_t actualMemberships = 0;'''
insert='''    const auto selectedSource = [&](EntityPropertyKey owner, uint32_t token,
        const std::function<void(size_t)> &admission) -> std::optional<HybridPropertySource> {
        if (group.mode != HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER)
            return context.selector(group, owner, token, admission);
        require(sourceMatrix && owner.owner_kind == group.owner_kind && hybridPropertyGroupFor(owner) == group.group_id,
            "cold inverse selection escaped its authenticated group");
        const auto *row = projected[hybridPropertySlotFor(owner)];
        if (!row) return {};
        const auto field = std::lower_bound(row->entries.begin(), row->entries.end(), token,
            [](const auto &entry, uint32_t selected) { return entry.key_token < selected; });
        if (field == row->entries.end() || field->key_token != token) return {};
        if (sourceMatrix->ordinal(token)) return sourceMatrix->contains(owner, token) ?
            std::optional(HybridPropertySource::HPS_COLUMN) : std::nullopt;
        return (sourceMatrix->cold_rows.owners & (UINT64_C(1) << hybridPropertySlotFor(owner))) ?
            std::optional(HybridPropertySource::HPS_COLD_ROW) : std::nullopt;
    };
    if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER)
        for (unsigned slot = 0; slot < 64; ++slot) {
            const EntityPropertyKey owner{conversion.owner_kind, (conversion.group_id - 1U) * 64U + slot + 1U};
            if (!owner.isValid()) continue;
            context.requireNoLegacySelector(owner, admit); boundary.set(0);
        }
'''
assert needle in c;c=c.replace(needle,insert+needle,1)
c=c.replace('''context.selector(group, owner, field.key_token, std::ref(admit))''','''selectedSource(owner, field.key_token, std::ref(admit))''',1)
c=c.replace('''context.selector(group, owner, token, [&](size_t bytes) { selectorBoundary.set(bytes); })''','''selectedSource(owner, token, [&](size_t bytes) { selectorBoundary.set(bytes); })''',1)
needle='''    if (descriptor.roots[base + 1].page_id.isValid()) {'''
insert='''    if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
        uint64_t seen = 0;
        for (unsigned slot = 0; slot < 64; ++slot) {
            const auto bit = UINT64_C(1) << slot;
            if (!(sourceMatrix->cold_rows.owners & bit) || (seen & bit)) continue;
            const auto reference = sourceMatrix->cold_rows.roots[slot];
            context.withColdBlock(group, *sourceMatrix, reference, [&](const auto &block, const auto &) {
                seen |= block.owners; bodyMemberships += block.fields;
            });
            references.push_back(reference);
        }
        require(seen == sourceMatrix->cold_rows.owners, "inverse cold source census lost actual owners");
    }
'''
assert needle in c;c=c.replace(needle,insert+needle,1)
needle='''        } else if (row.entries.size() == 1) {'''
insert='''        } else if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
            const EntityPropertyKey owner{conversion.owner_kind, row.owner_entity_id};
            const auto slot = hybridPropertySlotFor(owner);
            const bool hasHot = std::ranges::any_of(sourceMatrix->owners[slot], [](uint64_t word) { return word != 0; });
            if (!hasHot && (sourceMatrix->cold_rows.owners & (UINT64_C(1) << slot)))
                context.withColdBlock(group, *sourceMatrix, sourceMatrix->cold_rows.roots[slot],
                    [&](const auto &block, const auto &) {
                        const auto cold = block.row(slot);
                        require(cold && cold->field_count == row.entries.size(), "inverse complete cold root lost its actual fields");
                        if (!cold->large_value_reference) return;
                        HybridPropertyColdRowInspection inspection(source, *cold, owner, block.semantic_generation,
                            std::span(sourceMatrix->tokens).first(sourceMatrix->column_count), conversion.memory,
                            &largeValues, conversion.cancellation);
                        require(bool(inspection.reference()), "inverse complete cold carrier has no authenticated reference");
                        const auto bytes = EntityPropertyLargeValueReferenceCodec::encode(*inspection.reference());
                        encoded.assign(bytes.begin(), bytes.end());
                    });
        } else if (row.entries.size() == 1) {'''
assert needle in c;c=c.replace(needle,insert,1)
s=s[:a]+c;p.write_text(s)
p=r/'tests/include/storage/relocatable/property/HybridPropertyNativeWriteTestAccess.hpp';s=p.read_text();a=s.rindex('};');s=s[:a]+'''    static void prepareInverse(RelocatableMetadataStore &metadata, const MetadataSnapshotPin &source,
        HybridPropertyReadContext &context, const NativeGraphHybridPropertyConversion &conversion) {
        utils::OwnedHeapReservation sourceStoreCredit(conversion.memory, 16U * 1024U);
        SlottedPagePool pages(metadata.io_, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        BTreePageStore trees(pages, metadata);
        LargeValueStore values(metadata.io_, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        EntityPageStore entities(metadata.io_, metadata, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
        auto writer = metadata.beginWrite(true);
        MetadataWriteBatch batch(metadata.metadata_page_pool_, source.superblock(),
            LogSequenceNumber(source.superblock().checkpoint_lsn.value() + 1), conversion.memory);
        static_cast<void>(metadata.publishHybridPropertyRows(batch, writer, source, conversion, entities,
            pages, trees, values, {}, &context));
    }
''' +s[a:];p.write_text(s)
