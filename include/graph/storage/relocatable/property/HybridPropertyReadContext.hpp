/**
 * @file HybridPropertyReadContext.hpp
 * @brief Pinned descriptor, directory and field access for the hybrid candidate.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/utils/OwnedPmrObject.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include <algorithm>
#include <type_traits>
#include <limits>
#include <vector>
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectory.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldPageStore.hpp"

namespace graph::storage {
class HybridPropertyReadContextTestAccess;
class HybridPropertyDirectoryIntegrity;
// Snapshot-local, with borrowed caller-owned page store and pin. Normal opening
// reads the actual physical descriptor root in the isolated native format.
// Decoded metadata stays within this source and its original resource owner.
// Callbacks borrow fields only during invocation; outputs must own their values.
class HybridPropertyReadContext final {
public:
    enum class FieldRoute : uint8_t { HFR_ROW = 1, HFR_MISSING, HFR_FOUND };
    struct Enumeration { bool routed = false; bool owner_live = false; bool complete = true; size_t fields = 0; };
    using ScalarVisitor = std::function<void(const HybridPropertyFieldView::ScalarCell &)>;
    using ExtensionVisitor = std::function<void(const HybridPropertyExtensionView &)>;
    using ColumnVisitor = std::function<void(const HybridPropertyGroupRecord &, const HybridPropertyColumnRecord &,
        const SlottedSlotReference &, const SlottedSlotReference &)>;
    HybridPropertyReadContext(std::shared_ptr<StorageIO> io, SlottedPagePool &pages,
        RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
        const CancellationCheckpoint *cancellation = nullptr)
        : HybridPropertyReadContext(io, pages, metadata, snapshot, load(io, metadata, snapshot), cancellation) {}
    // Typed maintenance can borrow the already authenticated page store's file
    // without acquiring raw I/O authority or constructing a second allocator.
    HybridPropertyReadContext(SlottedPagePool &pages, RelocatableMetadataStore &metadata,
        const MetadataSnapshotPin &snapshot, const CancellationCheckpoint *cancellation = nullptr)
        : HybridPropertyReadContext(pages.pool_.io_, pages, metadata, snapshot, cancellation) {}
    HybridPropertyReadContext(const HybridPropertyReadContext &) = delete;
    HybridPropertyReadContext &operator=(const HybridPropertyReadContext &) = delete;
    HybridPropertyReadContext(HybridPropertyReadContext &&) noexcept = default;

    [[nodiscard]] bool usesSnapshot(const MetadataSnapshotPin &snapshot) const {
        return &snapshot == &snapshot_ && metadata_.ownsSnapshot(snapshot) && snapshot.superblock().generation == generation_;
    }
    void checkpoint() const { check(); }
    // A full-entity scan may retain one bounded group worth of columns, rather
    // than thrashing the small selected-field window for each successive owner.
    // It uses the same cache/account, fills only requested fields, and remains
    // optional under pressure. There is no second ROW image or prefetch.
    void enableFullEntityWindow(size_t actualFields) const {
        check();
        const auto desired = (std::min)((std::max)(actualFields, size_t{8}), HybridPropertyColumnMembership::MAX_COLUMNS);
        if (desired <= full_entity_columns_) return;
        full_entity_columns_ = desired;
        if (membership_cache_) {
            try { membership_cache_->columns.resize(desired); }
            catch (const query::execution::MaterializedByteLimitExceeded &) {}
            catch (const query::execution::detail::ResourceAdmissionDenied &) {}
        }
    }
    [[nodiscard]] bool hasNativeGroups() const {
        check(); return hasRoot(EntityPropertyOwnerKind::EPOK_NODE, 3) || hasRoot(EntityPropertyOwnerKind::EPOK_EDGE, 3);
    }
    // Fixed-size authenticated source for bounded inverse conversion. This
    // grants no write authority; the caller must retain/fence the same pin.
    [[nodiscard]] std::optional<HybridPropertyGroupRecord> groupRecord(EntityPropertyKey owner,
        const std::function<void(size_t)> &boundaryAdmission = {}, SlottedSlotReference *reference = nullptr) const {
        return findGroup(owner, boundaryAdmission, reference);
    }
    // Next explicit-selector owner for immutable-value maintenance. Compact
    // column-only owners have primitive values and need no LargeValue visit.
    [[nodiscard]] std::optional<EntityPropertyKey> nextOwnerAfter(EntityPropertyOwnerKind kind, uint64_t afterOwnerId,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        check(); require(hybrid_property_detail::validKind(kind) && afterOwnerId <= RSS_HYBRID_MAX_ENTITY_ID,
            "hybrid owner seek has an invalid namespace or cursor");
        if (!hasRoot(kind, 0)) return {};
        HybridPropertyOwnerMap::Key first{};
        if (afterOwnerId == RSS_HYBRID_MAX_ENTITY_ID) first[0] = std::byte{0x80};
        else if (afterOwnerId) {
            first = HybridPropertyDirectoryCodec::ownerKey({kind, afterOwnerId + 1U}, 1);
            std::fill(first.begin() + 8, first.end(), std::byte{0});
        }
        std::optional<EntityPropertyKey> result;
        const auto map = ownerMap(kind);
        static_cast<void>(map.visitFrom(snapshot_, first, 1, [&](const auto &entry) {
            const auto field = HybridPropertyDirectoryCodec::decodeOwnerKey(entry.key, kind);
            static_cast<void>(HybridPropertyDirectoryCodec::decodeSource(entry.value));
            require(field.owner.entity_id > afterOwnerId, "hybrid owner seek did not advance");
            result = field.owner; return false;
        }, boundaryAdmission, cancellation_));
        return result;
    }
    [[nodiscard]] Enumeration visitOwner(EntityPropertyKey owner, size_t maximumEntries,
        const std::function<bool(uint32_t, HybridPropertySource)> &visitor,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        require(maximumEntries && visitor, "hybrid enumeration requires a positive bound and visitor");
        const auto group = findGroup(owner, boundaryAdmission);
        if (!group) return {};
        const bool live = isLive(owner, *group);
        Enumeration result{true, live, true, 0};
        const auto checked = [&](uint32_t token, HybridPropertySource source) {
            check();
            if (result.fields == maximumEntries) return false;
            require(live && result.fields < group->memberships,
                "hybrid owner membership contradicts group liveness or count");
            require(hasRoot(owner.owner_kind, source == HybridPropertySource::HPS_COLUMN ? 1U : 2U),
                "hybrid owner membership has no source directory");
            ++result.fields; return visitor(token, source);
        };
        if (group->mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) {
            std::array<uint32_t, HybridPropertyColumnMembership::MAX_COLUMNS> columns{}; size_t count = 0, position = 0;
            withColumnMembership(*group, [&](const auto &matrix) {
                require(matrix.visitOwner(owner, columns.size(), [&](uint32_t token) { columns[count++] = token; return true; }),
                    "compact owner exceeds its bounded column tokens");
            });
            const auto remaining = [&] {
                while (position < count)
                    if (!checked(columns[position++], HybridPropertySource::HPS_COLUMN)) return false;
                return true;
            };
            if (hasRoot(owner.owner_kind, 0)) {
                const auto map = ownerMap(owner.owner_kind);
                const auto extension = [&](uint32_t token, HybridPropertySource source) {
                    require(source == HybridPropertySource::HPS_EXTENSION, "compact owner has a repeated legacy column selector");
                    while (position < count && columns[position] < token)
                        if (!checked(columns[position++], HybridPropertySource::HPS_COLUMN)) return false;
                    require(position == count || columns[position] != token, "compact owner has two field authorities");
                    return checked(token, source);
                };
                result.complete = HybridPropertyDirectoryCodec::visitOwner(map, snapshot_, owner, maximumEntries,
                    std::ref(extension), boundaryAdmission, cancellation_);
            }
            if (result.complete) result.complete = remaining();
        } else if (hasRoot(owner.owner_kind, 0)) {
            const auto map = ownerMap(owner.owner_kind);
            result.complete = HybridPropertyDirectoryCodec::visitOwner(map, snapshot_, owner, maximumEntries,
                std::ref(checked), boundaryAdmission, cancellation_);
        } else require(group->memberships == 0, "hybrid group memberships have no owner directory");
        return result;
    }
    // A disengaged result means this owner still follows ROW routing. Counts
    // and tokens authenticate memberships, not unvisited field/LargeValue bodies.
    [[nodiscard]] std::optional<size_t> count(EntityPropertyKey owner,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        const auto result = visitOwner(owner, (std::numeric_limits<size_t>::max)() - 1U,
            [](uint32_t, HybridPropertySource) { return true; }, boundaryAdmission);
        if (!result.routed) return {};
        if (!result.complete)
            throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "hybrid membership count exceeded its native bound");
        return result.fields;
    }
    [[nodiscard]] std::optional<std::vector<uint32_t>> tokens(EntityPropertyKey owner,
        const SelectedPropertyReadAdmission &outputAdmission,
        const SelectedPropertyReadAdmission &transientAdmission) const {
        require(outputAdmission && transientAdmission, "hybrid token enumeration requires retained output and transient admission");
        const auto size = count(owner, transientAdmission);
        transientAdmission(0);
        if (!size) return {};
        if (*size > (std::numeric_limits<size_t>::max)() / sizeof(uint32_t))
            throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW, "hybrid token output capacity overflowed");
        outputAdmission(*size * sizeof(uint32_t));
        std::vector<uint32_t> result;
        result.reserve(*size); outputAdmission(result.capacity() * sizeof(uint32_t));
        const auto append = [&](uint32_t token, HybridPropertySource) { result.push_back(token); return true; };
        const auto enumeration = visitOwner(owner, *size ? *size : 1U, std::ref(append), transientAdmission);
        require(enumeration.routed && enumeration.complete && enumeration.fields == *size,
            "hybrid owner membership changed within its pinned token enumeration");
        transientAdmission(0);
        return result;
    }
    [[nodiscard]] FieldRoute visitField(EntityPropertyKey owner, uint32_t token,
        const ScalarVisitor &scalar, const ExtensionVisitor &extension,
        const std::function<void(size_t)> &boundaryAdmission = {},
        const HybridPropertyExtensionView::ReferenceAdmission &referenceAdmission = {},
        const ColumnVisitor &columnVisitor = {}) const {
        require(token && scalar && extension, "hybrid field read requires a token and typed visitors");
        SlottedSlotReference groupReference;
        const auto group = findGroup(owner, boundaryAdmission, &groupReference);
        if (!group) return FieldRoute::HFR_ROW;
        const auto key = HybridPropertyDirectoryCodec::ownerKey(owner, token);
        const auto selected = selector(*group, owner, token, boundaryAdmission);
        if (!selected) return FieldRoute::HFR_MISSING;
        require(isLive(owner, *group) && group->memberships,
            "hybrid field membership belongs to a dead or empty owner group");
        const auto source = *selected;
        const size_t role = source == HybridPropertySource::HPS_COLUMN ? 1U : 2U;
        require(hasRoot(owner.owner_kind, role), "hybrid member source directory is absent");
        if (role == 1) {
            if (!columnVisitor) if (const auto *cached = cachedColumn(*group, token)) {
                const auto slot = hybridPropertySlotFor(owner); const auto bit = UINT64_C(1) << slot;
                require(bool(cached->column.presence & bit), "hybrid column membership has no present value");
                // Copy the selected bits before calling user code. Reentrancy
                // may resize/evict the cache; no borrowed column escapes.
                const HybridPropertyFieldView::ScalarCell cell{cached->column.identity.scalar_kind,
                    true, bool(cached->column.nulls & bit), cached->column.words[slot]};
                debug::PerfTrace::addValue("native_graph.hybrid.column_cache_hits", 1);
                scalar(cell); return FieldRoute::HFR_FOUND;
            }
            const auto selectedColumn = columnRecord(*group, token, boundaryAdmission);
            const auto &column = selectedColumn.column;
            const auto slot = hybridPropertySlotFor(owner); const auto bit = UINT64_C(1) << slot;
            require(bool(column.presence & bit), "hybrid column membership has no present value");
            if (columnVisitor) columnVisitor(*group, column, groupReference, selectedColumn.reference);
            scalar({column.identity.scalar_kind, true, bool(column.nulls & bit), column.words[slot]});
        } else {
            const auto binding = bind(owner.owner_kind, role);
            HybridPropertyFieldDirectoryMap fields(binding.root.index_id, trees_, metadata_, binding);
            const auto locator = fields.find(snapshot_, key, boundaryAdmission, cancellation_);
            require(bool(locator), "hybrid field membership has no source record");
            const auto reference = HybridPropertyDirectoryCodec::decodeLocator(*locator);
            const auto page = fields_.read(snapshot_, reference.page_id);
            check();
            const auto value = page.view().readExtension(reference, owner, token, *group,
                descriptor_->semantic_generation, referenceAdmission);
            extension(value);
        }
        check();
        return FieldRoute::HFR_FOUND;
    }
private:
#if ZYX_EXPERIMENTAL_HYBRID_PROPERTY_FORMAT
    friend class HybridPropertyValueReader;
    // The token list comes from this context's authenticated membership walk.
    // Only a pure compact owner containing every group column can use a single
    // bounded directory walk. Mixed/sparse owners retain the point boundary.
    [[nodiscard]] bool fullColumnCells(EntityPropertyKey owner, std::span<const uint32_t> tokens,
        std::span<HybridPropertyFieldView::ScalarCell> cells,
        const std::function<void(size_t)> &admission) const {
        check();
        if (tokens.empty() || tokens.size() > HybridPropertyColumnMembership::MAX_COLUMNS ||
            StorageIO::SequentialReadScope::activeFor(*membership_io_)) return false;
        const auto epoch = membership_io_->dataMutationEpoch();
        if (epoch == (std::numeric_limits<uint64_t>::max)()) return false;
        const auto group = findGroup(owner, admission);
        if (!group || group->mode != HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP ||
            hasRoot(owner.owner_kind, 0) || hasRoot(owner.owner_kind, 2) || group->columns != tokens.size()) return false;
        require(cells.size() >= tokens.size() && isLive(owner, *group), "invalid full-column owner output");
        const auto slot = hybridPropertySlotFor(owner);
        const auto bit = UINT64_C(1) << slot;
        const auto copy = [&](size_t i, const HybridPropertyColumnRecord &column) {
            require(bool(column.presence & bit), "full-column membership has no present value");
            cells[i] = {column.identity.scalar_kind, true, bool(column.nulls & bit), column.words[slot]};
        };
        size_t cached = 0;
        for (; cached < tokens.size(); ++cached) {
            const auto *column = cachedColumn(*group, tokens[cached]);
            if (!column) break;
            copy(cached, column->column);
        }
        if (cached != tokens.size()) {
            debug::PerfTrace::addValue("native_graph.hybrid.full_column_directory_walks", 1);
            const auto binding = bind(owner.owner_kind, 1);
            HybridPropertyFieldDirectoryMap directory(binding.root.index_id, trees_, metadata_, binding);
            std::optional<HybridPropertyFieldPage> page;
            PageId pageId{};
            size_t count = 0;
            const auto visit = [&](const HybridPropertyFieldDirectoryMap::Entry &entry) {
                check();
                require(count < tokens.size() && entry.key ==
                    HybridPropertyDirectoryCodec::columnKey(group->group_id, tokens[count]),
                    "full-column directory disagrees with owner membership");
                const auto reference = HybridPropertyDirectoryCodec::decodeLocator(entry.value);
                if (!page || pageId != reference.page_id) {
                    page.reset();
                    page.emplace(fields_.read(snapshot_, reference.page_id));
                    pageId = reference.page_id;
                }
                const AuthenticatedColumn value{page->view().readColumn(reference, *group, tokens[count],
                    descriptor_->semantic_generation), reference};
                copy(count++, value.column);
                debug::PerfTrace::addValue("native_graph.hybrid.column_decodes", 1);
                prepareReadCache();
                if (membership_cache_) {
                    auto &destination = membership_cache_->columns[membership_cache_->column_cursor++ % membership_cache_->columns.size()];
                    destination.group = *group; destination.value = value;
                    destination.io_epoch = epoch; destination.valid = true;
                }
                return count < tokens.size();
            };
            static_cast<void>(directory.visitFrom(snapshot_,
                HybridPropertyDirectoryCodec::columnKey(group->group_id, tokens.front()), tokens.size(),
                std::ref(visit), admission, cancellation_));
            require(count == tokens.size(), "full-column directory is incomplete");
        }
        check();
        const bool unchanged = membership_io_->dataMutationEpoch() == epoch;
        if (unchanged) debug::PerfTrace::addValue("native_graph.hybrid.full_column_batches", 1);
        return unchanged;
    }
#endif
    friend class HybridPropertyReadContextTestAccess;
    friend class HybridPropertyDirectoryIntegrity;
    friend class HybridPropertyDemotionPreparation;
    friend class HybridPropertyColumnDelta;
    friend class RelocatableMetadataStore;
    HybridPropertyReadContext(std::shared_ptr<StorageIO> io, SlottedPagePool &pages,
        RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
        std::optional<HybridPropertyDescriptor> descriptor, const CancellationCheckpoint *cancellation)
        : snapshot_(snapshot), metadata_(metadata), generation_(snapshot.superblock().generation),
          trees_(pages, metadata), membership_io_(io.get()), fields_(std::move(io), metadata, snapshot),
          descriptor_(std::move(descriptor)), cancellation_(cancellation),
          membership_memory_owner_(utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() :
              (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr)) {
        require(metadata_.ownsSnapshot(snapshot_), "hybrid read context requires its own live pin");
        if (descriptor_) require(descriptor_->database_id == snapshot_.superblock().database_id &&
            descriptor_->semantic_generation.isValid() && descriptor_->semantic_generation <= snapshot_.superblock().generation,
            "hybrid read descriptor is foreign or not visible");
    }
    static std::optional<HybridPropertyDescriptor> load(const std::shared_ptr<StorageIO> &io,
        RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot) {
        require(metadata.ownsSnapshot(snapshot) && metadata.usesDataStorage(io), "hybrid descriptor read uses a foreign pin or file");
        if (!snapshot.superblock().hybrid_property_descriptor_root.isValid()) return {};
        MetadataPageFile pages(io, snapshot.superblock());
        return pages.readHybridPropertyDescriptor(snapshot.superblock().hybrid_property_descriptor_root,
            snapshot.superblock()).descriptor;
    }
    void check() const {
        require(metadata_.ownsSnapshot(snapshot_) && snapshot_.superblock().generation == generation_,
            "hybrid read context requires its original live metadata generation");
        if (cancellation_) cancellation_->check();
    }
    static void require(bool value, const char *message) { hybrid_property_detail::require(value, message); }
    static size_t base(EntityPropertyOwnerKind kind) {
        require(hybrid_property_detail::validKind(kind), "hybrid owner namespace is invalid");
        return kind == EntityPropertyOwnerKind::EPOK_NODE ? 0U : 4U;
    }
    bool hasRoot(EntityPropertyOwnerKind kind, size_t role) const {
        return descriptor_ && descriptor_->roots[base(kind) + role].page_id.isValid();
    }
    BTreeRootReadBinding bind(EntityPropertyOwnerKind kind, size_t role) const {
        return HybridPropertyDirectoryCodec::bindRoot(*descriptor_, snapshot_,
            static_cast<HybridPropertyRootOrdinal>(base(kind) + role));
    }
    HybridPropertyOwnerMap ownerMap(EntityPropertyOwnerKind kind) const {
        const auto binding = bind(kind, 0);
        return HybridPropertyOwnerMap(binding.root.index_id, trees_, metadata_, binding);
    }
    static bool isLive(EntityPropertyKey owner, const HybridPropertyGroupRecord &group) {
        return group.live & (UINT64_C(1) << hybridPropertySlotFor(owner));
    }
    std::optional<HybridPropertyGroupRecord> findGroup(EntityPropertyKey owner,
        const std::function<void(size_t)> &boundaryAdmission, SlottedSlotReference *source = nullptr) const {
        check(); require(owner.isValid(), "hybrid group lookup requires a valid owner");
        if (!hasRoot(owner.owner_kind, 3)) {
            require(!hasRoot(owner.owner_kind, 0) && !hasRoot(owner.owner_kind, 1) && !hasRoot(owner.owner_kind, 2),
                "hybrid descriptor has field directories without a group directory");
            return {};
        }
        const auto epoch = membership_io_->dataMutationEpoch();
        const bool cacheable = epoch != (std::numeric_limits<uint64_t>::max)() &&
            !StorageIO::SequentialReadScope::activeFor(*membership_io_);
        const auto groupId = hybridPropertyGroupFor(owner);
        const auto ownerBit = UINT64_C(1) << hybridPropertySlotFor(owner);
        std::optional<HybridPropertyGroupRecord> decoded;
        SlottedSlotReference reference;
        uint64_t authenticatedOwners = 0;
        if (cacheable && membership_cache_) for (const auto &entry : membership_cache_->groups)
            if (entry.valid && entry.io_epoch == epoch && entry.group.owner_kind == owner.owner_kind && entry.group.group_id == groupId) {
                decoded = entry.group; reference = entry.reference; authenticatedOwners = entry.authenticated_owners;
                debug::PerfTrace::addValue("native_graph.hybrid.group_cache_hits", 1); break;
            }
        if (!decoded) {
            const auto binding = bind(owner.owner_kind, 3);
            HybridPropertyGroupDirectoryMap groups(binding.root.index_id, trees_, metadata_, binding);
            const auto locator = groups.find(snapshot_, HybridPropertyDirectoryCodec::groupKey(groupId), boundaryAdmission, cancellation_);
            if (!locator) return {};
            reference = HybridPropertyDirectoryCodec::decodeLocator(*locator);
            const auto page = fields_.read(snapshot_, reference.page_id);
            decoded = page.view().readGroup(reference, owner.owner_kind, groupId, descriptor_->semantic_generation);
            require(decoded->mode != HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP ||
                (snapshot_.superblock().flags & RSS_SUPERBLOCK_FLAG_HYBRID_COLUMN_MEMBERSHIP),
                "compact group is missing its native format feature");
            debug::PerfTrace::addValue("native_graph.hybrid.group_decodes", 1);
        }
        if (!(authenticatedOwners & ownerBit)) {
            const auto entity = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ?
                metadata_.findNodePageLocationOptional(snapshot_, owner.entity_id) :
                metadata_.findEdgePageLocationOptional(snapshot_, owner.entity_id);
            require(bool(entity) == isLive(owner, *decoded), "hybrid group liveness disagrees with the Entity Map");
            require(!metadata_.findEntityPropertyLocationOptional(snapshot_, owner), "hybrid group owner still has a competing ROW root");
            authenticatedOwners |= ownerBit;
            debug::PerfTrace::addValue("native_graph.hybrid.owner_route_checks", 1);
        }
        if (cacheable) {
            prepareReadCache();
            if (membership_cache_) {
                auto *destination = &membership_cache_->groups[membership_cache_->group_cursor % membership_cache_->groups.size()];
                bool found = false;
                for (auto &entry : membership_cache_->groups)
                    if (entry.valid && entry.io_epoch == epoch && entry.group == *decoded) { destination = &entry; found = true; break; }
                if (!found) ++membership_cache_->group_cursor;
                destination->group = *decoded; destination->reference = reference;
                destination->authenticated_owners = authenticatedOwners; destination->io_epoch = epoch; destination->valid = true;
            }
        }
        if (source) *source = reference;
        return decoded;
    }
    [[nodiscard]] std::optional<HybridPropertySource> selector(const HybridPropertyGroupRecord &group,
        EntityPropertyKey owner, uint32_t token, const std::function<void(size_t)> &admission = {}) const {
        check(); require(owner.owner_kind == group.owner_kind && hybridPropertyGroupFor(owner) == group.group_id,
            "hybrid selector has a foreign group or owner");
        const auto key = HybridPropertyDirectoryCodec::ownerKey(owner, token);
        std::optional<HybridPropertyOwnerMap::Value> explicitSource;
        if (hasRoot(owner.owner_kind, 0)) explicitSource = ownerMap(owner.owner_kind).find(snapshot_, key, admission, cancellation_);
        if (group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP) {
            const bool column = withColumnMembership(group, [&](const auto &matrix) { return matrix.contains(owner, token); });
            if (explicitSource) require(!column && HybridPropertyDirectoryCodec::decodeSource(*explicitSource) == HybridPropertySource::HPS_EXTENSION,
                "compact field has two authorities or a legacy column selector");
            if (column) return HybridPropertySource::HPS_COLUMN;
        } else if (!hasRoot(owner.owner_kind, 0))
            require(!group.memberships, "hybrid group memberships have no owner directory");
        if (!explicitSource) return {};
        return HybridPropertyDirectoryCodec::decodeSource(*explicitSource);
    }
    struct AuthenticatedColumn {
        HybridPropertyColumnRecord column;
        SlottedSlotReference reference;
    };
    struct MembershipCache final : utils::OwnedPmrObject {
        struct GroupEntry {
            HybridPropertyGroupRecord group;
            SlottedSlotReference reference;
            uint64_t authenticated_owners = 0;
            uint64_t io_epoch = 0;
            bool valid = false;
        };
        std::array<GroupEntry, 2> groups{};
        size_t group_cursor = 0;
        struct Entry {
            HybridPropertyGroupRecord group;
            HybridPropertyColumnMembership value;
            bool valid = false;
            uint64_t io_epoch = 0;
        };
        std::array<Entry, 2> entries{};
        size_t cursor = 0;
        struct ColumnEntry {
            HybridPropertyGroupRecord group;
            AuthenticatedColumn value;
            uint64_t io_epoch = 0;
            bool valid = false;
        };
        explicit MembershipCache(std::pmr::memory_resource *memory, size_t count) : columns(memory) { columns.resize(count); }
        std::pmr::vector<ColumnEntry> columns;
        size_t column_cursor = 0;
        size_t column_lookup_cursor = 0;
    };
    void prepareReadCache() const {
        if (membership_cache_ || membership_cache_disabled_) return;
        if (!membership_memory_owner_) membership_cache_disabled_ = true;
        else try { membership_cache_.reset(new (membership_memory_owner_) MembershipCache(membership_memory_owner_.get(),
            full_entity_columns_)); }
        catch (const query::execution::MaterializedByteLimitExceeded &) { membership_cache_disabled_ = true; }
        catch (const query::execution::detail::ResourceAdmissionDenied &) { membership_cache_disabled_ = true; }
    }
    [[nodiscard]] const AuthenticatedColumn *cachedColumn(const HybridPropertyGroupRecord &group, uint32_t token) const {
        check();
        const auto epoch = membership_io_->dataMutationEpoch();
        if (epoch == (std::numeric_limits<uint64_t>::max)() || StorageIO::SequentialReadScope::activeFor(*membership_io_) ||
            !membership_cache_) return nullptr;
        // Full collections visit their actual tokens in the same order for
        // successive owners. Continue after the previous hit so a retained
        // F-field window does not incur F linear probes for every field.
        size_t ordinal = membership_cache_->column_lookup_cursor;
        for (size_t checked = 0; checked < membership_cache_->columns.size(); ++checked) {
            const auto &entry = membership_cache_->columns[ordinal];
            if (++ordinal == membership_cache_->columns.size()) ordinal = 0;
            if (entry.valid && entry.io_epoch == epoch && entry.group == group && entry.value.column.identity.token == token) {
                membership_cache_->column_lookup_cursor = ordinal;
                return &entry.value;
            }
        }
        return nullptr;
    }
    [[nodiscard]] AuthenticatedColumn columnRecord(const HybridPropertyGroupRecord &group, uint32_t token,
        const std::function<void(size_t)> &admission) const {
        check();
        const auto epoch = membership_io_->dataMutationEpoch();
        const bool cacheable = epoch != (std::numeric_limits<uint64_t>::max)() &&
            !StorageIO::SequentialReadScope::activeFor(*membership_io_);
        // Column callbacks retain an owning record across reentrant eviction.
        if (const auto *cached = cachedColumn(group, token)) {
            const auto value = *cached;
            debug::PerfTrace::addValue("native_graph.hybrid.column_cache_hits", 1);
            return value;
        }
        const auto binding = bind(group.owner_kind, 1);
        HybridPropertyFieldDirectoryMap directory(binding.root.index_id, trees_, metadata_, binding);
        const auto locator = directory.find(snapshot_, HybridPropertyDirectoryCodec::columnKey(group.group_id, token),
            admission, cancellation_);
        require(bool(locator), "hybrid field membership has no source record");
        const auto reference = HybridPropertyDirectoryCodec::decodeLocator(*locator);
        const auto page = fields_.read(snapshot_, reference.page_id);
        check();
        const AuthenticatedColumn value{page.view().readColumn(reference, group, token, descriptor_->semantic_generation), reference};
        debug::PerfTrace::addValue("native_graph.hybrid.column_decodes", 1);
        if (cacheable) {
            prepareReadCache();
            if (membership_cache_) {
                auto &entry = membership_cache_->columns[membership_cache_->column_cursor++ % membership_cache_->columns.size()];
                entry.group = group; entry.value = value; entry.io_epoch = epoch; entry.valid = true;
            }
        }
        return value;
    }
    template<class Visitor>
    auto withColumnMembership(const HybridPropertyGroupRecord &group, Visitor &&visitor) const
        -> std::invoke_result_t<Visitor &, const HybridPropertyColumnMembership &> {
        check(); require(group.mode == HybridPropertyGroupMode::HPGM_COLUMN_MEMBERSHIP_BITMAP &&
            (snapshot_.superblock().flags & RSS_SUPERBLOCK_FLAG_HYBRID_COLUMN_MEMBERSHIP) && hasRoot(group.owner_kind, 1),
            "compact membership has no admitted source feature or column directory");
        const auto epoch = membership_io_->dataMutationEpoch();
        const bool cacheable = epoch != (std::numeric_limits<uint64_t>::max)() &&
            !StorageIO::SequentialReadScope::activeFor(*membership_io_);
        if (cacheable && membership_cache_) for (const auto &entry : membership_cache_->entries)
            if (entry.valid && entry.io_epoch == epoch && entry.group == group) {
                debug::PerfTrace::addValue("native_graph.hybrid.column_membership_cache_hits", 1);
                return visitor(std::as_const(entry.value));
            }
        const auto page = fields_.read(snapshot_, group.column_membership.page_id);
        const auto decoded = page.view().readColumnMembership(group.column_membership, group, descriptor_->semantic_generation);
        require(hasRoot(group.owner_kind, 0) || decoded.memberships() == group.memberships,
            "compact group claims extensions without an owner directory");
        debug::PerfTrace::addValue("native_graph.hybrid.column_membership_decodes", 1);
        if (!cacheable) return visitor(std::as_const(decoded));
        prepareReadCache();
        if (!membership_cache_) return visitor(std::as_const(decoded));
        auto &entry = membership_cache_->entries[membership_cache_->cursor++ % membership_cache_->entries.size()];
        entry.group = group; entry.value = decoded; entry.valid = true; entry.io_epoch = epoch;
        return visitor(std::as_const(entry.value));
    }
    [[nodiscard]] HybridPropertyColumnMembership columnMembership(const HybridPropertyGroupRecord &group) const {
        return withColumnMembership(group, [](const auto &value) { return value; });
    }
    const MetadataSnapshotPin &snapshot_;
    RelocatableMetadataStore &metadata_;
    Generation generation_;
    mutable BTreePageStore trees_;
    const StorageIO *membership_io_ = nullptr; // owned by fields_, never escaped
    HybridPropertyFieldPageStore fields_;
    std::optional<HybridPropertyDescriptor> descriptor_;
    const CancellationCheckpoint *cancellation_ = nullptr;
    // Capture at context construction, never charge a later caller's temporary
    // integrity/maintenance account for metadata retained by this context.
    std::shared_ptr<std::pmr::memory_resource> membership_memory_owner_;
    mutable std::unique_ptr<MembershipCache> membership_cache_;
    mutable bool membership_cache_disabled_ = false;
    mutable size_t full_entity_columns_ = 8U;
};
} // namespace graph::storage
