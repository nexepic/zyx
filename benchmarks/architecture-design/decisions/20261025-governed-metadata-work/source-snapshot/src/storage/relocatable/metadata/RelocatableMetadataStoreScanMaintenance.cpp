/**
 * @file RelocatableMetadataStoreScanMaintenance.cpp
 * @brief Snapshot-safe single-pool reclamation for a private BUILDING image.
 * Copyright (c) 2026 Nexepic. Licensed under the Apache License, Version 2.0.
 */
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"
#include "graph/storage/relocatable/format/BuildingScanMaintenance.hpp"
#include "graph/storage/relocatable/format/BuildingScanCompletion.hpp"
#include "graph/storage/relocatable/maintenance/NativeStorageAllocationCensus.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"
#include "graph/storage/relocatable/metadata/MetadataDirectoryEvacuation.hpp"
#include "graph/utils/OwnedHeapReservation.hpp"
#include "graph/debug/PerfTrace.hpp"
#include <algorithm>
#include <bit>

namespace graph::storage {
Superblock RelocatableMetadataStore::completeBuildingScanImage(const BuildingScanCompletionRequest &request,
    LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
#if !ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
    static_cast<void>(request); static_cast<void>(checkpointLsn); static_cast<void>(options);
    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "scan completion requires its isolated format");
#else
    constexpr uint64_t diskLimit = UINT64_C(2) * 1024U * 1024U * 1024U;
    constexpr size_t memoryLimit = 7U * 1024U * 1024U;
    if (!request.memory || request.memory->used() > memoryLimit ||
        request.memory->available() > memoryLimit - request.memory->used() ||
        !request.source_integrity_generation.isValid() || !request.require_stable_files ||
        request.io_epoch == UINT64_MAX || !request.source_bytes ||
        request.maximum_total_bytes > diskLimit || request.source_bytes > request.maximum_total_bytes)
        throw std::invalid_argument("scan completion requires a bounded producer certificate");
    const auto check = [&] {
        if (request.cancellation) request.cancellation->check();
        request.require_stable_files();
    };
    utils::OwnedAllocationScope owner(request.memory);
    utils::OwnedHeapReservation frontier(request.memory, 32U * 1024U);
    check();
    const auto requireFence = [&](const Superblock &root) {
        if (!(root.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING) || root.database_id != request.database ||
            root.generation != request.generation || root.hybrid_property_descriptor_root != request.descriptor ||
            io_->dataMutationEpoch() != request.io_epoch || !checkpointLsn.isValid() || checkpointLsn <= root.checkpoint_lsn)
            throw FormatException(FormatError::FERR_INVALID_STATE, "scan completion lost its validated image fence");
    };
    requireFence(bootstrap_.recover());
    auto writer = beginWrite(true);
    const auto current = bootstrap_.recover(); requireFence(current);
    MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
    batch.hybrid_property_publication_ = true;
    batch.candidate_.flags &= ~RSS_SUPERBLOCK_FLAG_SCAN_BUILDING;
    // Materialize every COW frame before measuring actual file size. Publication
    // subsequently uses this prepared batch without allocating more disk space.
    prepareBatchPublication(batch);
    debug::PerfTrace::addValue("native_graph.scan_build.prepared_completion", 1);
    NativeStorageAllocationCensusOptions censusOptions;
    censusOptions.maximum_catalog_entries = 16384;
    censusOptions.maximum_retirement_records = 16384;
    censusOptions.maximum_allocated_slots = 16384;
    censusOptions.maximum_allocator_metadata_pages = 1024;
    censusOptions.cancellation = request.cancellation;
    const auto footprint = NativeStorageAllocationCensus(io_, *this, request.memory)
        .inspectPreparedCandidate(writer, batch, censusOptions);
    const uint64_t targetBytes = io_->fileSize();
    if (!footprint || !footprint->fullyAttributed() || footprint->file_bytes != targetBytes ||
        targetBytes > request.maximum_total_bytes - request.source_bytes ||
        targetBytes > diskLimit || targetBytes * 10U > request.source_bytes * 9U)
        throw FormatException(FormatError::FERR_INVALID_STATE, "scan image failed actual whole-file space admission");
    check();
    publishBatch(batch, writer, options);
    return batch.candidate();
#endif
}

MetadataPagePoolReclamation RelocatableMetadataStore::reclaimOneBuildingScanMetadataPool(
    const BuildingScanMaintenanceRequest &request, LogSequenceNumber checkpointLsn,
    const MetadataPublishOptions &options) {
#if !ZYX_EXPERIMENTAL_SCAN_PATH_FORMAT
    static_cast<void>(request); static_cast<void>(checkpointLsn); static_cast<void>(options);
    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION, "private scan maintenance requires its isolated format");
