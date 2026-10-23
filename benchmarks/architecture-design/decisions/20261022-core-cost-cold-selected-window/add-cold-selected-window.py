from pathlib import Path
p=Path('include/graph/storage/relocatable/property/HybridPropertyReadContext.hpp');s=p.read_text();idx=s.index('    // One compact field is authenticated')
s=s[:idx]+'''    // Borrow one decoded cold body for a consecutive same-reference run in
    // the existing <=64-owner window. Output ordinals stay ordered; no decoded
    // body or callback is retained in the context and no second cache exists.
    [[nodiscard]] bool visitColdOwnersSorted(std::span<const EntityPropertyKey> owners, uint32_t token,
        const std::function<void(size_t, std::optional<std::span<const std::byte>>)> &visitor,
        const std::function<void(size_t)> &admission, LargeValueStore *largeValues) const {
        check(); require(!owners.empty() && owners.size() <= 64 && token && visitor, "invalid cold grouped request");
        const auto kind = owners.front().owner_kind; const auto groupId = hybridPropertyGroupFor(owners.front());
        uint64_t previous = 0;
        for (const auto owner : owners) {
            require(owner.isValid() && owner.owner_kind == kind && hybridPropertyGroupFor(owner) == groupId &&
                owner.entity_id > previous, "cold grouped owners must be ordered, unique and in one group");
            previous = owner.entity_id;
        }
        constexpr size_t scratch = 64U * (sizeof(uint64_t) + sizeof(std::optional<EntityPageLocation>) +
            sizeof(std::optional<EntityPropertyLocation>));
        const auto admit = [&](size_t bytes) {
            require(bytes <= (std::numeric_limits<size_t>::max)() - scratch, "cold grouped scratch overflow");
            if (admission) admission(bytes + scratch);
        };
        admit(0);
        const auto group = findGroup(owners.front(), admit, nullptr, false, true);
        if (!group || group->mode != HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) return false;
        return withColumnMembership(*group, [&](const auto &matrix) {
            if (matrix.ordinal(token)) return false; // globally hot absence never falls through to cold
            std::array<uint64_t, 64> ids{};
            std::array<std::optional<EntityPageLocation>, 64> entities{};
            std::array<std::optional<EntityPropertyLocation>, 64> rows{};
            for (size_t i = 0; i < owners.size(); ++i) ids[i] = owners[i].entity_id;
            if (kind == EntityPropertyOwnerKind::EPOK_NODE)
                metadata_.findNodePageLocationsSorted(snapshot_, std::span(ids).first(owners.size()),
                    std::span(entities).first(owners.size()), cancellation_);
            else metadata_.findEdgePageLocationsSorted(snapshot_, std::span(ids).first(owners.size()),
                std::span(entities).first(owners.size()), cancellation_);
            metadata_.findEntityPropertyLocationsSorted(snapshot_, owners, std::span(rows).first(owners.size()), cancellation_);
            for (size_t i = 0; i < owners.size(); ++i) {
                check(); require(bool(entities[i]) == isLive(owners[i], *group) && !rows[i],
                    "cold grouped owner has invalid Entity Map or competing ROW routing");
                requireNoLegacySelector(owners[i], admit);
            }
            debug::PerfTrace::addValue("native_graph.hybrid.grouped_owner_route_checks", int64_t(owners.size()));
            const auto hasCold = [&](size_t i) { return bool(matrix.cold_rows.owners & (UINT64_C(1) << hybridPropertySlotFor(owners[i]))); };
            const auto missing = [&](size_t i) {
                bool present = hasCold(i);
                for (const auto word : matrix.owners[hybridPropertySlotFor(owners[i])]) present |= word != 0;
                if (present) { check(); visitor(i, std::nullopt); }
            };
            size_t first = 0;
            while (first < owners.size()) {
                if (!hasCold(first)) { missing(first++); continue; }
                const auto root = matrix.cold_rows.roots[hybridPropertySlotFor(owners[first])];
                size_t end = first + 1;
                while (end < owners.size() && (!hasCold(end) ||
                    matrix.cold_rows.roots[hybridPropertySlotFor(owners[end])] == root)) ++end;
                withColdBlock(*group, matrix, root, [&](const auto &block, const auto &memory) {
                    for (size_t i = first; i < end; ++i) {
                        if (!hasCold(i)) { missing(i); continue; }
                        const auto row = block.row(hybridPropertySlotFor(owners[i]));
                        require(bool(row), "cold grouped membership has no row");
                        const auto inspect = [&](LargeValueStore *values) {
                            HybridPropertyColdRowInspection source(snapshot_, *row, owners[i], block.semantic_generation,
                                std::span(matrix.tokens).first(matrix.column_count), memory, values, cancellation_);
                            const std::array<uint32_t, 1> selected{token};
                            source.visitSelected(selected, [&](size_t, auto bytes) { check(); visitor(i, bytes); });
                        };
                        if (row->large_value_reference && !largeValues) {
                            utils::OwnedHeapReservation credit(memory,
                                utils::detail::standardAllocatorRequestBytes(sizeof(SlottedPagePool), alignof(SlottedPagePool)) +
                                utils::detail::standardAllocatorRequestBytes(sizeof(BTreePageStore), alignof(BTreePageStore)));
                            LargeValueStore values(source_io_, metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
                            inspect(&values);
                        } else inspect(largeValues);
                    }
                });
                first = end;
            }
            return true;
        });
    }
''' +s[idx:];p.write_text(s)
p=Path('include/graph/storage/relocatable/property/HybridPropertyValueReader.hpp');s=p.read_text();idx=s.index('            if(batches && tokens.size()==1')
s=s[:idx]+'''            if (sink && scalars && texts && tokens.size() == 1 && counts.empty()) {
                size_t end = i + 1;
                while (end < owners.size() && owners[end].owner_kind == owners[i].owner_kind &&
                    hybridPropertyGroupFor(owners[end]) == hybridPropertyGroupFor(owners[i])) ++end;
                // No decline/retry after a callback: exceptions propagate with
                // their existing ordinal and release the sole decoded body.
                const bool consumed = context.visitColdOwnersSorted(owners.subspan(i, end - i), tokens.front(),
                    [&](size_t relative, std::optional<std::span<const std::byte>> bytes) {
                        const auto ordinal = i + relative;
                        if (!bytes) { (*sink)(ordinal, 0, std::nullopt); return; }
                        if (admission) (*admission)(sizeof(uint32_t) + bytes->size());
                        const auto primitive = CanonicalPropertyValueCodec::tryBorrowPrimitive(*bytes);
                        context.checkpoint();
                        if (primitive) {
                            if (primitive->type == PropertyType::STRING) (*texts)(ordinal, 0, primitive->text);
                            else (*scalars)(ordinal, 0, primitive->type, primitive->bits);
                        } else {
                            size_t offset = 0;
                            auto value = CanonicalPropertyValueCodec::decodeNext(*bytes, offset);
                            hybrid_property_detail::require(offset == bytes->size(), "cold grouped value has trailing bytes");
                            (*sink)(ordinal, 0, PropertyCollectionRootEntry{tokens.front(), std::move(value)});
                        }
                    }, std::ref(boundary), largeValues);
                boundaryCredit.set(0);
                if (consumed) { i = end - 1; continue; }
            }
''' +s[idx:];p.write_text(s)
