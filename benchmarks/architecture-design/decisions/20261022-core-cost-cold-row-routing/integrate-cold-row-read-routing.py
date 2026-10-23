from pathlib import Path
root=Path('/workspace/zyx')
p=root/'include/graph/storage/relocatable/property/HybridPropertyRecords.hpp';s=p.read_text().replace('HPS_EXTENSION = 2 };','HPS_EXTENSION = 2, HPS_COLD_ROW = 3 };');p.write_text(s)
p=root/'include/graph/storage/relocatable/property/HybridPropertyReadContext.hpp';s=p.read_text().replace('#include "graph/storage/relocatable/property/HybridPropertyFieldPageStore.hpp"','#include "graph/storage/relocatable/property/HybridPropertyFieldPageStore.hpp"\n#include "graph/storage/relocatable/property/HybridPropertyColdRowInspection.hpp"\n#include "graph/utils/StandardContainerHeapBytes.hpp"')
s=s.replace('    using ExtensionVisitor =','    using EncodedVisitor = std::function<void(std::span<const std::byte>)>;\n    using ExtensionVisitor =',1)
# Read entry points alone can cross the new-mode guard. Maintenance/writes keep it.
s=s.replace('const auto group = findGroup(owner, boundaryAdmission);\n        if (!group) return {};\n        const bool live', 'const auto group = findGroup(owner, boundaryAdmission, nullptr, true, true);\n        if (!group) return {};\n        if (group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER)\n            return visitRemainderOwner(owner, *group, maximumEntries, visitor, boundaryAdmission);\n        const bool live',1)
s=s.replace('        const ColumnVisitor &columnVisitor = {}) const {','        const ColumnVisitor &columnVisitor = {}, const EncodedVisitor &encoded = {}, LargeValueStore *largeValues = nullptr) const {',1)
s=s.replace('const auto group = findGroup(owner, boundaryAdmission, &groupReference);\n        if (!group) return FieldRoute::HFR_ROW;', 'const auto group = findGroup(owner, boundaryAdmission, &groupReference, true, true);\n        if (!group) return FieldRoute::HFR_ROW;\n        if (group->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER)\n            return visitRemainderField(owner, token, *group, groupReference, scalar, encoded,\n                boundaryAdmission, columnVisitor, largeValues);',1)
s=s.replace('findGroup(owners.front(),admit,&groupReference,false);','findGroup(owners.front(),admit,&groupReference,false,true);',1)
s=s.replace('if(!group || group->mode!=HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP)return false;','if(!group || !group->usesColumnMembership())return false;',1)
s=s.replace('                if(selected<owners.size() && owners[selected]==field.owner && field.token==token) {','                if(selected<owners.size() && owners[selected]==field.owner)\n                    require(group->mode != HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER,\n                        "ROW-remainder owner has a competing explicit selector");\n                if(selected<owners.size() && owners[selected]==field.owner && field.token==token) {',1)
s=s.replace('const auto group = findGroup(owner, admission);','const auto group = findGroup(owner, admission, nullptr, true, true);',1)
s=s.replace('trees_(pages, metadata), membership_io_(io.get()), fields_(std::move(io), metadata, snapshot),','trees_(pages, metadata), membership_io_(io.get()), source_io_(io), fields_(std::move(io), metadata, snapshot),',1)
s=s.replace('        bool authenticateOwner = true) const {','        bool authenticateOwner = true, bool allowRemainderRead = false) const {',1)
s=s.replace('if (decoded->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER)\n            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,\n                "native cold ROW group reads are not integrated yet");','if (decoded->mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER && !allowRemainderRead)\n            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,\n                "native cold ROW mutation and ownership lifecycle is not integrated yet");',1)
s=s.replace('if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) {\n                debug::PerfTrace::addValue("native_graph.hybrid.direct_column_locator_reads", 1);','if (group.usesColumnMembership()) {\n                debug::PerfTrace::addValue("native_graph.hybrid.direct_column_locator_reads", 1);',1)
s=s.replace('check(); require(group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP &&','check(); require(group.usesColumnMembership() &&',1)
s=s.replace('require(hasRoot(group.owner_kind, 0) || decoded.memberships() == group.memberships,','require(group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER ||\n            hasRoot(group.owner_kind, 0) || decoded.memberships() == group.memberships,',1)
s=s.replace('    HybridPropertyFieldPageStore fields_;','    std::shared_ptr<StorageIO> source_io_;\n    HybridPropertyFieldPageStore fields_;',1)
# These helpers use the same authenticated metadata/context and existing store.
pos=s.index('    [[nodiscard]] AuthenticatedColumn columnRecord(')
s=s[:pos]+'''    void requireNoLegacySelector(EntityPropertyKey owner, const std::function<void(size_t)> &admission) const {
        if (!hasRoot(owner.owner_kind, 0)) return;
        const auto map = ownerMap(owner.owner_kind);
        static_cast<void>(HybridPropertyDirectoryCodec::visitOwner(map, snapshot_, owner, 1,
            [](uint32_t, HybridPropertySource) -> bool {
                require(false, "ROW-remainder owner has a competing explicit selector"); return false;
            }, admission, cancellation_));
    }
    template<class Visitor>
    bool withColdOwner(EntityPropertyKey owner, const HybridPropertyGroupRecord &group,
        const HybridPropertyColumnMembership &matrix, Visitor &&visitor, LargeValueStore *largeValues = nullptr) const {
        check();
        const auto slot = hybridPropertySlotFor(owner);
        if (!(matrix.cold_rows.owners & (UINT64_C(1) << slot))) return false;
        const auto memory = utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
            (snapshot_.readContext() ? snapshot_.readContext()->memoryOwner() : nullptr);
        require(bool(memory), "cold ROW reading requires the caller's original resource account");
        utils::OwnedAllocationScope scope(memory);
        const auto reference = matrix.cold_rows.roots[slot];
        const auto page = fields_.read(snapshot_, reference.page_id);
        const auto block = page.view().readColdRows(reference, group, matrix, descriptor_->semantic_generation, memory, cancellation_);
        const auto row = block.row(slot);
        require(bool(row), "cold ROW membership has no source row");
        debug::PerfTrace::addValue("native_graph.hybrid.cold_block_decodes", 1);
        const auto inspect = [&](LargeValueStore *values) {
            HybridPropertyColdRowInspection source(snapshot_, *row, owner, block.semantic_generation,
                std::span(matrix.tokens).first(matrix.column_count), memory, values, cancellation_);
            visitor(source);
        };
        if (row->large_value_reference && !largeValues) {
            // Read-only instance of the same immutable store, not another
            // carrier or cache. Charge its two existing holder allocations.
            utils::OwnedHeapReservation storeCredit(memory,
                utils::detail::standardAllocatorRequestBytes(sizeof(SlottedPagePool), alignof(SlottedPagePool)) +
                utils::detail::standardAllocatorRequestBytes(sizeof(BTreePageStore), alignof(BTreePageStore)));
            LargeValueStore values(source_io_, metadata_, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
            inspect(&values);
        } else inspect(largeValues);
        check(); return true;
    }
    Enumeration visitRemainderOwner(EntityPropertyKey owner, const HybridPropertyGroupRecord &group, size_t maximumEntries,
        const std::function<bool(uint32_t, HybridPropertySource)> &visitor,
        const std::function<void(size_t)> &admission) const {
        requireNoLegacySelector(owner, admission);
        Enumeration result{true, isLive(owner, group), true, 0};
        withColumnMembership(group, [&](const auto &matrix) {
            std::array<uint32_t, HybridPropertyColumnMembership::MAX_COLUMNS> hot{}; size_t count = 0, position = 0;
            require(matrix.visitOwner(owner, hot.size(), [&](uint32_t token) { hot[count++] = token; return true; }),
                "ROW-remainder owner exceeds its bounded hot columns");
            const auto emit = [&](uint32_t token, HybridPropertySource source) {
                check();
                if (result.fields == maximumEntries) return false;
                require(result.owner_live && result.fields < group.memberships, "cold ROW owner is dead or exceeds its group count");
                ++result.fields; return visitor(token, source);
            };
            withColdOwner(owner, group, matrix, [&](auto &source) {
                for (size_t i = 0; result.complete && i < source.count(); ++i) {
                    const auto token = source.token(i);
                    while (position < count && hot[position] < token)
                        if (!emit(hot[position++], HybridPropertySource::HPS_COLUMN)) { result.complete = false; break; }
                    if (result.complete && !emit(token, HybridPropertySource::HPS_COLD_ROW)) result.complete = false;
                }
            });
            while (result.complete && position < count)
                if (!emit(hot[position++], HybridPropertySource::HPS_COLUMN)) result.complete = false;
        });
        return result;
    }
    FieldRoute visitRemainderField(EntityPropertyKey owner, uint32_t token, const HybridPropertyGroupRecord &group,
        const SlottedSlotReference &groupReference, const ScalarVisitor &scalar, const EncodedVisitor &encoded,
        const std::function<void(size_t)> &admission, const ColumnVisitor &columnVisitor, LargeValueStore *largeValues) const {
        requireNoLegacySelector(owner, admission);
        return withColumnMembership(group, [&](const auto &matrix) {
            if (matrix.ordinal(token)) {
                if (!matrix.contains(owner, token)) return FieldRoute::HFR_MISSING;
                const auto selected = columnRecord(group, token, admission);
                const auto slot = hybridPropertySlotFor(owner); const auto bit = UINT64_C(1) << slot;
                require(selected.column.presence == matrix.presence(token), "ROW-remainder column disagrees with its membership");
                const HybridPropertyFieldView::ScalarCell cell{selected.column.identity.scalar_kind,
                    true, bool(selected.column.nulls & bit), selected.column.words[slot]};
                if (columnVisitor) columnVisitor(group, selected.column, groupReference, selected.reference);
                check(); scalar(cell); return FieldRoute::HFR_FOUND;
            }
            bool found = false;
            withColdOwner(owner, group, matrix, [&](auto &source) {
                const std::array<uint32_t, 1> tokens{token};
                const auto sink = [&](size_t, std::optional<std::span<const std::byte>> bytes) {
                    check(); found = bool(bytes); if (bytes && encoded) encoded(*bytes);
                };
                source.visitSelected(tokens, std::ref(sink));
            }, largeValues);
            return found ? FieldRoute::HFR_FOUND : FieldRoute::HFR_MISSING;
        });
    }
'''+s[pos:]
p.write_text(s)
p=root/'include/graph/storage/relocatable/property/HybridPropertyValueReader.hpp';s=p.read_text()
# Cold root presence/NULL inspection uses authenticated selected canonical bytes.
needle='''        const auto route = context.visitField(owner, token, std::ref(scalar), std::ref(extension),
            std::ref(boundary), std::ref(reference));'''
