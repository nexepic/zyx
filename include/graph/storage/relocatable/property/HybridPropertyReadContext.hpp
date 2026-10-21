/**
 * @file HybridPropertyReadContext.hpp
 * @brief Pinned descriptor, directory and field access for the hybrid candidate.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include <algorithm>
#include <limits>
#include <vector>
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/property/HybridPropertyDirectory.hpp"
#include "graph/storage/relocatable/property/HybridPropertyFieldPageStore.hpp"

namespace graph::storage {
class HybridPropertyReadContextTestAccess;
class HybridPropertyDirectoryIntegrity;
// Snapshot-local, with borrowed caller-owned page store and pin. Normal opening
// reads the actual physical descriptor root. Its nonempty publication/read guard
// remains until all graph writers, integrity and retirement consumers are ready.
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
    [[nodiscard]] bool hasNativeGroups() const {
        check(); return hasRoot(EntityPropertyOwnerKind::EPOK_NODE, 3) || hasRoot(EntityPropertyOwnerKind::EPOK_EDGE, 3);
    }
    // Fixed-size authenticated source for bounded inverse conversion. This
    // grants no write authority; the caller must retain/fence the same pin.
    [[nodiscard]] std::optional<HybridPropertyGroupRecord> groupRecord(EntityPropertyKey owner,
        const std::function<void(size_t)> &boundaryAdmission = {}) const {
        return findGroup(owner, boundaryAdmission);
    }
    // First actual membership owner beyond a stable logical cursor. Fieldless
    // live owners have no LargeValue references and need no maintenance visit.
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
        if (!hasRoot(owner.owner_kind, 0)) {
            require(group->memberships == 0, "hybrid group memberships have no owner directory");
            return result;
        }
        require(bool(visitor), "hybrid owner enumeration requires a visitor");
        const auto map = ownerMap(owner.owner_kind);
        const auto checked = [&](uint32_t token, HybridPropertySource source) {
            check();
            require(live && result.fields < group->memberships,
                "hybrid owner membership contradicts group liveness or count");
            require(hasRoot(owner.owner_kind, source == HybridPropertySource::HPS_COLUMN ? 1U : 2U),
                "hybrid owner membership has no source directory");
            ++result.fields;
            return visitor(token, source);
        };
        result.complete = HybridPropertyDirectoryCodec::visitOwner(map, snapshot_, owner, maximumEntries,
            std::ref(checked), boundaryAdmission, cancellation_);
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
        if (!hasRoot(owner.owner_kind, 0)) {
            require(group->memberships == 0, "hybrid group memberships have no owner directory");
            return FieldRoute::HFR_MISSING;
        }
        const auto key = HybridPropertyDirectoryCodec::ownerKey(owner, token);
        const auto membership = ownerMap(owner.owner_kind).find(snapshot_, key, boundaryAdmission, cancellation_);
        if (!membership) return FieldRoute::HFR_MISSING;
        require(isLive(owner, *group) && group->memberships,
            "hybrid field membership belongs to a dead or empty owner group");
        const auto source = HybridPropertyDirectoryCodec::decodeSource(*membership);
        const size_t role = source == HybridPropertySource::HPS_COLUMN ? 1U : 2U;
        require(hasRoot(owner.owner_kind, role), "hybrid member source directory is absent");
        const auto binding = bind(owner.owner_kind, role);
        HybridPropertyFieldDirectoryMap fields(binding.root.index_id, trees_, metadata_, binding);
        const auto locator = fields.find(snapshot_, role == 1 ?
            HybridPropertyDirectoryCodec::columnKey(group->group_id, token) : key,
            boundaryAdmission, cancellation_);
        require(bool(locator), "hybrid field membership has no source record");
        const auto reference = HybridPropertyDirectoryCodec::decodeLocator(*locator);
        const auto page = fields_.read(snapshot_, reference.page_id);
        check();
        if (role == 1) {
            const auto column = page.view().readColumn(reference, *group, token, descriptor_->semantic_generation);
            const auto slot = hybridPropertySlotFor(owner); const auto bit = UINT64_C(1) << slot;
            require(bool(column.presence & bit), "hybrid column membership has no present value");
            if (columnVisitor) columnVisitor(*group, column, groupReference, reference);
            scalar({column.identity.scalar_kind, true, bool(column.nulls & bit), column.words[slot]});
        } else {
            const auto value = page.view().readExtension(reference, owner, token, *group,
                descriptor_->semantic_generation, referenceAdmission);
            extension(value);
        }
        check();
        return FieldRoute::HFR_FOUND;
    }
private:
    friend class HybridPropertyReadContextTestAccess;
    friend class HybridPropertyDirectoryIntegrity;
    friend class HybridPropertyDemotionPreparation;
    friend class HybridPropertyColumnDelta;
    HybridPropertyReadContext(std::shared_ptr<StorageIO> io, SlottedPagePool &pages,
        RelocatableMetadataStore &metadata, const MetadataSnapshotPin &snapshot,
        std::optional<HybridPropertyDescriptor> descriptor, const CancellationCheckpoint *cancellation)
        : snapshot_(snapshot), metadata_(metadata), generation_(snapshot.superblock().generation),
          trees_(pages, metadata), fields_(std::move(io), metadata, snapshot),
          descriptor_(std::move(descriptor)), cancellation_(cancellation) {
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
        const auto binding = bind(owner.owner_kind, 3);
        HybridPropertyGroupDirectoryMap groups(binding.root.index_id, trees_, metadata_, binding);
        const auto groupId = hybridPropertyGroupFor(owner);
        const auto locator = groups.find(snapshot_, HybridPropertyDirectoryCodec::groupKey(groupId), boundaryAdmission, cancellation_);
        if (!locator) return {};
        const auto reference = HybridPropertyDirectoryCodec::decodeLocator(*locator);
        const auto page = fields_.read(snapshot_, reference.page_id);
        const auto group = page.view().readGroup(reference, owner.owner_kind, groupId, descriptor_->semantic_generation);
        const auto entity = owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ?
            metadata_.findNodePageLocationOptional(snapshot_, owner.entity_id) :
            metadata_.findEdgePageLocationOptional(snapshot_, owner.entity_id);
        require(bool(entity) == isLive(owner, group), "hybrid group liveness disagrees with the Entity Map");
        require(!metadata_.findEntityPropertyLocationOptional(snapshot_, owner), "hybrid group owner still has a competing ROW root");
        if (source) *source = reference;
        return group;
    }
    const MetadataSnapshotPin &snapshot_;
    RelocatableMetadataStore &metadata_;
    Generation generation_;
    mutable BTreePageStore trees_;
    HybridPropertyFieldPageStore fields_;
    std::optional<HybridPropertyDescriptor> descriptor_;
    const CancellationCheckpoint *cancellation_ = nullptr;
};
} // namespace graph::storage
