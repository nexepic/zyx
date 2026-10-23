from pathlib import Path
p=Path('include/graph/storage/relocatable/metadata/HybridPropertyConversionPreparation.hpp');s=p.read_text();s=s.replace('    using LargeFieldResolver =', '''    using WholeRootResolver = std::function<std::pmr::vector<std::byte>(EntityPropertyKey,
        std::span<const BorrowedPropertyCollectionRootEntry>)>;
    using LargeFieldResolver =''',1);s=s.replace('const LargeFieldResolver &largeField = {}, bool compactColumns = false)', 'const LargeFieldResolver &largeField = {}, bool compactColumns = false,\n        bool coldRows = false, const WholeRootResolver &wholeRoot = {})',1);s=s.replace('        compact_ = compactColumns && !plan.columns().empty();','        compact_ = coldRows || (compactColumns && !plan.columns().empty());',1);s=s.replace('owners_.reserve(compact_ ? size_t(extensionCount) : plan.members().size());', 'owners_.reserve(coldRows ? 0U : compact_ ? size_t(extensionCount) : plan.members().size());',1);s=s.replace('        extensions_.reserve(size_t(extensionCount));','        extensions_.reserve(coldRows ? 0U : size_t(extensionCount));',1);s=s.replace('        if (compact_) matrix = HybridPropertyColumnMembership::fromColumns(group, plan.columns());','''        if (compact_ && !plan.columns().empty()) matrix = HybridPropertyColumnMembership::fromColumns(group, plan.columns());
        if (coldRows) {
            if (!matrix) {
                matrix.emplace(); matrix->owner_kind = group.owner_kind; matrix->group_id = group.group_id;
                matrix->semantic_generation = group.semantic_generation; matrix->live = group.live;
            }
            matrix->row_remainder = true;
        }''',1)
