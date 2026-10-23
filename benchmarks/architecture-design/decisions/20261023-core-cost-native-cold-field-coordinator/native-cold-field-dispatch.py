from pathlib import Path
p=Path('/workspace/zyx/src/storage/relocatable/metadata/RelocatableMetadataStoreHybridMutation.cpp');s=p.read_text();a=s.index('bool RelocatableMetadataStore::prepareHybridPropertyColumns');b=s.index('Superblock RelocatableMetadataStore::',a+10);c=s[a:b]
c=c.replace('''    inlineChanges.reserve(replacements.size());''','''    inlineChanges.reserve(replacements.size());
    utils::OwnedVector<const NativeHybridPropertyColumnReplacement *> coldChanges{
        utils::OwnedPmrAllocator<const NativeHybridPropertyColumnReplacement *>(request.memory)};
''',1)
needle='''        auto delta = HybridPropertyColumnDelta::planExistingColumn(context, source, replacement.owner,'''
c=c.replace(needle,'''        if (sourceGroup->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
            const auto page = fields.read(source, sourceGroup->column_membership.page_id);
            const auto matrix = page.view().readColumnMembership(sourceGroup->column_membership, *sourceGroup,
                descriptor.semantic_generation);
            if (!std::binary_search(matrix.tokens.begin(), matrix.tokens.begin() + matrix.column_count, replacement.token)) {
                require(request.field_lifecycle, "cold fields require explicit lifecycle coordination");
                coldChanges.push_back(&replacement); continue;
            }
        }
''' +needle,1)
needle='''    if (onlyCompatibleColumns) {'''
assert needle in c
c=c.replace(needle,'''    if (!coldChanges.empty()) {
        require(preparationSource && !multipleGroups && request.owner_deletions.empty() &&
            request.pending_owner_additions.empty() && inlineChanges.empty(),
            "cold field preparation requires one source group without pending liveness");
        const auto page = fields.read(source, sourceGroup->column_membership.page_id);
        const auto matrix = page.view().readColumnMembership(sourceGroup->column_membership, *sourceGroup,
            descriptor.semantic_generation);
        std::sort(coldChanges.begin(), coldChanges.end(), [](auto *left, auto *right) {
            return left->owner.entity_id < right->owner.entity_id ||
                (left->owner == right->owner && left->token < right->token);
        });
        struct PreparedRow { EntityPropertyKey owner; SlottedSlotReference reference; HybridColdRowInput row; };
        utils::OwnedVector<std::pmr::vector<std::byte>> roots{
            utils::OwnedPmrAllocator<std::pmr::vector<std::byte>>(request.memory)};
        utils::OwnedVector<PreparedRow> rows{utils::OwnedPmrAllocator<PreparedRow>(request.memory)};
        roots.reserve(64); rows.reserve(64);
        const std::function<void()> checkpoint = std::ref(check);
        const uint32_t ownerType = sourceGroup->owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
        for (size_t begin = 0; begin < coldChanges.size();) {
            check(); const auto owner = coldChanges[begin]->owner; size_t end = begin + 1;
            while (end < coldChanges.size() && coldChanges[end]->owner == owner) ++end;
            utils::OwnedHeapReservation decodeCredit(request.memory);
            struct DecodeOwner { utils::OwnedHeapReservation &credit; const std::function<void()> &check; } decodeOwner{decodeCredit, checkpoint};
            property_decode::AllocationScope decoding(&decodeOwner, [](void *p, size_t bytes) {
                auto &credit = static_cast<DecodeOwner *>(p)->credit;
                credit.set(utils::detail::addStandardHeapBytes(credit.bytes(), bytes));
            }, [](void *p) { static_cast<DecodeOwner *>(p)->check(); });
            auto collection = HybridPropertyValueReader::readCollection(context, source, owner, ownerType, &sourceValues);
            require(collection.routed, "cold native field lost its authenticated collection");
            utils::OwnedVector<BorrowedPropertyCollectionRootEntry> entries{
                utils::OwnedPmrAllocator<BorrowedPropertyCollectionRootEntry>(request.memory)};
            if (collection.value) for (const auto &entry : collection.value->entries) {
                if (!std::binary_search(matrix.tokens.begin(), matrix.tokens.begin() + matrix.column_count, entry.key_token))
                    entries.push_back({entry.key_token, &entry.value});
            }
            utils::OwnedVector<PropertyValue> scalars{utils::OwnedPmrAllocator<PropertyValue>(request.memory)};
            scalars.reserve(end - begin); bool changed = false;
            for (size_t i = begin; i < end; ++i) {
                check(); const auto &cell = *coldChanges[i];
                auto found = std::lower_bound(entries.begin(), entries.end(), cell.token,
                    [](const auto &entry, uint32_t token) { return entry.key_token < token; });
                if (cell.erase) {
                    if (found != entries.end() && found->key_token == cell.token) { entries.erase(found); changed = true; }
                    continue;
                }
                scalars.emplace_back(); const auto &value = cell.propertyValue(scalars.back());
                if (found != entries.end() && found->key_token == cell.token) {
                    if (*found->value == value) continue;
                    found->value = &value;
                } else entries.insert(found, {cell.token, &value});
                changed = true;
            }
            if (!changed) { begin = end; continue; }
            require(entries.size() <= UINT32_MAX, "cold field count overflows its original envelope");
            std::pmr::vector<std::byte> encoded(request.memory.get()); bool large = false;
            if (!entries.empty()) {
                const auto bytes = EntityPropertyCollectionRootCodec::encodedSizeBorrowed(owner.entity_id, ownerType, entries, checkpoint);
                large = bytes > HybridPropertyColdRowBlock::MAX_STORED_BYTES - HybridPropertyColdRowBlock::DIRECTORY_ENTRY_BYTES;
                if (!large) encoded = EntityPropertyCollectionRootCodec::encodeBorrowed(owner.entity_id, ownerType, entries, *request.memory, checkpoint);
                else {
                    auto proposal = HybridPropertyLargeFieldPreparation::prepareRootBorrowed(*this, source, sourceValues,
                        owner, entries, LargeValueId(batch.candidate().next_large_value_id), request.memory, request.cancellation);
                    const HybridPropertyLargeFieldPreparation *selected = &proposal;
                    if (!proposal.reused()) {
                        for (const auto &previous : largeFields) if (std::ranges::equal(previous.payload(), proposal.payload())) {
                            selected = &previous; break;
                        }
                        if (selected == &proposal) {
                            require(batch.allocateLargeValueId() == proposal.reference().value_id,
                                "cold native root lost its batch-local identity sequence");
                            largeFields.push_back(std::move(proposal)); selected = &largeFields.back();
                        }
                    }
                    const EntityPropertyLargeValueReference reference{owner.entity_id, ownerType, selected->reference()};
                    utils::OwnedHeapReservation referenceCredit(request.memory,
                        RSS_ENTITY_PROPERTY_LARGE_VALUE_REFERENCE_HEADER_BYTES + RSS_LARGE_VALUE_REF_HEADER_BYTES +
                        selected->reference().media_type.size() + 3U * utils::detail::standardContainerProxyBytes());
                    const auto bytes = EntityPropertyLargeValueReferenceCodec::encode(reference);
                    encoded.assign(bytes.begin(), bytes.end());
                }
            }
            roots.push_back(std::move(encoded));
            rows.push_back({owner, matrix.cold_rows.roots[hybridPropertySlotFor(owner)],
                {uint8_t(hybridPropertySlotFor(owner)), uint32_t(entries.size()), large, roots.back()}});
            begin = end;
        }
        std::sort(rows.begin(), rows.end(), [](const auto &left, const auto &right) {
            return left.reference < right.reference || (left.reference == right.reference && left.owner.entity_id < right.owner.entity_id);
        });
        utils::OwnedVector<HybridColdRowInput> inputs{utils::OwnedPmrAllocator<HybridColdRowInput>(request.memory)};
        utils::OwnedVector<HybridPropertyDemotionPreparation::ColdBlockChange> blocks{
            utils::OwnedPmrAllocator<HybridPropertyDemotionPreparation::ColdBlockChange>(request.memory)};
        inputs.reserve(rows.size()); blocks.reserve(rows.size());
        for (size_t begin = 0; begin < rows.size();) {
            size_t end = begin + 1; while (end < rows.size() && rows[end].reference == rows[begin].reference) ++end;
            const auto offset = inputs.size();
            for (size_t i = begin; i < end; ++i) inputs.push_back(rows[i].row);
            blocks.push_back({rows[begin].owner, rows[begin].reference, std::span(inputs).subspan(offset, end - begin)});
            begin = end;
        }
        if (!blocks.empty()) lifecycle.emplace(HybridPropertyDemotionPreparation::prepareColdBlocks(batch,
            context, blocks, request.memory, changes, &sourceValues, request.cancellation, largeFields));
        else if (!changes.empty()) lifecycle.emplace(batch, source, fields, changes, request.memory, request.cancellation, largeResolver);
        else { lifecycle.emplace(HybridPropertyDemotionPreparation(request.memory)); lifecycle->group_ = *sourceGroup; }
    } else if (onlyCompatibleColumns) {''',1)
s=s[:a]+c+s[b:];p.write_text(s)
