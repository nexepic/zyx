/**
 * @file HybridPropertyRowPreparation.hpp
 * @brief Accounted native hybrid-to-ROW source preparation for one group.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/property/HybridPropertyGroupConversion.hpp"
#include "graph/storage/relocatable/property/HybridPropertyValueReader.hpp"

namespace graph::storage {
// Inverse conversion preparation only: no IDs, pages, roots or retirement are
// published here. The enclosing writer must fence this source generation and
// install ROW roots, erase native directories and retire storage atomically.
class HybridPropertyRowPreparation final {
    // Keep decoded standard-container credit until after every returned value
    // is destroyed. The thread-local decode scope exists only in construction.
    utils::OwnedHeapReservation decoded_credit_;
    utils::OwnedVector<PropertyCollectionRoot> rows_;
    DatabaseId database_id_{};
    Generation source_generation_{};
    HybridPropertyGroupRecord source_group_;
public:
    HybridPropertyRowPreparation(const HybridPropertyReadContext &context,
        const MetadataSnapshotPin &snapshot, RelocatableMetadataStore &metadata, EntityPageStore &entities,
        EntityPropertyOwnerKind kind, uint64_t groupId, std::shared_ptr<std::pmr::memory_resource> memory,
        LargeValueStore *largeValues = nullptr, const CancellationCheckpoint *cancellation = nullptr)
        : decoded_credit_(memory), rows_(utils::OwnedPmrAllocator<PropertyCollectionRoot>(memory)) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native inverse property conversion is disabled");
        const auto account = std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(memory);
        constexpr size_t cap = 1024U * 1024U;
        hybrid_property_detail::require(account && account->used() <= cap && account->available() <= cap - account->used() &&
            context.usesSnapshot(snapshot) && metadata.ownsSnapshot(snapshot) && entities.usesMetadataStore(metadata) &&
            (!largeValues || largeValues->usesMetadataStore(metadata)) && hybrid_property_detail::validKind(kind) &&
            groupId && groupId <= RSS_HYBRID_MAX_GROUP_ID,
            "native inverse conversion requires one source pin, group and bounded conversion account");
        utils::OwnedAllocationScope allocation(memory);
        const auto check = [&] { context.checkpoint(); if (cancellation) cancellation->check(); };
        utils::OwnedHeapReservation boundaryCredit(memory);
        const auto boundary = [&](size_t bytes) { boundaryCredit.set(bytes); };
        check();
        const auto first = (groupId - 1U) * RSS_HYBRID_GROUP_CAPACITY + 1U;
        const auto group = context.groupRecord({kind, first}, std::ref(boundary));
        hybrid_property_detail::require(bool(group), "native inverse conversion requires an installed source group");
        boundaryCredit.set(0);
        if (group->memberships > HybridPropertyGroupConversion::MAX_MEMBERSHIPS)
            throw std::length_error("native inverse conversion exceeds its membership bound");
        source_group_ = *group;
        database_id_ = snapshot.superblock().database_id; source_generation_ = snapshot.superblock().generation;
        struct Owner { EntityPropertyKey key; size_t fields; };
        utils::OwnedVector<Owner> owners{utils::OwnedPmrAllocator<Owner>(memory)};
        owners.reserve(std::popcount(group->live));
        size_t memberships = 0; uint64_t live = 0;
        for (unsigned slot = 0; slot < RSS_HYBRID_GROUP_CAPACITY && first + slot <= RSS_HYBRID_MAX_ENTITY_ID; ++slot) {
            check(); const EntityPropertyKey owner{kind, first + slot};
            const auto summary = context.visitOwner(owner, HybridPropertyGroupConversion::MAX_MEMBERSHIPS + 1U,
                [](uint32_t, HybridPropertySource) { return true; }, std::ref(boundary));
            boundaryCredit.set(0);
            hybrid_property_detail::require(summary.routed && summary.complete &&
                (summary.owner_live || !summary.fields), "native inverse conversion has an incomplete or dead source owner");
            if (summary.fields > HybridPropertyGroupConversion::MAX_MEMBERSHIPS - memberships)
                throw std::length_error("native inverse conversion exceeds its membership bound");
            memberships += summary.fields;
            if (!summary.owner_live) continue;
            live |= UINT64_C(1) << slot;
            if (kind == EntityPropertyOwnerKind::EPOK_NODE) static_cast<void>(entities.readNode(snapshot, owner.entity_id));
            else static_cast<void>(entities.readEdge(snapshot, owner.entity_id));
            owners.push_back({owner, summary.fields});
        }
        hybrid_property_detail::require(live == group->live && memberships == group->memberships,
            "native inverse conversion group counts disagree with actual live owner memberships");
        rows_.reserve(owners.size());
        // Complete bounded metadata preflight before allocating/decoding values.
        struct DecodeOwner { HybridPropertyRowPreparation &result; const decltype(check) &checkpoint; } decodeOwner{*this, check};
        property_decode::AllocationScope decoding(&decodeOwner,
            [](void *owner, size_t bytes) {
                auto &credit = static_cast<DecodeOwner *>(owner)->result.decoded_credit_;
                credit.set(utils::detail::addStandardHeapBytes(credit.bytes(), bytes));
            }, [](void *owner) { static_cast<DecodeOwner *>(owner)->checkpoint(); });
        const uint32_t type = kind == EntityPropertyOwnerKind::EPOK_NODE ? Node::typeId : Edge::typeId;
        for (const auto &owner : owners) {
            check();
            // Supported debug STLs may allocate proxy storage on empty/moved
            // vector construction. Admit the bounded move frontier up front.
            decoded_credit_.set(utils::detail::addStandardHeapBytes(decoded_credit_.bytes(),
                3U * utils::detail::standardContainerProxyBytes()));
            auto collection = HybridPropertyValueReader::readCollection(context, snapshot, owner.key, type, largeValues);
            hybrid_property_detail::require(collection.routed, "native inverse conversion lost its pinned source route");
            if (collection.value) {
                const auto &row = *collection.value;
                hybrid_property_detail::require(row.owner_entity_id == owner.key.entity_id && row.owner_entity_type == type &&
                    row.entries.size() == owner.fields, "native inverse conversion changed its source owner or field count");
                rows_.push_back(std::move(*collection.value));
            } else {
                hybrid_property_detail::require(!owner.fields, "native inverse conversion lost a nonempty source owner");
                rows_.push_back({owner.key.entity_id, type, {}});
            }
        }
        check();
    }
    HybridPropertyRowPreparation(const HybridPropertyRowPreparation &) = delete;
    HybridPropertyRowPreparation &operator=(const HybridPropertyRowPreparation &) = delete;
    HybridPropertyRowPreparation(HybridPropertyRowPreparation &&) noexcept = default;
    HybridPropertyRowPreparation &operator=(HybridPropertyRowPreparation &&) = delete;
    [[nodiscard]] std::span<const PropertyCollectionRoot> rows() const & noexcept { return rows_; }
    std::span<const PropertyCollectionRoot> rows() const && = delete;
    [[nodiscard]] DatabaseId databaseId() const noexcept { return database_id_; }
    [[nodiscard]] Generation sourceGeneration() const noexcept { return source_generation_; }
    [[nodiscard]] HybridPropertyGroupRecord sourceGroup() const noexcept { return source_group_; }
};
} // namespace graph::storage