#else
    constexpr size_t limit = 7U * 1024U * 1024U;
    if (!request.memory || request.memory->used() > limit ||
        request.memory->available() > limit - request.memory->used() || !checkpointLsn.isValid() ||
        !request.expected_database_id.isValid() || !request.expected_generation.isValid())
        throw std::invalid_argument("private scan maintenance requires exact identity and a bounded account");
    const auto check = [&] { if (request.cancellation) request.cancellation->check(); };
    check();
    utils::OwnedAllocationScope owner(request.memory);
    utils::OwnedHeapReservation frontier(request.memory, 32U * 1024U);
    const auto requireFence = [&](const Superblock &root) {
        if (!(root.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING) || root.database_id != request.expected_database_id ||
            root.generation != request.expected_generation || root.hybrid_property_descriptor_root != request.expected_descriptor ||
            checkpointLsn <= root.checkpoint_lsn)
            throw FormatException(FormatError::FERR_INVALID_STATE, "private scan maintenance lost its BUILDING root fence");
    };
    requireFence(bootstrap_.recover());

		MetadataWriteGuard writer = beginWrite(true);
		const Superblock current = bootstrap_.recover();
        requireFence(current);

        // Reachability work tables now charge their actual allocations to the
        // active maintenance owner. Keep a small separate allowance for the
        // snapshot registry's protected-root copy; do not reject a sparse image
        // merely because it still has many allocated, unreachable frames.
        const size_t pins = snapshots_.activeSnapshotCount();
        constexpr size_t fixedBytes = 32U * 1024U;
        if (pins > (limit - fixedBytes) / 4096U)
            throw query::execution::MaterializedByteLimitExceeded();
        const size_t pinCredit = pins * 4096U;
        frontier.set(fixedBytes + pinCredit);
		// This reconciles page-frame bitmaps against current and pinned roots while
		// the writer barrier prevents a new snapshot from observing an unprotected
		// frame. A selected pool therefore has no metadata frame reachable from any
		// generation that can still read this process.
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, current);

		utils::OwnedVector<CatalogEntry> candidates;
        size_t lowerFreeFrames = 0;
        std::optional<CatalogEntry> evacuation;
        std::optional<MetadataPagePoolHeader> evacuationHeader;
		catalog_.visit(current, [this, &candidates, &current, &check, &lowerFreeFrames,
            &evacuation, &evacuationHeader, &request](const CatalogEntry &entry) {
            check();
			if (entry.kind != SegmentKind::SGK_METADATA_PAGE_POOL || entry.state == SegmentState::SGS_OBSOLETE) {
				return;
			}
			const uint8_t invalid = static_cast<uint8_t>(entry.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(entry.owner_id != entry.physical_slot.value()) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, entry.physical_slot)) |
									static_cast<uint8_t>(!metadata_page_pool_.isMetadataPoolSlot(entry.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata-pool reclaimer found invalid durable ownership");
			}
			const MetadataPagePoolHeader header = metadata_page_pool_.poolHeader(entry.physical_slot);
			if (header.segment_id != entry.segment_id || header.generation != entry.generation ||
				header.committed_lsn != entry.committed_lsn) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata-pool reclaimer found a Catalog/header identity mismatch");
			}
            const auto frames = static_cast<size_t>(std::popcount(header.allocated_frames));
            if (entry.physical_slot < request.evacuate_pool)
                lowerFreeFrames += RSS_PAGE_POOL_DATA_FRAME_COUNT - frames;
            if (entry.physical_slot == request.evacuate_pool) { evacuation = entry; evacuationHeader = header; }
			if (candidates.empty() && metadata_page_pool_.isMetadataPoolEmpty(entry.physical_slot)) {
				candidates.push_back(entry);
			}
		});
        // Complete-route collectors and evacuation work tables retain the same
        // governed owner. Their actual simultaneous allocations, rather than a
        // per-frame estimate, enforce the unchanged maintenance byte limit.
        if (request.evacuate_pool.isValid()) {
            if (!evacuation || !evacuationHeader)
                throw FormatException(FormatError::FERR_INVALID_STATE, "private metadata evacuation lost its exact pool");
            uint32_t frames = evacuationHeader->allocated_frames;
            const bool descriptorHere = current.hybrid_property_descriptor_root.isValid() &&
                current.hybrid_property_descriptor_root.physical_slot == request.evacuate_pool;
            const size_t liveFrames = std::popcount(frames);
            // Reclaimed data/metadata pools are also usable lower capacity.
            // Discover only enough allocator-proven holes for this bounded step;
            // pools still registered for an old pin cannot be counted as free.
            uint64_t search = 1U;
            while (lowerFreeFrames < liveFrames + RSS_PAGE_POOL_DATA_FRAME_COUNT &&
                search < request.evacuate_pool.value()) {
                check();
                const auto free = allocator_.findFirstFree(current, PhysicalSlotId(search),
                    PhysicalSlotId(request.evacuate_pool.value() - 1U));
                if (!free) break;
                if (!metadata_page_pool_.isMetadataPoolSlot(*free)) lowerFreeFrames += RSS_PAGE_POOL_DATA_FRAME_COUNT;
                search = free->value() + 1U;
            }
            debug::PerfTrace::addValue("native_graph.scan_build.metadata_source_pool", request.evacuate_pool.value());
            debug::PerfTrace::addValue("native_graph.scan_build.metadata_source_frames", liveFrames);
            debug::PerfTrace::addValue("native_graph.scan_build.metadata_lower_free_frames", lowerFreeFrames);
            // Leave a complete pool's worth of lower COW capacity for directory
            // publication. No whole-directory clone or speculative tail growth
            // is used when this bounded evacuation cannot make forward progress.
            if (!liveFrames || lowerFreeFrames < liveFrames + RSS_PAGE_POOL_DATA_FRAME_COUNT) {
                debug::PerfTrace::addValue("native_graph.scan_build.metadata_capacity_refusal", 1);
                return MetadataPagePoolReclamation{current, 0U, {}};
            }
            if (descriptorHere) {
                const uint32_t descriptorBit = 1U <<
                    (current.hybrid_property_descriptor_root.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
                if (!(frames & descriptorBit))
                    throw FormatException(FormatError::FERR_INVALID_STATE, "private descriptor frame is not allocated");
                frames &= ~descriptorBit;
            }
            enum class EvacuationKind : uint8_t {
                MEK_DESCRIPTOR, MEK_ROUTED, MEK_MIXED, MEK_ROUTE_BRANCH, MEK_BOOTSTRAP, MEK_PLUMBING
            };
            std::optional<EvacuationKind> kind;
            if (!frames && descriptorHere) kind = EvacuationKind::MEK_DESCRIPTOR;
            else if (canRelocateRoutedMetadataPoolPages(pages_, current, request.evacuate_pool, frames))
                kind = EvacuationKind::MEK_ROUTED;
            else if (canEvacuateMixedRoutedMetadataPool(pages_, current, request.evacuate_pool, frames))
                kind = EvacuationKind::MEK_MIXED;
            else if (canRefreshMetadataRouteBranchForEvacuation(pages_, current, request.evacuate_pool, frames))
                kind = EvacuationKind::MEK_ROUTE_BRANCH;
            else if (canRefreshBootstrapHeaderForEvacuation(pages_, current, request.evacuate_pool, frames))
                kind = EvacuationKind::MEK_BOOTSTRAP;
            if (!kind && current.usesPageDirectoryBootstrap()) {
                const auto bootstrap = pages_.readPageDirectoryBootstrap(current.page_directory_root);
                uint32_t plumbing = 0;
                for (const auto &reference : MetadataPageRouteDirectory(pages_).collectRouteBranchFrames(
                    current, bootstrap.metadata_page_directory_root)) {
                    if (reference.physical_slot == request.evacuate_pool)
                        plumbing |= 1U << (reference.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
                }
                if (current.page_directory_root.physical_slot == request.evacuate_pool) {
                    const uint32_t headerBit = 1U <<
                        (current.page_directory_root.frame_index - RSS_PAGE_POOL_FIRST_DATA_FRAME);
                    if (plumbing & headerBit)
                        throw FormatException(FormatError::FERR_INVALID_STATE, "private bootstrap aliases route plumbing");
                    plumbing |= headerBit;
                }
                // Existing generic helpers deliberately separate a pure route
                // branch from a sole bootstrap header. Their co-resident union
                // is also complete and can use the same bounded refresh path.
                if (plumbing && plumbing == frames) kind = EvacuationKind::MEK_PLUMBING;
            }
            if (!kind) {
                debug::PerfTrace::addValue("native_graph.scan_build.metadata_shape_refusal", 1);
                return MetadataPagePoolReclamation{current, 0U, {}};
            }
            check();
            MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
            batch.hybrid_property_publication_ = true;
            constexpr uint64_t maximumFileBytes = UINT64_C(2) * 1024U * 1024U * 1024U;
            if (current.data_region_offset >= maximumFileBytes) throw StorageFileLimitExceeded();
            const uint64_t lastSlot = (maximumFileBytes - current.data_region_offset) / RSS_SEGMENT_SIZE + 1U;
            const uint64_t initialSlots = (io_->fileSize() - current.data_region_offset) / RSS_SEGMENT_SIZE;
            if (request.evacuate_pool.value() > initialSlots || initialSlots >= lastSlot)
                throw StorageFileLimitExceeded();
            // Existing range reservations also govern metadata pool allocation.
            // Excluding the complete upper suffix admits COW only below source;
            // exhaustion reaches the target's hard file limit before any append.
            // Existing slots use individual reservations: a ranged reservation
            // is specifically a provisional suffix and its rollback requires
            // first > the initial file tail. Include the first out-of-limit slot
            // in that future suffix so attempting any append refuses atomically.
            for (uint64_t slot = request.evacuate_pool.value(); slot <= initialSlots; ++slot)
                batch.reserveDataSlot(PhysicalSlotId(slot));
            batch.reserveDataSlotRange({PhysicalSlotId(initialSlots + 1U), PhysicalSlotId(lastSlot)});
            if (descriptorHere) pages_.prepareHybridPropertyDescriptorRelocation(batch, current);
            std::optional<uint32_t> moved;
            switch (*kind) {
                case EvacuationKind::MEK_DESCRIPTOR: moved = 1U; break;
                case EvacuationKind::MEK_ROUTED:
                    moved = relocateRoutedMetadataPoolPages(pages_, batch, current, request.evacuate_pool, frames); break;
                case EvacuationKind::MEK_MIXED:
                    moved = evacuateMixedRoutedMetadataPool(pages_, batch, current, request.evacuate_pool, frames); break;
                case EvacuationKind::MEK_ROUTE_BRANCH:
                    moved = refreshMetadataRouteBranchForEvacuation(pages_, batch, current, request.evacuate_pool, frames); break;
                case EvacuationKind::MEK_BOOTSTRAP:
                    moved = refreshBootstrapHeaderForEvacuation(pages_, batch, current, request.evacuate_pool, frames); break;
                case EvacuationKind::MEK_PLUMBING:
                    batch.requestBootstrapRouteRefresh(); moved = std::popcount(frames); break;
            }
            if (!moved)
                throw FormatException(FormatError::FERR_INVALID_STATE, "private metadata evacuation lost its preflight");
            check(); publishBatch(batch, writer, options);
            // Old pins may still own source frames. A later ordinary single-pool
            // reclaimer performs the physical ownership release after that fence.
            return MetadataPagePoolReclamation{batch.candidate(), 0U, {}};
        }
        bool pruneRoutes = false;
        if (current.usesPageDirectoryBootstrap()) {
            MetadataPageRouteDirectory routes(pages_);
            const auto header = pages_.readPageDirectoryBootstrap(current.page_directory_root);
            // Validate the complete old branch before discarding detached targets.
            const auto registered = routes.collectRoutesFromRouteBranch(current, header.metadata_page_directory_root);
            auto physical = current; physical.page_directory_root = header.data_page_directory_root;
            const auto live = routes.collectRoutesFromPhysicalDirectoryRoots(physical);
            pruneRoutes = registered.size() > live.size();
        }
		if (candidates.empty() && !pruneRoutes) {
			return MetadataPagePoolReclamation{current, 0U, {}};
		}

		check();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
        batch.hybrid_property_publication_ = true;
        if (pruneRoutes) batch.requestMetadataRoutePruning();
		utils::OwnedStdVector<PhysicalSlotId> candidateSlots;
		utils::OwnedVector<SegmentId> candidateSegmentIds;
		candidateSlots.reserve(candidates.size());
		candidateSegmentIds.reserve(candidates.size());
		for (const CatalogEntry &entry: candidates) {
			candidateSlots.push_back(entry.physical_slot);
			candidateSegmentIds.push_back(entry.segment_id);
		}
		std::ranges::sort(candidateSlots);
		if (std::ranges::adjacent_find(candidateSlots) != candidateSlots.end()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata-pool reclaimer found duplicate physical-slot ownership");
		}
		// Reserve every selected pool before allocator/Catalog COW can allocate a
		// metadata frame. Reserving one candidate at a time allowed an allocator
		// root update to reuse a later candidate and made the eventual release
		// transition internally inconsistent.
		for (const PhysicalSlotId slot: candidateSlots) {
			batch.reserveDataSlot(slot);
		}
		for (CatalogEntry &entry: candidates) {
			allocator_.prepareSetAllocated(batch, entry.physical_slot, false);
		}
		if (!candidateSegmentIds.empty()) catalog_.prepareErase(batch, candidateSegmentIds);
		try {
			// The normal publication path establishes route-frame capacity after
			// excluding every released pool, and reconciles newly created pools.
			publishBatch(batch, writer, options);
		} catch (const BootstrapPublishInterrupted &) {
			// An interruption after the inactive Superblock is durable must also
			// retire the in-memory pool registry; otherwise this process could
			// allocate from a slot the durable Allocator has already released.
			if (bootstrap_.recover() == batch.candidate()) {
				metadata_page_pool_.releaseEmptyCatalogPools(candidateSlots);
			}
			throw;
		}
		metadata_page_pool_.releaseEmptyCatalogPools(candidateSlots);
		return MetadataPagePoolReclamation{batch.candidate(), candidateSlots.size(), std::move(candidateSlots)};
#endif
}
} // namespace graph::storage
