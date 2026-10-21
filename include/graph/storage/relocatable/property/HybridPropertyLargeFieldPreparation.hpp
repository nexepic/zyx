/**
 * @file HybridPropertyLargeFieldPreparation.hpp
 * @brief Owned single-field immutable-value proposals for compound graph COW.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/storage/detail/PropertyDecodeWorkspace.hpp"
#include "graph/core/Node.hpp"
#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/property/HybridPropertyExtension.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/utils/StringBufferBytes.hpp"

namespace graph::storage {
// Attach store.contentIndex() before entering the metadata writer (its lazy
// allocator attachment opens a snapshot). With that attachment complete, this
// can also run under the writer using its fenced pin. This freezes one existing-format
// field payload and authenticates content-dedup candidates under one pin. It
// does not allocate identities, reserve slots or publish a standalone value.
// New IDs are proposals from the same fenced graph batch; the metadata writer
// must validate their consecutive sequence and publish data/manifest/content
// index/field references together. Reused references must join its revalidation
// list. Content deduplication is NOT logical no-op equality (notably for NaN).
class HybridPropertyLargeFieldPreparation final {
    struct State {
        utils::OwnedHeapReservation reference_credit;
        std::pmr::vector<std::byte> payload;
        LargeValueRef reference;
        bool reused = false;
        size_t candidates = 0, data_pages = 0, manifest_pages = 0;
        explicit State(const std::shared_ptr<std::pmr::memory_resource> &memory)
            : reference_credit(memory, 2U * utils::detail::standardFreshStringHeapUpper(RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES)),
              payload(memory.get()) {}
    };
    std::shared_ptr<const State> state_;
    DatabaseId database_id_{};
    Generation source_generation_{};
    EntityPropertyKey owner_{};
    uint32_t token_ = 0;
    HybridPropertyLargeFieldPreparation(std::shared_ptr<State> state, const Superblock &source,
        EntityPropertyKey owner, uint32_t token)
        : state_(std::move(state)), database_id_(source.database_id), source_generation_(source.generation),
          owner_(owner), token_(token) {}
    template<class Encoder>
    [[nodiscard]] static HybridPropertyLargeFieldPreparation prepareEncoded(RelocatableMetadataStore &metadata,
        const MetadataSnapshotPin &snapshot, LargeValueStore &store, EntityPropertyKey owner, uint32_t token,
        LargeValueId proposedId, std::shared_ptr<std::pmr::memory_resource> memory,
        const LargeValueCompressionOptions &compression, LargeValueExtentClass extentClass,
        size_t maximumCandidates, const CancellationCheckpoint *cancellation, Encoder &&encode) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native large-field preparation is disabled");
        const bool admitted = bool(std::dynamic_pointer_cast<query::execution::MaterializedByteBudget>(memory)) ||
            bool(std::dynamic_pointer_cast<query::execution::detail::PrepaidMemoryResource>(memory));
        hybrid_property_detail::require(metadata.ownsSnapshot(snapshot) && store.usesMetadataStore(metadata) &&
            owner.isValid() && admitted && maximumCandidates > 0 &&
            maximumCandidates <= RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES, "native large field requires its source pin, owner/token and admitted account");
        utils::OwnedAllocationScope allocation(memory);
        const auto check = [&] { if (cancellation) cancellation->check(); };
        const std::function<void()> checkpoint = std::ref(check);
        check();
        auto state = std::allocate_shared<State>(utils::OwnedPmrAllocator<State>(memory), memory);
        state->payload = encode(*memory);
        check();
        {
            // Existing dedup APIs also use small standard strings/identity
            // vectors. Bound their supported-STL return/move overlap; payload
            // and decoded-candidate buffers retain separate actual admission.
            constexpr size_t identityBytes = 1U + 2U * sizeof(uint16_t) + 2U * sizeof(uint64_t) +
                RSS_LARGE_VALUE_CONTENT_HASH_BYTES + RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES;
            const size_t strings = 8U * utils::detail::standardFreshStringHeapUpper(RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES);
            const size_t vectors = 4U * (utils::detail::standardContainerProxyBytes() +
                utils::detail::standardAllocatorRequestBytes(identityBytes, alignof(std::byte)));
            utils::OwnedHeapReservation metadataScratch(memory, strings + vectors);
            detail::PropertyDecodeWorkspace probe(memory, &checkpoint);
            auto existing = store.findContentDuplicateWithStats(snapshot, state->payload, LargeValueKind::LVK_BYTES,
                RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, maximumCandidates);
            state->candidates = existing.candidates_scanned;
            if (existing.reference) {
                state->reference = std::move(*existing.reference); state->reused = true;
            } else {
                hybrid_property_detail::require(proposedId.isValid() && snapshot.superblock().next_large_value_id &&
                    proposedId.value() >= snapshot.superblock().next_large_value_id,
                    "native large-field identity is not a valid same-batch counter proposal");
                state->reference = store.planReference(proposedId, Generation(1), state->payload,
                    LargeValueKind::LVK_BYTES, RSS_ENTITY_PROPERTY_LARGE_VALUE_MEDIA_TYPE, compression, extentClass);
                state->data_pages = store.plannedDataPageCount(state->payload, compression, extentClass);
                state->manifest_pages = store.plannedManifestPageCount(state->payload, compression, extentClass);
            }
            check();
        }
        state->reference_credit.set(utils::detail::dynamicStringBufferBytes(state->reference.media_type));
        check();
        return HybridPropertyLargeFieldPreparation(std::move(state), snapshot.superblock(), owner, token);
    }
public:
    [[nodiscard]] static HybridPropertyLargeFieldPreparation prepare(RelocatableMetadataStore &metadata,
        const MetadataSnapshotPin &snapshot, LargeValueStore &store, EntityPropertyKey owner, uint32_t token,
        const PropertyValue &value, LargeValueId proposedId, std::shared_ptr<std::pmr::memory_resource> memory,
        const LargeValueCompressionOptions &compression = {},
        LargeValueExtentClass extentClass = LargeValueExtentClass::LVEC_32K,
        size_t maximumCandidates = RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES,
        const CancellationCheckpoint *cancellation = nullptr) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native large-field preparation is disabled");
        hybrid_property_detail::require(token != 0, "native large field requires a nonzero token");
        return prepareEncoded(metadata, snapshot, store, owner, token, proposedId, std::move(memory), compression,
            extentClass, maximumCandidates, cancellation, [&](std::pmr::memory_resource &resource) {
                return EntityPropertyLargeValuePayloadCodec::encodeSingleField(token, value, resource);
            });
    }
    // Whole-ROW inverse carriers use the same immutable format and ownership,
    // while keeping their multiple-field payload distinct from a field record.
    [[nodiscard]] static HybridPropertyLargeFieldPreparation prepareRoot(RelocatableMetadataStore &metadata,
        const MetadataSnapshotPin &snapshot, LargeValueStore &store, EntityPropertyKey owner,
        const PropertyCollectionRoot &row, LargeValueId proposedId, std::shared_ptr<std::pmr::memory_resource> memory,
        const CancellationCheckpoint *cancellation = nullptr) {
        if (!RSS_HYBRID_PROPERTY_FORMAT_ENABLED)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "native ROW carrier preparation is disabled");
        hybrid_property_detail::require(owner.isValid() && owner.entity_id == row.owner_entity_id &&
            row.owner_entity_type == (owner.owner_kind == EntityPropertyOwnerKind::EPOK_NODE ? graph::Node::typeId : graph::Edge::typeId),
            "native ROW carrier has a mismatched logical owner");
        return prepareEncoded(metadata, snapshot, store, owner, 0, proposedId, std::move(memory), {},
            LargeValueExtentClass::LVEC_32K, RSS_LARGE_VALUE_DEFAULT_MAX_DEDUP_CANDIDATES, cancellation,
            [&](std::pmr::memory_resource &resource) {
                return EntityPropertyLargeValuePayloadCodec::encode(row.entries, resource,
                    [&] { if (cancellation) cancellation->check(); });
            });
    }
    [[nodiscard]] bool reused() const noexcept { return state_->reused; }
    [[nodiscard]] size_t candidatesScanned() const noexcept { return state_->candidates; }
    [[nodiscard]] size_t newDataPages() const noexcept { return state_->data_pages; }
    [[nodiscard]] size_t newManifestPages() const noexcept { return state_->manifest_pages; }
    [[nodiscard]] DatabaseId databaseId() const noexcept { return database_id_; }
    [[nodiscard]] Generation sourceGeneration() const noexcept { return source_generation_; }
    [[nodiscard]] EntityPropertyKey owner() const noexcept { return owner_; }
    [[nodiscard]] uint32_t token() const noexcept { return token_; }
    [[nodiscard]] const LargeValueRef &reference() const & { return state_->reference; }
    const LargeValueRef &reference() const && = delete;
    [[nodiscard]] std::span<const std::byte> payload() const & { return state_->payload; }
    std::span<const std::byte> payload() const && = delete;
};
} // namespace graph::storage