assert s.count(needle)==1
s=s.replace(needle,'''        const auto encoded = [&](std::span<const std::byte> bytes) {
            boundaryCredit.set(0);
            present = !requireNonNull || !CanonicalPropertyValueCodec::isNullHeader(bytes, bytes.size());
        };
        const auto route = context.visitField(owner, token, std::ref(scalar), std::ref(extension),
            std::ref(boundary), std::ref(reference), {}, std::ref(encoded), largeValues);''',1)
needle='''        result.route = context.visitField(owner, token, std::ref(scalar), std::ref(extension),
            std::ref(boundary), std::ref(reference));'''
assert needle in s
s=s.replace(needle,'''        const auto encoded = [&](std::span<const std::byte> bytes) {
            boundaryCredit.set(0);
            if (admission) (*admission)(sizeof(uint32_t) + bytes.size());
            if (mode == ReadMode::HRM_ADMISSION_ONLY) return;
            size_t offset = 0;
            auto value = CanonicalPropertyValueCodec::decodeNext(bytes, offset);
            hybrid_property_detail::require(offset == bytes.size(), "cold ROW selected value has trailing bytes");
            result.value = PropertyCollectionRootEntry{token, std::move(value)};
        };
        result.route = context.visitField(owner, token, std::ref(scalar), std::ref(extension),
            std::ref(boundary), std::ref(reference), {}, std::ref(encoded), largeValues);''',1)
p.write_text(s)
