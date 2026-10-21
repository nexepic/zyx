/**
 * @file HybridPropertyImmutablePreparation.hpp
 * @brief Metadata-owned private LargeValue frames for native field conversion.
 * Copyright (c) 2026 Nexepic. Licensed under Apache-2.0.
 */
#pragma once
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/property/HybridPropertyLargeFieldPreparation.hpp"

namespace graph::storage {
// The enclosing metadata writer owns allocation and publication. This object
// prepares complete immutable frames and routes without writing any file bytes;
// its pool reservations belong to the same batch as the field/index images.
class HybridPropertyImmutablePreparation final {
    friend class RelocatableMetadataStore;
    struct Frame {
        CatalogEntry pool;
        PageLocation location;
        PageOwnerHeader owner;
        utils::OwnedVector<std::byte> payload;
    };
    struct Placement { CatalogEntry pool; PageLocation location; PageOwnerKind kind; };
    utils::OwnedVector<Frame> frames_;
    utils::OwnedVector<LargeValueManifestLocation> manifests_;
    size_t data_frames_ = 0;

    template<class AllocatePool, class Checkpoint>
    HybridPropertyImmutablePreparation(MetadataWriteBatch &batch, LargeValueStore &values,
        std::span<const HybridPropertyLargeFieldPreparation> proposals,
        const std::shared_ptr<std::pmr::memory_resource> &memory,
        AllocatePool &&allocatePool, Checkpoint &&checkpoint)
        : frames_(utils::OwnedPmrAllocator<Frame>(memory)),
          manifests_(utils::OwnedPmrAllocator<LargeValueManifestLocation>(memory)) {
        hybrid_property_detail::require(bool(memory), "native immutable images require their conversion account");
        utils::OwnedAllocationScope scope(memory);
        CatalogEntry dataPool, manifestPool;
        size_t dataUsed = RSS_PAGE_POOL_DATA_FRAME_COUNT, manifestUsed = RSS_PAGE_POOL_DATA_FRAME_COUNT;
        const auto allocateRange = [&](size_t count) {
            PageId first;
            for (size_t i = 0; i < count; ++i) {
                checkpoint();
                const auto page = batch.allocateLogicalPageId();
                if (!i) first = page;
                else hybrid_property_detail::require(page.value() - first.value() == i,
                    "native immutable page range lost its batch identity sequence");
            }
            return first;
        };
        for (const auto &proposal : proposals) {
            checkpoint();
            hybrid_property_detail::require(proposal.databaseId() == batch.current().database_id &&
                proposal.sourceGeneration() == batch.current().generation,
                "native immutable proposal has a foreign or stale source fence");
            if (proposal.reused()) continue;
            // This coordinator currently proposes RAW/32K new values. Bound the
            // existing standard-vector encoder's chunk/frame/manifest frontier
            // before invoking it; retained images use actual owned allocations.
            hybrid_property_detail::require(proposal.reference().codec == LargeValueCodec::LVC_RAW,
                "native immutable preparation requires its registered RAW encoding");
            constexpr size_t scratchBytes = 6U * RSS_LARGE_VALUE_EXTENT_32K_BYTES +
                16U * RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES +
                4U * RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS * sizeof(LargeValueChunk) +
                16U * RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES + 4096U;
            utils::OwnedHeapReservation scratch(memory, scratchBytes);
            LargeValueStreamedMaterializationRequest request;
            request.reference = proposal.reference(); request.logical_bytes = proposal.payload();
            request.data_page_count = proposal.newDataPages(); request.manifest_page_count = proposal.newManifestPages();
            request.first_data_page_id = allocateRange(request.data_page_count);
            request.first_manifest_page_id = allocateRange(request.manifest_page_count);
            hybrid_property_detail::require(request.first_manifest_page_id.isValid(), "native value has no manifest range");
            utils::OwnedVector<Placement> placements{utils::OwnedPmrAllocator<Placement>(memory)};
            placements.reserve(request.data_page_count + request.manifest_page_count);
            const auto place = [&](PageId first, size_t count, bool data) {
                auto &pool = data ? dataPool : manifestPool;
                auto &used = data ? dataUsed : manifestUsed;
                for (size_t i = 0; i < count; ++i) {
                    checkpoint();
                    if (used == RSS_PAGE_POOL_DATA_FRAME_COUNT) {
                        pool = allocatePool(data ? SegmentKind::SGK_LARGE_VALUE_DATA_POOL :
                            SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL, data ? RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID :
                            RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID);
                        used = 0;
                    }
                    placements.push_back({pool, {PageId(first.value() + i), pool.segment_id,
                        uint16_t(RSS_PAGE_POOL_FIRST_DATA_FRAME + used++), pool.generation, 0},
                        data ? PageOwnerKind::POK_LARGE_VALUE_DATA : PageOwnerKind::POK_LARGE_VALUE_MANIFEST});
                }
            };
            place(request.first_data_page_id, request.data_page_count, true);
            place(request.first_manifest_page_id, request.manifest_page_count, false);
            const auto placementFor = [&](PageId id, PageOwnerKind kind) -> const Placement & {
                const auto found = std::lower_bound(placements.begin(), placements.end(), id,
                    [](const Placement &placement, PageId page) { return placement.location.page_id < page; });
                hybrid_property_detail::require(found != placements.end() && found->location.page_id == id && found->kind == kind,
                    "native immutable frame lost its assigned placement");
                return *found;
            };
            const auto resolve = [&](PageId id, PageOwnerKind kind) {
                checkpoint(); const auto &placement = placementFor(id, kind);
                return PageOwnerHeader{id, proposal.reference().value_id.value(), kind, 0,
                    RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, placement.pool.generation, batch.checkpointLsn()};
            };
            const size_t before = frames_.size();
            const auto retain = [&](const PageOwnerHeader &owner, std::vector<std::byte> bytes, PageOwnerKind kind) {
                checkpoint(); const auto &placement = placementFor(owner.page_id, kind);
                hybrid_property_detail::require(PageOwnerHeaderCodec::encode(owner) ==
                    PageOwnerHeaderCodec::encode(resolve(owner.page_id, kind)) && bytes.size() == RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
                    "native immutable frame changed its assigned owner or width");
                utils::OwnedVector<std::byte> payload(bytes.begin(), bytes.end(), utils::OwnedPmrAllocator<std::byte>(memory));
                frames_.push_back({placement.pool, placement.location, owner, std::move(payload)});
                data_frames_ += kind == PageOwnerKind::POK_LARGE_VALUE_DATA;
            };
            values.materializeStreamedValue(request, batch.checkpointLsn(), std::ref(resolve), std::ref(retain));
            hybrid_property_detail::require(frames_.size() - before == placements.size(),
                "native immutable encoder did not emit every assigned frame");
            manifests_.push_back({proposal.reference().value_id, request.first_manifest_page_id,
                placementFor(request.first_manifest_page_id, PageOwnerKind::POK_LARGE_VALUE_MANIFEST).pool.generation});
        }
        checkpoint();
    }
public:
    [[nodiscard]] size_t dataFrameCount() const noexcept { return data_frames_; }
    [[nodiscard]] size_t manifestFrameCount() const noexcept { return frames_.size() - data_frames_; }
};
} // namespace graph::storage