idx=s.index('        if (matrix) {\n            // Reserve both')
s=s[:idx]+'''        if (coldRows) {
            // Source values remain borrowed. Only canonical bytes and existing
            // whole-root references are staged in the original account.
            using Entry = BorrowedPropertyCollectionRootEntry;
            utils::OwnedHeapReservation scratch(plan.memoryOwner(), sizeof(std::array<Entry, HybridPropertyGroupConversion::MAX_MEMBERSHIPS>));
            std::array<Entry, HybridPropertyGroupConversion::MAX_MEMBERSHIPS> selected{};
            utils::OwnedVector<std::pmr::vector<std::byte>> roots{utils::OwnedPmrAllocator<std::pmr::vector<std::byte>>(plan.memoryOwner())};
            utils::OwnedVector<HybridColdRowInput> rows{utils::OwnedPmrAllocator<HybridColdRowInput>(plan.memoryOwner())};
            roots.reserve(64); rows.reserve(64);
            const auto members = plan.members(); size_t first = 0;
            const auto kindType = group.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
            while (first < members.size()) {
                if (checkpoint) checkpoint();
                const auto ownerId = members[first].owner; size_t end = first, count = 0;
                while (end < members.size() && members[end].owner == ownerId) {
                    const auto &member = members[end++];
                    if (member.source == HybridPropertySource::HPS_EXTENSION) selected[count++] = {member.token, member.value};
                }
                first = end; if (!count) continue;
                const EntityPropertyKey owner{group.owner_kind, ownerId};
                const auto entries = std::span(selected).first(count);
                const auto size = EntityPropertyCollectionRootCodec::encodedSizeBorrowed(ownerId, kindType, entries, checkpoint);
                bool large = size > HybridPropertyColdRowBlock::MAX_UNCOMPRESSED_BYTES - HybridPropertyColdRowBlock::DIRECTORY_ENTRY_BYTES;
                std::pmr::vector<std::byte> bytes(plan.memoryOwner().get());
                if (!large) {
                    bytes = EntityPropertyCollectionRootCodec::encodeBorrowed(ownerId, kindType, entries, *plan.memoryOwner(), checkpoint);
                    if (size > HybridPropertyColdRowBlock::MAX_STORED_BYTES - HybridPropertyColdRowBlock::DIRECTORY_ENTRY_BYTES) {
                        const std::array<HybridColdRowInput, 1> probe{{{uint8_t(hybridPropertySlotFor(owner)), uint32_t(count), false, bytes}}};
                        try { static_cast<void>(HybridPropertyColdRowBlock::encode(group.owner_kind, group.group_id,
                            group.semantic_generation, probe, plan.memoryOwner())); }
                        catch (const std::length_error &) { large = true; }
                    }
                }
                if (large) {
                    if (!wholeRoot) throw std::length_error("cold ROW requires the same-batch whole-root LargeValue resolver");
                    bytes = wholeRoot(owner, entries);
                    utils::OwnedHeapReservation credit(plan.memoryOwner(), bytes.size());
                    const auto reference = EntityPropertyLargeValueReferenceCodec::decode(bytes);
                    hybrid_property_detail::require(reference.owner_entity_id == ownerId && reference.owner_entity_type == kindType &&
                        reference.value.kind == LargeValueKind::LVK_BYTES && reference.value.media_type == RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE &&
                        reference.value.generation <= group.semantic_generation &&
                        reference.value.logical_bytes >= RSS_ENTITY_PROPERTY_LARGE_VALUE_PAYLOAD_HEADER_BYTES,
                        "cold ROW resolver returned a foreign, mistyped or future whole-root reference");
                }
                roots.push_back(std::move(bytes));
                rows.push_back({uint8_t(hybridPropertySlotFor(owner)), uint32_t(count), large, roots.back()});
            }
            first = 0;
            const auto rowBytes = [&](size_t i) { return HybridPropertyColdRowBlock::DIRECTORY_ENTRY_BYTES +
                (rows[i].large_value_reference ? 4U : 0U) + rows[i].root.size(); };
            while (first < rows.size()) {
                if (checkpoint) checkpoint();
                size_t end = first, size = 0;
                while (end < rows.size() && rowBytes(end) <= HybridPropertyColdRowBlock::MAX_UNCOMPRESSED_BYTES - size)
                    size += rowBytes(end++);
                hybrid_property_detail::require(end > first, "cold ROW reference exceeds its existing block bound");
                utils::OwnedVector<std::byte> encoded{utils::OwnedPmrAllocator<std::byte>(plan.memoryOwner())};
                const auto encode = [&] { return HybridPropertyColdRowBlock::encode(group.owner_kind, group.group_id,
                    group.semantic_generation, std::span(rows).subspan(first, end - first), plan.memoryOwner()); };
                try { encoded = encode(); }
                catch (const std::length_error &) {
                    // One retry at the RAW-safe prefix. A single larger row
                    // has already proved that its existing LZ4 form fits.
                    end = first + 1; size = rowBytes(first);
                    while (end < rows.size() && size <= HybridPropertyColdRowBlock::MAX_STORED_BYTES &&
                        rowBytes(end) <= HybridPropertyColdRowBlock::MAX_STORED_BYTES - size) size += rowBytes(end++);
                    encoded = encode();
                }
                const auto slot = destination(encoded.size()).appendColdRows(encoded, group.owner_kind, group.group_id);
                for (size_t i = first; i < end; ++i) {
                    matrix->cold_rows.owners |= UINT64_C(1) << rows[i].owner_slot;
                    matrix->cold_rows.roots[rows[i].owner_slot] = slot;
                    matrix->cold_fields += rows[i].field_count;
                }
                first = end;
            }
        }
''' +s[idx:];s=s.replace('            group.mode = HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP;', '            group.mode = coldRows ? HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER :\n                HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP;',1);s=s.replace('        for (const auto &member : plan.members()) {','        if (!coldRows) for (const auto &member : plan.members()) {',1);p.write_text(s)
