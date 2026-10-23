/**
 * @file HybridPropertyDirectoryIntegrity.hpp
 * @brief Streaming native hybrid cross-directory consistency checks.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/storage/relocatable/property/HybridPropertyReadContext.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/property/HybridPropertyLargeFieldInspection.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"

namespace graph::storage {
// Verifies the descriptor-bound logical graph of group/owner/column/extension
// directories, including full canonical single-field bodies and LargeValue
// authentication. It does not authorize publication or reclaim storage, and
// does not replace physical unreferenced-slot/retirement reconciliation.
class HybridPropertyDirectoryIntegrity final {
public:
    using LargeValueVisitor = std::function<void(const LargeValueRef &)>;
    struct Report {
        uint64_t groups = 0, memberships = 0, columns = 0, extensions = 0, large_values = 0, directory_selectors = 0, cold_blocks = 0, cold_rows = 0;
    };
    [[nodiscard]] static Report verify(const HybridPropertyReadContext &context,
        const MetadataSnapshotPin &snapshot, std::shared_ptr<std::pmr::memory_resource> memory,
        LargeValueStore *largeValues = nullptr) {
        require(context.usesSnapshot(snapshot) && bool(memory), "hybrid integrity requires its original pin and account");
        utils::OwnedAllocationScope scope(memory);
        Report report;
        for (const auto kind : {EntityPropertyOwnerKind::EPOK_NODE, EntityPropertyOwnerKind::EPOK_EDGE}) {
            context.check();
            if (!context.hasRoot(kind, 3)) {
                require(!context.hasRoot(kind, 0) && !context.hasRoot(kind, 1) && !context.hasRoot(kind, 2),
                    "hybrid integrity found field namespaces without a group namespace");
                continue;
            }
            Report local;
            uint64_t directColumns = 0;
            const auto binding = context.bind(kind, 3);
            HybridPropertyGroupDirectoryMap groups(binding.root.index_id, context.trees_, context.metadata_, binding);
            // Each live cursor has its own boundary reservation. Nested member
            // and field checks must not refund a still-live outer cursor's key.
            utils::OwnedHeapReservation groupBoundary(memory);
            const auto admitGroup = [&](size_t bytes) { groupBoundary.set(bytes); };
            const auto inspect = [&](const HybridPropertyGroupDirectoryMap::Entry &entry) {
                context.check();
                const auto groupId = HybridPropertyDirectoryCodec::decodeGroupKey(entry.key);
                const auto reference = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
                const auto page = context.fields_.read(snapshot, reference.page_id);
                const auto group = page.view().readGroup(reference, kind, groupId, context.descriptor_->semantic_generation);
                inspectGroup(context, snapshot, group, memory, largeValues, local);
                if (group.usesColumnMembership())
                    add(directColumns, group.columns);
                add(local.groups, 1);
                return true;
            };
            require(groups.visitAll(snapshot, bound(), std::ref(inspect), std::ref(admitGroup), context.cancellation_),
                "hybrid group integrity scan was incomplete");
            // Every locally inspected body is required to own its selector.
            // Equal global cardinalities then exclude orphan directory entries
            // without retaining a database-sized key or ownership set.
            require(countOwners(context, snapshot, kind, memory) == local.directory_selectors &&
                countFields(context, snapshot, kind, 1, memory) == local.columns - directColumns &&
                countFields(context, snapshot, kind, 2, memory) == local.extensions,
                "hybrid directories contain entries outside their group ownership");
            add(report.groups, local.groups); add(report.memberships, local.memberships);
            add(report.directory_selectors, local.directory_selectors);
            add(report.columns, local.columns); add(report.extensions, local.extensions); add(report.large_values, local.large_values);
            add(report.cold_blocks, local.cold_blocks); add(report.cold_rows, local.cold_rows);
        }
        context.check();
        return report;
    }
    // Companion phase for each CURRENT Page Directory native field frame.
    // Directory verification authenticates all referenced bodies; this reverse
    // lookup rejects live physical records with no exact directory locator.
    // Old unrouted versions/retire queues require their separate pinned walker.
    [[nodiscard]] static uint64_t verifyPageOwnership(const HybridPropertyReadContext &context,
        const MetadataSnapshotPin &snapshot, PageId id, std::shared_ptr<std::pmr::memory_resource> memory,
        const LargeValueStore *largeValues = nullptr, const LargeValueVisitor &visitLarge = {}) {
        require(context.usesSnapshot(snapshot) && bool(memory),
            "native physical ownership requires its original pin/account");
        utils::OwnedAllocationScope scope(memory);
        context.check();
        const auto page = context.fields_.read(snapshot, id);
        uint64_t records = 0;
        page.visitRecords(memory, [&](const SlottedSlotReference &reference, std::span<const std::byte> bytes) {
            context.check();
            require(bool(context.descriptor_), "native live slot has no descriptor ownership");
            require(bytes.size() >= 32, "native live slot has no complete typed identity");
            const auto kind = EntityPropertyOwnerKind(std::to_integer<uint8_t>(bytes[6]));
            const auto magic = LittleEndianCodec::readU32(bytes, 0);
            if (magic == RSS_HYBRID_GROUP_MAGIC) {
                const auto group = HybridPropertyGroupRecord::decode(bytes, kind, LittleEndianCodec::readU64(bytes, 8),
                    context.descriptor_->semantic_generation, snapshot.superblock().generation);
                requireLocator<HybridPropertyGroupDirectoryMap>(context, snapshot, kind, 3,
                    HybridPropertyDirectoryCodec::groupKey(group.group_id), reference, memory);
                if (group.usesColumnMembership()) {
                    require(group.column_membership.page_id == reference.page_id && group.column_membership.slot_id != reference.slot_id,
                        "compact physical group has no same-page membership record");
                    static_cast<void>(context.columnMembership(group));
                }
            } else if (magic == HybridPropertyColumnMembership::MAGIC) {
                require(bytes.size() >= 64, "native membership record has a truncated header");
                const auto groupId = LittleEndianCodec::readU64(bytes, 8);
                const auto live = LittleEndianCodec::readU64(bytes, 24);
                require(live != 0, "native membership record has no live group");
                hybrid_property_detail::validateGroup(groupId, live);
                const EntityPropertyKey owner{kind, (groupId - 1U) * 64U + 1U + unsigned(std::countr_zero(live))};
                utils::OwnedHeapReservation boundary(memory);
                const auto group = context.findGroup(owner, [&](size_t count) { boundary.set(count); }, nullptr, true, true);
                require(group && group->usesColumnMembership() &&
                    group->column_membership == reference, "native membership record has no exact group locator");
                static_cast<void>(HybridPropertyColumnMembership::decode(bytes, *group,
                    context.descriptor_->semantic_generation, snapshot.superblock().generation));
            } else if (magic == RSS_HYBRID_COLUMN_MAGIC) {
                const HybridPropertyColumnIdentity identity{kind,
                    HybridPropertyScalarKind(std::to_integer<uint8_t>(bytes[7])), LittleEndianCodec::readU64(bytes, 8),
                    LittleEndianCodec::readU32(bytes, 16), Generation(LittleEndianCodec::readU64(bytes, 24))};
                const auto column = HybridPropertyColumnRecord::decodePayload(bytes, identity);
                const EntityPropertyKey firstOwner{kind, (identity.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY +
                    1U + unsigned(std::countr_zero(column.presence))};
                utils::OwnedHeapReservation boundary(memory);
                const auto admit = [&](size_t count) { boundary.set(count); };
                const auto group = context.findGroup(firstOwner, std::ref(admit), nullptr, true, true);
                require(bool(group), "native physical column has no registered group");
                column.validateInGroup(*group, context.descriptor_->semantic_generation, snapshot.superblock().generation);
                if (group->usesColumnMembership()) {
                    const auto matrix = context.columnMembership(*group);
                    require(matrix.columnLocator(identity.token) == reference && matrix.presence(identity.token) == column.presence,
                        "compact physical column has no exact membership locator or presence");
                } else requireLocator<HybridPropertyFieldDirectoryMap>(context, snapshot, kind, 1,
                        HybridPropertyDirectoryCodec::columnKey(identity.group_id, identity.token), reference, memory);
            } else if (magic == HybridPropertyColdRowBlock::MAGIC) {
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
            } else if (magic == RSS_HYBRID_EXTENSION_MAGIC) {
                const EntityPropertyKey owner{kind, LittleEndianCodec::readU64(bytes, 8)};
                const auto token = LittleEndianCodec::readU32(bytes, 16);
                utils::OwnedHeapReservation boundary(memory), referenceCredit(memory);
                const auto admit = [&](size_t count) { boundary.set(count); };
                const auto group = context.findGroup(owner, std::ref(admit), nullptr, true, true);
                require(group && group->mode != HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER &&
                    group->memberships && context.isLive(owner, *group),
                    "native physical extension has no registered legacy live owner");
                const auto admitReference = [&](size_t count) { referenceCredit.set(count); };
                const auto extension = HybridPropertyExtensionView::open(bytes, owner, token, group->semantic_generation,
                    context.descriptor_->semantic_generation, snapshot.superblock().generation, std::ref(admitReference));
                static_cast<void>(extension);
                requireSource(context, snapshot, owner, token, HybridPropertySource::HPS_EXTENSION, memory, &*group);
                requireLocator<HybridPropertyFieldDirectoryMap>(context, snapshot, kind, 2,
                    HybridPropertyDirectoryCodec::ownerKey(owner, token), reference, memory);
                if (visitLarge && extension.mode() == HybridPropertyExtensionMode::HPEM_LARGE_REFERENCE) {
                    if (!largeValues)
                        throw FormatException(FormatError::FERR_INVALID_STATE,
                            "native reference enumeration requires its immutable value store");
                    // GC needs the complete immutable identity, not a decoded
                    // value body. The caller combines ALL ROW/native sources
                    // before retiring values and discards partial visits on any
                    // exception; the final publication still fences this pin.
                    utils::OwnedHeapReservation manifestCredit(memory);
                    const auto admitManifest = [&](size_t count) { context.check(); manifestCredit.set(count); };
                    const auto authenticated = largeValues->manifest(snapshot, *extension.reference(), std::ref(admitManifest));
                    static_cast<void>(authenticated);
                    context.check(); visitLarge(*extension.reference());
                }
            } else require(false, "native field page contains an unknown live record type");
            add(records, 1);
        });
        // Zero-live-slot pages may remain allocated for reuse after the last
        // group disappears; descriptor absence is only an error for live data.
        context.check(); return records;
    }
private:
    template<class Map>
    static void requireLocator(const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot,
        EntityPropertyOwnerKind kind, size_t role, const typename Map::Key &key,
        const SlottedSlotReference &reference, const std::shared_ptr<std::pmr::memory_resource> &memory) {
        require(context.hasRoot(kind, role), "native physical record has no directory namespace");
        const auto binding = context.bind(kind, role);
        Map map(binding.root.index_id, context.trees_, context.metadata_, binding);
        utils::OwnedHeapReservation boundary(memory);
        const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        const auto value = map.find(snapshot, key, std::ref(admit), context.cancellation_);
        require(value && HybridPropertyDirectoryCodec::decodeLocator(*value) == reference,
            "native live slot is not owned by its exact directory locator");
    }
    static constexpr size_t bound() { return (std::numeric_limits<size_t>::max)() - 1U; }
    static void require(bool value, const char *message) { hybrid_property_detail::require(value, message); }
    static void add(uint64_t &total, uint64_t value) {
        if (value > (std::numeric_limits<uint64_t>::max)() - total)
            throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "hybrid integrity count overflowed");
        total += value;
    }
    static void requireSource(const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot,
        EntityPropertyKey owner, uint32_t token, HybridPropertySource expected,
        const std::shared_ptr<std::pmr::memory_resource> &memory, const HybridPropertyGroupRecord *knownGroup = nullptr) {
        utils::OwnedHeapReservation boundary(memory);
        const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        std::optional<HybridPropertyGroupRecord> loaded;
        if (!knownGroup) { loaded = context.findGroup(owner, std::ref(admit)); knownGroup = loaded ? &*loaded : nullptr; }
        require(knownGroup != nullptr, "hybrid field body has no source group");
        const auto selected = context.selector(*knownGroup, owner, token, std::ref(admit));
        require(selected && *selected == expected, "hybrid field body has no matching authoritative selector");
    }
    static void inspectGroup(const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot,
        const HybridPropertyGroupRecord &group, const std::shared_ptr<std::pmr::memory_resource> &memory,
        LargeValueStore *largeValues, Report &report) {
        if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_ROW_REMAINDER) {
            inspectRemainderGroup(context, snapshot, group, memory, largeValues, report); return;
        }
        uint64_t ownerCount = 0, columnCount = 0, bodyCount = 0, extensionCount = 0;
        const uint64_t first = (group.group_id - 1U) * RSS_HYBRID_GROUP_CAPACITY + 1U;
        for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot) {
            context.check();
            const EntityPropertyKey owner{group.owner_kind, first + slot};
            if (!owner.isValid()) continue; // The last identifier group has fewer valid slots.
            utils::OwnedHeapReservation boundary(memory);
            const auto admit = [&](size_t bytes) { boundary.set(bytes); };
            const auto count = context.count(owner, std::ref(admit));
            require(bool(count), "hybrid integrity owner escaped its registered group");
            add(ownerCount, *count);
        }
        if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) {
            context.withColumnMembership(group, [&](const auto &matrix) {
                for (size_t i = 0; i < matrix.column_count; ++i) {
                    context.check();
                    const auto column = context.columnRecord(group, matrix.tokens[i], {});
                    require(column.reference == matrix.locators[i] && column.column.presence == matrix.presence(matrix.tokens[i]),
                        "compact column locator or presence contradicts its membership");
                    for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot) {
                        if (!(column.column.presence & (UINT64_C(1) << slot))) continue;
                        context.check();
                        requireSource(context, snapshot, {group.owner_kind, first + slot}, matrix.tokens[i],
                            HybridPropertySource::HPS_COLUMN, memory, &group);
                    }
                    add(columnCount, 1); add(bodyCount, uint64_t(std::popcount(column.column.presence)));
                }
            });
        }
        if (context.hasRoot(group.owner_kind, 1)) {
            const auto binding = context.bind(group.owner_kind, 1);
            HybridPropertyFieldDirectoryMap columns(binding.root.index_id, context.trees_, context.metadata_, binding);
            const auto prefix = HybridPropertyDirectoryCodec::groupKey(group.group_id);
            utils::OwnedHeapReservation boundary(memory);
            const auto admit = [&](size_t bytes) { boundary.set(bytes); };
            const auto inspect = [&](const HybridPropertyFieldDirectoryMap::Entry &entry) {
                context.check();
                require(group.mode != HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP,
                    "compact group has a competing column-directory entry");
                const auto key = HybridPropertyDirectoryCodec::decodeColumnKey(entry.key);
                require(key.group == group.group_id, "hybrid column integrity escaped its group prefix");
                const auto reference = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
                const auto page = context.fields_.read(snapshot, reference.page_id);
                const auto column = page.view().readColumn(reference, group, key.token, context.descriptor_->semantic_generation);
                for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot) {
                    if (!(column.presence & (UINT64_C(1) << slot))) continue;
                    context.check();
                    requireSource(context, snapshot, {group.owner_kind, first + slot}, key.token,
                        HybridPropertySource::HPS_COLUMN, memory, &group);
                }
                add(columnCount, 1); add(bodyCount, uint64_t(std::popcount(column.presence)));
                return true;
            };
            require(columns.visitPrefix(snapshot, prefix, bound(), std::ref(inspect), std::ref(admit), context.cancellation_),
                "hybrid column integrity scan was incomplete");
        }
        if (context.hasRoot(group.owner_kind, 2)) {
            const auto binding = context.bind(group.owner_kind, 2);
            HybridPropertyFieldDirectoryMap extensions(binding.root.index_id, context.trees_, context.metadata_, binding);
            for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY; ++slot) {
                context.check();
                const EntityPropertyKey owner{group.owner_kind, first + slot};
                if (!owner.isValid()) continue;
                const auto prefix = HybridPropertyDirectoryCodec::ownerKey(owner, 1);
                utils::OwnedHeapReservation boundary(memory);
                const auto admit = [&](size_t bytes) { boundary.set(bytes); };
                const auto inspect = [&](const HybridPropertyFieldDirectoryMap::Entry &entry) {
                    context.check();
                    const auto key = HybridPropertyDirectoryCodec::decodeOwnerKey(entry.key, group.owner_kind);
                    require(key.owner == owner, "hybrid extension integrity escaped its owner prefix");
                    requireSource(context, snapshot, owner, key.token, HybridPropertySource::HPS_EXTENSION, memory, &group);
                    const auto reference = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
                    const auto page = context.fields_.read(snapshot, reference.page_id);
                    utils::OwnedHeapReservation referenceCredit(memory);
                    const auto admitReference = [&](size_t bytes) { referenceCredit.set(bytes); };
                    const auto value = page.view().readExtension(reference, owner, key.token, group,
                        context.descriptor_->semantic_generation, std::ref(admitReference));
                    if (value.mode() == HybridPropertyExtensionMode::HPEM_INLINE) {
                        size_t offset = 0;
                        const auto checkpoint = [&] { context.check(); };
                        static_cast<void>(CanonicalPropertyValueCodec::inspectNext(value.payload(), offset, std::ref(checkpoint)));
                        require(offset == value.payload().size(), "hybrid inline integrity found trailing bytes");
                    } else {
                        if (!largeValues)
                            throw FormatException(FormatError::FERR_INVALID_STATE, "hybrid integrity requires its immutable value store");
                        HybridPropertyLargeFieldInspection inspection(context, snapshot, value, *largeValues, memory);
                        static_cast<void>(inspection.inspect());
                        add(report.large_values, 1);
                    }
                    add(extensionCount, 1); add(bodyCount, 1); return true;
                };
                require(extensions.visitPrefix(snapshot, std::span(prefix).first(8), bound(), std::ref(inspect),
                    std::ref(admit), context.cancellation_), "hybrid extension integrity scan was incomplete");
            }
        }
        require(ownerCount == group.memberships && bodyCount == ownerCount && columnCount == group.columns,
            "hybrid group counts or selector/body ownership disagree");
        const auto columnSelectors = group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP ?
            context.withColumnMembership(group, [](const auto &matrix) { return uint64_t(matrix.memberships()); }) : 0U;
        require(columnSelectors <= ownerCount, "compact selector count exceeds actual group memberships");
        add(report.directory_selectors, ownerCount - columnSelectors);
        add(report.memberships, ownerCount); add(report.columns, columnCount); add(report.extensions, extensionCount);
    }
    struct ColdCounts { uint64_t fields = 0, rows = 0, large_values = 0; };
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
    static uint64_t countOwners(const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot,
        EntityPropertyOwnerKind kind, const std::shared_ptr<std::pmr::memory_resource> &memory) {
        if (!context.hasRoot(kind, 0)) return 0;
        const auto map = context.ownerMap(kind);
        utils::OwnedHeapReservation boundary(memory);
        const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        uint64_t count = 0;
        const auto inspect = [&](const HybridPropertyOwnerMap::Entry &entry) {
            context.check(); static_cast<void>(HybridPropertyDirectoryCodec::decodeOwnerKey(entry.key, kind));
            static_cast<void>(HybridPropertyDirectoryCodec::decodeSource(entry.value)); add(count, 1); return true;
        };
        require(map.visitAll(snapshot, bound(), std::ref(inspect), std::ref(admit), context.cancellation_),
            "hybrid global owner integrity scan was incomplete"); return count;
    }
    static uint64_t countFields(const HybridPropertyReadContext &context, const MetadataSnapshotPin &snapshot,
        EntityPropertyOwnerKind kind, size_t role, const std::shared_ptr<std::pmr::memory_resource> &memory) {
        if (!context.hasRoot(kind, role)) return 0;
        const auto binding = context.bind(kind, role);
        HybridPropertyFieldDirectoryMap map(binding.root.index_id, context.trees_, context.metadata_, binding);
        utils::OwnedHeapReservation boundary(memory);
        const auto admit = [&](size_t bytes) { boundary.set(bytes); };
        uint64_t count = 0;
        const auto inspect = [&](const HybridPropertyFieldDirectoryMap::Entry &entry) {
            context.check();
            if (role == 1) static_cast<void>(HybridPropertyDirectoryCodec::decodeColumnKey(entry.key));
            else static_cast<void>(HybridPropertyDirectoryCodec::decodeOwnerKey(entry.key, kind));
            static_cast<void>(HybridPropertyDirectoryCodec::decodeLocator(entry.value)); add(count, 1); return true;
        };
        require(map.visitAll(snapshot, bound(), std::ref(inspect), std::ref(admit), context.cancellation_),
            "hybrid global field integrity scan was incomplete"); return count;
    }
};
} // namespace graph::storage
