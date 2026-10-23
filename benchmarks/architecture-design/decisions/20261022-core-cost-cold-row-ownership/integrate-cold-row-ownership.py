from pathlib import Path
root=Path('/workspace/zyx')
# All immutable-store inspection operations are const, including physical audit.
for name in ['include/graph/storage/relocatable/property/HybridPropertyColdRowInspection.hpp','src/storage/relocatable/property/HybridPropertyColdRowInspection.cpp']:
 p=root/name;s=p.read_text().replace('LargeValueStore *values_', 'const LargeValueStore *values_').replace('LargeValueStore *values =', 'const LargeValueStore *values =').replace('memory, LargeValueStore *values,','memory, const LargeValueStore *values,');p.write_text(s)
p=root/'include/graph/storage/relocatable/property/HybridPropertyFieldView.hpp';s=p.read_text();a=s.index('    [[nodiscard]] HybridPropertyColdRowBlock::Decoded readColdRows(');b=s.index('    [[nodiscard]] HybridPropertyGroupRecord readGroup(',a);part=s[a:b]
bodyStart=part.index('        matrix.validateInGroup');body=part[bodyStart:];assert body.endswith('    }\n');body=body[:-len('    }\n')].replace('visible_generation_', 'visibleGeneration').replace('page_.read(reference)', 'bytes')
method=part[:bodyStart]+'''        return decodeColdRows(page_.read(reference), reference, group, matrix, descriptorGeneration,
            visible_generation_, std::move(memory), cancellation);
    }
    // Physical auditing already owns the exact authenticated slot bytes. Share
    // the same binding/owner/count checks without opening a second ROW body.
    [[nodiscard]] static HybridPropertyColdRowBlock::Decoded decodeColdRows(std::span<const std::byte> bytes,
        const SlottedSlotReference &reference, const HybridPropertyGroupRecord &group,
        const HybridPropertyColumnMembership &matrix, Generation descriptorGeneration, Generation visibleGeneration,
        std::shared_ptr<std::pmr::memory_resource> memory, const CancellationCheckpoint *cancellation = nullptr) {
'''+body+'    }\n'
s=s[:a]+method+s[b:];p.write_text(s)
p=root/'include/graph/storage/relocatable/property/HybridPropertyDirectoryIntegrity.hpp';s=p.read_text()
s=s.replace('directory_selectors = 0;','directory_selectors = 0, cold_blocks = 0, cold_rows = 0;',1)
s=s.replace('if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP)\n                    add(directColumns, group.columns);','if (group.usesColumnMembership())\n                    add(directColumns, group.columns);',1)
s=s.replace('add(report.columns, local.columns); add(report.extensions, local.extensions); add(report.large_values, local.large_values);','add(report.columns, local.columns); add(report.extensions, local.extensions); add(report.large_values, local.large_values);\n            add(report.cold_blocks, local.cold_blocks); add(report.cold_rows, local.cold_rows);',1)
# Allow the new read representation only inside this audited ownership entry.
s=s.replace('if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) {\n                    require(group.column_membership', 'if (group.usesColumnMembership()) {\n                    require(group.column_membership',1)
s=s.replace('const auto group = context.findGroup(owner, [&](size_t count) { boundary.set(count); });','const auto group = context.findGroup(owner, [&](size_t count) { boundary.set(count); }, nullptr, true, true);',1)
s=s.replace('require(group && group->mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP &&','require(group && group->usesColumnMembership() &&',1)
s=s.replace('const auto group = context.findGroup(firstOwner, std::ref(admit));','const auto group = context.findGroup(firstOwner, std::ref(admit), nullptr, true, true);',1)
s=s.replace('if (group->mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) {\n                    const auto matrix', 'if (group->usesColumnMembership()) {\n                    const auto matrix',1)
# Reverse cold slots use owner-kind@7 (unlike column/group owner-kind@6).
needle='''            } else if (magic == RSS_HYBRID_EXTENSION_MAGIC) {''';assert needle in s;s=s.replace(needle,'''            } else if (magic == HybridPropertyColdRowBlock::MAGIC) {
                require(bytes.size() >= HybridPropertyColdRowBlock::HEADER_BYTES, "native cold block header is truncated");
                const auto coldKind = EntityPropertyOwnerKind(std::to_integer<uint8_t>(bytes[7]));
                const auto groupId = LittleEndianCodec::readU64(bytes, 8);
                const auto owners = LittleEndianCodec::readU64(bytes, 24);
                require(hybrid_property_detail::validKind(coldKind) && owners, "native cold block has no owner namespace");
                hybrid_property_detail::validateGroup(groupId, owners);
                const EntityPropertyKey firstOwner{coldKind, (groupId - 1U) * 64U + 1U + unsigned(std::countr_zero(owners))};
                utils::OwnedHeapReservation boundary(memory);
                const auto admit = [&](size_t count) { boundary.set(count); };
                const auto group = context.findGroup(firstOwner, std::ref(admit), nullptr, true, true);
                require(group && group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER,
                    "native cold record has no ROW-remainder group");
                context.withColumnMembership(*group, [&](const auto &matrix) {
                    static_cast<void>(inspectColdBlock(context, snapshot, *group, matrix, reference, memory,
                        largeValues, visitLarge, bytes));
                });
'''+needle,1)
# Only legacy groups can own EXT records.
s=s.replace('''                require(group && group->memberships && context.isLive(owner, *group),
                    "native physical extension has no registered live owner");''','''                require(group && group->mode != HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
                    group->memberships && context.isLive(owner, *group),
                    "native physical extension has no registered legacy live owner");''',1)
