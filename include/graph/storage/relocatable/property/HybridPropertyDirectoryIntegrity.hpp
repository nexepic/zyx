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
        uint64_t groups = 0, memberships = 0, columns = 0, extensions = 0, large_values = 0, directory_selectors = 0;
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
                add(local.groups, 1);
                return true;
            };
            require(groups.visitAll(snapshot, bound(), std::ref(inspect), std::ref(admitGroup), context.cancellation_),
                "hybrid group integrity scan was incomplete");
            // Every locally inspected body is required to own its selector.
            // Equal global cardinalities then exclude orphan directory entries
            // without retaining a database-sized key or ownership set.
            require(countOwners(context, snapshot, kind, memory) == local.directory_selectors &&
                countFields(context, snapshot, kind, 1, memory) == local.columns &&
                countFields(context, snapshot, kind, 2, memory) == local.extensions,
                "hybrid directories contain entries outside their group ownership");
            add(report.groups, local.groups); add(report.memberships, local.memberships);
            add(report.directory_selectors, local.directory_selectors);
            add(report.columns, local.columns); add(report.extensions, local.extensions); add(report.large_values, local.large_values);
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
                if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) {
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
                const auto group = context.findGroup(owner, [&](size_t count) { boundary.set(count); });
                require(group && group->mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP &&
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
                const auto group = context.findGroup(firstOwner, std::ref(admit));
                require(bool(group), "native physical column has no registered group");
                column.validateInGroup(*group, context.descriptor_->semantic_generation, snapshot.superblock().generation);
                requireLocator<HybridPropertyFieldDirectoryMap>(context, snapshot, kind, 1,
                    HybridPropertyDirectoryCodec::columnKey(identity.group_id, identity.token), reference, memory);
            } else if (magic == RSS_HYBRID_EXTENSION_MAGIC) {
                const EntityPropertyKey owner{kind, LittleEndianCodec::readU64(bytes, 8)};
                const auto token = LittleEndianCodec::readU32(bytes, 16);
                utils::OwnedHeapReservation boundary(memory), referenceCredit(memory);
                const auto admit = [&](size_t count) { boundary.set(count); };
                const auto group = context.findGroup(owner, std::ref(admit));
                require(group && group->memberships && context.isLive(owner, *group),
                    "native physical extension has no registered live owner");
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
        if (context.hasRoot(group.owner_kind, 1)) {
            const auto binding = context.bind(group.owner_kind, 1);
            HybridPropertyFieldDirectoryMap columns(binding.root.index_id, context.trees_, context.metadata_, binding);
            const auto prefix = HybridPropertyDirectoryCodec::groupKey(group.group_id);
            utils::OwnedHeapReservation boundary(memory);
            const auto admit = [&](size_t bytes) { boundary.set(bytes); };
            const auto inspect = [&](const HybridPropertyFieldDirectoryMap::Entry &entry) {
                context.check();
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