# Avoid repeating cold decompression for each owner during the full audit.
needle='''        uint64_t ownerCount = 0, columnCount = 0, bodyCount = 0, extensionCount = 0;''';assert needle in s;s=s.replace(needle,'''        if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
            inspectRemainderGroup(context, snapshot, group, memory, largeValues, report); return;
        }
'''+needle,1)
pos=s.index('    static uint64_t countOwners(')
s=s[:pos]+'''    struct ColdCounts { uint64_t fields = 0, rows = 0, large_values = 0; };
    static ColdCounts inspectColdBlock(const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot,
        const HybridPropertyGroupRecord &group, const HybridPropertyColumnMembership &matrix,
        const SlottedSlotReference &reference, const std::shared_ptr<std::pmr::memory_resource> &memory,
        const LargeValueStore *largeValues, const LargeValueVisitor &visitLarge = {}, std::span<const std::byte> supplied = {}) {
        // Forward checks borrow the authenticated page; reverse checks already
        // own that exact slot. Both feed one bounded decoded cold body.
        const auto decode = [&] {
            if (!supplied.empty()) return HybridPropertyFieldView::decodeColdRows(supplied, reference, group, matrix,
                context.descriptor_->semantic_generation, snapshot.superblock().generation, memory, context.cancellation_);
            const auto page = context.fields_.read(snapshot, reference.page_id);
            return page.view().readColdRows(reference, group, matrix, context.descriptor_->semantic_generation, memory, context.cancellation_);
        };
        const auto block = decode();
        ColdCounts result;
        auto pending = block.owners;
        while (pending) {
            context.check();
            const auto slot = unsigned(std::countr_zero(pending)); pending &= pending - 1U;
            const auto row = block.row(slot);
            require(bool(row), "native cold block lost an owner row");
            const EntityPropertyKey owner{group.owner_kind, (group.group_id - 1U) * 64U + slot + 1U};
            HybridPropertyColdRowInspection source(snapshot, *row, owner, block.semantic_generation,
                std::span(matrix.tokens).first(matrix.column_count), memory, largeValues, context.cancellation_);
            std::array<uint32_t, 64> tokens{};
            for (size_t first = 0; first < source.count();) {
                const auto count = (std::min)(tokens.size(), source.count() - first);
                for (size_t i = 0; i < count; ++i) tokens[i] = source.token(first + i);
                const auto inspect = [&](size_t, std::optional<std::span<const std::byte>> bytes) {
                    context.check(); require(bool(bytes), "native cold directory lost a selected field");
                };
                source.visitSelected(std::span(tokens).first(count), std::ref(inspect));
                first += count;
            }
            add(result.fields, source.count()); add(result.rows, 1);
            if (source.reference()) {
                add(result.large_values, 1);
                if (visitLarge) { context.check(); visitLarge(source.reference()->value); }
            }
        }
        require(result.fields == block.fields && result.rows == unsigned(std::popcount(block.owners)),
            "native cold block fields or owner count disagrees with actual roots");
        return result;
    }
    static void inspectRemainderGroup(const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot,
        const HybridPropertyGroupRecord &group, const std::shared_ptr<std::pmr::memory_resource> &memory,
        const LargeValueStore *largeValues, Report &report) {
        const auto first = (group.group_id - 1U) * 64U + 1U;
        for (unsigned slot = 0; slot < 64; ++slot) {
            context.check(); const EntityPropertyKey owner{group.owner_kind, first + slot};
            if (!owner.isValid()) continue;
            utils::OwnedHeapReservation boundary(memory);
            const auto admit = [&](size_t bytes) { boundary.set(bytes); };
            const auto actual = context.findGroup(owner, std::ref(admit), nullptr, true, true);
            require(actual && *actual == group, "native cold group changed its authenticated owner route");
            context.requireNoLegacySelector(owner, std::ref(admit));
        }
        context.withColumnMembership(group, [&](const auto &matrix) {
            uint64_t hotFields = 0, coldFields = 0, coldRows = 0, blocks = 0, large = 0;
            for (size_t i = 0; i < matrix.column_count; ++i) {
                context.check(); const auto column = context.columnRecord(group, matrix.tokens[i], {});
                require(column.reference == matrix.locators[i] && column.column.presence == matrix.presence(matrix.tokens[i]),
                    "native cold group column locator or presence contradicts its membership");
                add(hotFields, unsigned(std::popcount(column.column.presence)));
            }
            auto pending = matrix.cold_rows.owners;
            while (pending) {
                context.check(); const auto reference = matrix.cold_rows.roots[unsigned(std::countr_zero(pending))];
                uint64_t aliases = 0;
                for (unsigned slot = 0; slot < 64; ++slot)
                    if ((matrix.cold_rows.owners & (UINT64_C(1) << slot)) && matrix.cold_rows.roots[slot] == reference)
                        aliases |= UINT64_C(1) << slot;
                require(aliases && !(aliases & ~pending), "native cold block has duplicate physical ownership");
                pending &= ~aliases;
                const auto actual = inspectColdBlock(context, snapshot, group, matrix, reference, memory, largeValues);
                add(coldFields, actual.fields); add(coldRows, actual.rows); add(large, actual.large_values); add(blocks, 1);
            }
            require(hotFields == matrix.memberships() && coldFields == matrix.cold_fields &&
                coldRows == unsigned(std::popcount(matrix.cold_rows.owners)) && hotFields + coldFields == group.memberships,
                "native cold group counts disagree with actual column and cold sources");
            add(report.memberships, hotFields + coldFields); add(report.columns, matrix.column_count);
            add(report.cold_blocks, blocks); add(report.cold_rows, coldRows); add(report.large_values, large);
        });
    }
'''+s[pos:]
p.write_text(s)
