/**
 * @file RelocatableMetadataStoreVector.cpp
 * @brief Atomic metadata publication for Vector-owned typed pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/vector/VectorAdjacencyPageStore.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <utility>
#include <vector>

namespace graph::storage {

	namespace {

		// The callback owns the typed bytes, but the metadata coordinator owns
		// reachability. Authenticate only the assigned frames before it publishes
		// their Page Directory routes.
		[[nodiscard]] std::vector<SharedPagePoolPreparedPublication>
		makePreparedVectorPublications(std::span<const PageLocation> locations, uint64_t ownerId,
										PageOwnerKind ownerKind, LogSequenceNumber checkpointLsn) {
			std::vector<SharedPagePoolPreparedPublication> publications;
			publications.reserve(locations.size());
			for (const PageLocation &location: locations) {
				publications.push_back({location, ownerId, ownerKind, checkpointLsn});
			}
			return publications;
		}

	} // namespace

	Superblock RelocatableMetadataStore::materializeNewVectorManifestPage(uint64_t indexId, PhysicalSlotId firstSlot,
																		  PhysicalSlotId lastSlot,
																		  LogSequenceNumber checkpointLsn,
																		  const NewVectorManifestPagePreparer &preparer,
																		  const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(indexId == 0U) | static_cast<uint8_t>(!firstSlot.isValid()) |
									 static_cast<uint8_t>(!lastSlot.isValid()) |
									 static_cast<uint8_t>(firstSlot > lastSlot) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Vector Manifest materialization requires an index, placement, LSN, and preparer");
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::optional<PhysicalSlotId> physicalSlot =
				allocator_.prepareReserveFirstFree(batch, firstSlot, lastSlot);
		if (!physicalSlot.has_value()) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Manifest materialization has no free physical slot in its requested range");
		}
		CatalogEntry pool;
		pool.segment_id = batch.allocateLogicalSegmentId();
		pool.physical_slot = *physicalSlot;
		pool.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
		pool.owner_id = indexId;
		pool.generation = Generation(1);
		pool.state = SegmentState::SGS_SEALED;
		pool.committed_lsn = checkpointLsn;
		catalog_.prepareUpsert(batch, std::span(&pool, 1));

		const PageId pageId = batch.allocateLogicalPageId();
		PageLocation location{pageId, pool.segment_id, 0, pool.generation, 0};
		preparer(pool, pageId, location);
		const uint8_t invalidLocation = static_cast<uint8_t>(location.page_id != pageId) |
										static_cast<uint8_t>(location.segment_id != pool.segment_id) |
										static_cast<uint8_t>(location.generation != pool.generation) |
										static_cast<uint8_t>(location.page_index != RSS_PAGE_POOL_FIRST_DATA_FRAME) |
										static_cast<uint8_t>(location.free_bytes != 0U);
		if (invalidLocation != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Manifest materialization preparer returned an invalid Page Directory location");
		}
		const VectorManifestLocation manifest{indexId, pageId, pool.generation};
		const std::vector<SharedPagePoolPreparedPublication> manifestPublications =
				makePreparedVectorPublications(std::span(&location, 1), indexId, PageOwnerKind::POK_VECTOR_MANIFEST,
										checkpointLsn);
		SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(
				*io_, current.data_region_offset, pool, manifestPublications);
		locations_.prepareUpsertPages(batch, std::span(&location, 1));
		locations_.prepareReplaceVectorManifests(batch, std::span(&manifest, 1), {});
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::materializeNewVectorRawPagesAcrossPools(
			std::span<const VectorRawMaterializationPoolRequest> pools, uint64_t indexId,
			LogSequenceNumber checkpointLsn, const VectorRawMultiPoolMaterializationPreparer &preparer,
			const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(pools.empty()) | static_cast<uint8_t>(indexId == 0U) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable raw Vector materialization requires pools, an index, an LSN, and a preparer");
		}

		size_t totalPageCount = 0;
		for (const VectorRawMaterializationPoolRequest &request: pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!request.first_slot.isValid()) |
										static_cast<uint8_t>(!request.last_slot.isValid()) |
										static_cast<uint8_t>(request.first_slot > request.last_slot) |
										static_cast<uint8_t>(request.page_count == 0U) |
										static_cast<uint8_t>(request.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			const uint8_t pageCountOverflows =
					static_cast<uint8_t>(request.page_count > std::numeric_limits<size_t>::max() - totalPageCount);
			if ((invalidPool | pageCountOverflows) != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable raw Vector materialization has an invalid pool range or page count");
			}
			totalPageCount += request.page_count;
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);

		std::vector<VectorRawMaterializationPoolBatch> batches;
		std::vector<CatalogEntry> catalogEntries;
		batches.reserve(pools.size());
		catalogEntries.reserve(pools.size());
		for (const VectorRawMaterializationPoolRequest &request: pools) {
			const std::optional<PhysicalSlotId> physicalSlot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable raw Vector materialization has no free physical slot in a requested range");
			}
			CatalogEntry pool;
			pool.segment_id = batch.allocateLogicalSegmentId();
			pool.physical_slot = *physicalSlot;
			pool.kind = SegmentKind::SGK_VECTOR_RAW_POOL;
			pool.owner_id = indexId;
			pool.generation = Generation(1);
			pool.state = SegmentState::SGS_SEALED;
			pool.committed_lsn = checkpointLsn;
			catalogEntries.push_back(pool);
			batches.push_back({pool, {}, {}});
		}
		catalog_.prepareUpsert(batch, catalogEntries);

		for (size_t poolOrdinal = 0; poolOrdinal < batches.size(); ++poolOrdinal) {
			VectorRawMaterializationPoolBatch &pool = batches[poolOrdinal];
			const size_t pageCount = pools[poolOrdinal].page_count;
			pool.page_ids.reserve(pageCount);
			pool.locations.reserve(pageCount);
			for (size_t pageOrdinal = 0; pageOrdinal < pageCount; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId)
							.has_value()) { // ZYX_COV_EXCL_LINE: durable page IDs are never reused.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable raw Vector materialization allocated an already reachable Page ID");
				}
				pool.page_ids.push_back(pageId);
				pool.locations.push_back({pageId, pool.pool.segment_id, 0, pool.pool.generation, 0});
			}
		}

		std::vector<VectorLocation> vectorLocations;
		preparer(batches, vectorLocations);

		std::set<PageId> uniquePageIds;
		std::set<std::pair<uint64_t, uint16_t>> uniqueFrames;
		std::set<uint64_t> uniqueVectorIds;
		std::set<std::pair<PageId, uint16_t>> uniqueVectorSlots;
		std::vector<PageLocation> publishedPages;
		publishedPages.reserve(totalPageCount);
		for (const VectorRawMaterializationPoolBatch &pool: batches) {
			if (pool.locations.size() != pool.page_ids.size()) { // ZYX_COV_EXCL_LINE: typed Vector materialization
																 // preserves metadata-assigned cardinality.
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable raw Vector materialization preparer changed a page batch cardinality");
			}
			for (size_t pageOrdinal = 0; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
				const PageLocation &location = pool.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				const std::pair<uint64_t, uint16_t> frame{location.segment_id.value(), location.page_index};
				if (invalidLocation != 0U || !uniquePageIds.insert(location.page_id).second ||
					!uniqueFrames.insert(frame)
							 .second) { // ZYX_COV_EXCL_LINE: typed preparation uses one new frame per assigned page.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable raw Vector materialization returned invalid or duplicate page locations");
				}
				publishedPages.push_back(location);
			}
		}
		for (const VectorLocation &location: vectorLocations) {
			const bool knownPage = std::ranges::any_of(
					publishedPages, [&location](const PageLocation &page) { return page.page_id == location.page_id; });
			const uint8_t invalidLocation =
					static_cast<uint8_t>(location.vector_id == 0U) | static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
					static_cast<uint8_t>(location.slot_generation == 0U) | static_cast<uint8_t>(!knownPage) |
					static_cast<uint8_t>(current.next_logical_vector_id == 0U) |
					static_cast<uint8_t>(location.vector_id < current.next_logical_vector_id) |
					static_cast<uint8_t>(
							locations_.findVectorLocationOptional(current, location.vector_id).has_value());
			if (invalidLocation != 0U || !uniqueVectorIds.insert(location.vector_id).second ||
				!uniqueVectorSlots.insert({location.page_id, location.slot_id}).second) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable raw Vector materialization returned an invalid, duplicate, or "
									  "already-published Vector ID");
			}
		}

		std::ranges::sort(publishedPages, {}, &PageLocation::page_id);
		std::ranges::sort(vectorLocations, {}, &VectorLocation::vector_id);
		for (const VectorRawMaterializationPoolBatch &pool: batches) {
			const std::vector<SharedPagePoolPreparedPublication> publications = makePreparedVectorPublications(
					pool.locations, indexId, PageOwnerKind::POK_VECTOR_RAW, checkpointLsn);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(
					*io_, current.data_region_offset, pool.pool, publications);
		}
		for (const VectorLocation &location: vectorLocations) {
			batch.observeLogicalVectorId(location.vector_id);
		}
		locations_.prepareUpsertPages(batch, publishedPages);
		locations_.prepareReplaceVectorLocations(batch, vectorLocations, {});
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::materializeNewVectorPqPagesAcrossPools(
			std::span<const VectorPqMaterializationPoolRequest> pools, uint64_t indexId,
			LogSequenceNumber checkpointLsn, const VectorPqMultiPoolMaterializationPreparer &preparer,
			const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(pools.empty()) | static_cast<uint8_t>(indexId == 0U) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable PQ Vector materialization requires pools, an index, an LSN, and a preparer");
		}

		size_t totalPageCount = 0U;
		for (const VectorPqMaterializationPoolRequest &request: pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!request.first_slot.isValid()) |
										static_cast<uint8_t>(!request.last_slot.isValid()) |
										static_cast<uint8_t>(request.first_slot > request.last_slot) |
										static_cast<uint8_t>(request.page_count == 0U) |
										static_cast<uint8_t>(request.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			const uint8_t pageCountOverflows =
					static_cast<uint8_t>(request.page_count > std::numeric_limits<size_t>::max() - totalPageCount);
			if ((invalidPool | pageCountOverflows) != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable PQ Vector materialization has an invalid pool range or page count");
			}
			totalPageCount += request.page_count;
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		std::vector<VectorPqMaterializationPoolBatch> batches;
		std::vector<CatalogEntry> catalogEntries;
		batches.reserve(pools.size());
		catalogEntries.reserve(pools.size());
		for (const VectorPqMaterializationPoolRequest &request: pools) {
			const std::optional<PhysicalSlotId> physicalSlot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable PQ Vector materialization has no free physical slot in a requested range");
			}
			CatalogEntry pool;
			pool.segment_id = batch.allocateLogicalSegmentId();
			pool.physical_slot = *physicalSlot;
			pool.kind = SegmentKind::SGK_VECTOR_PQ_POOL;
			pool.owner_id = indexId;
			pool.generation = Generation(1);
			pool.state = SegmentState::SGS_SEALED;
			pool.committed_lsn = checkpointLsn;
			catalogEntries.push_back(pool);
			batches.push_back({pool, {}, {}});
		}
		catalog_.prepareUpsert(batch, catalogEntries);

		for (size_t poolOrdinal = 0; poolOrdinal < batches.size(); ++poolOrdinal) {
			VectorPqMaterializationPoolBatch &pool = batches[poolOrdinal];
			const size_t pageCount = pools[poolOrdinal].page_count;
			pool.page_ids.reserve(pageCount);
			pool.locations.reserve(pageCount);
			for (size_t pageOrdinal = 0; pageOrdinal < pageCount; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId)
							.has_value()) { // ZYX_COV_EXCL_LINE: durable IDs are never reused.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable PQ Vector materialization allocated an already reachable Page ID");
				}
				pool.page_ids.push_back(pageId);
				pool.locations.push_back({pageId, pool.pool.segment_id, 0, pool.pool.generation, 0});
			}
		}

		preparer(batches);
		std::set<PageId> uniquePageIds;
		std::set<std::pair<uint64_t, uint16_t>> uniqueFrames;
		std::vector<PageLocation> publishedPages;
		publishedPages.reserve(totalPageCount);
		for (const VectorPqMaterializationPoolBatch &pool: batches) {
			if (pool.locations.size() !=
				pool.page_ids.size()) { // ZYX_COV_EXCL_LINE: typed preparation preserves cardinality.
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable PQ Vector materialization preparer changed a page batch cardinality");
			}
			for (size_t pageOrdinal = 0; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
				const PageLocation &location = pool.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				const std::pair<uint64_t, uint16_t> frame{location.segment_id.value(), location.page_index};
				if (invalidLocation != 0U || !uniquePageIds.insert(location.page_id).second ||
					!uniqueFrames.insert(frame)
							 .second) { // ZYX_COV_EXCL_LINE: typed preparation assigns one frame per page.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable PQ Vector materialization returned invalid or duplicate page locations");
				}
				publishedPages.push_back(location);
			}
		}
		std::ranges::sort(publishedPages, {}, &PageLocation::page_id);
		for (const VectorPqMaterializationPoolBatch &pool: batches) {
			const std::vector<SharedPagePoolPreparedPublication> publications = makePreparedVectorPublications(
					pool.locations, indexId, PageOwnerKind::POK_VECTOR_PQ, checkpointLsn);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(
					*io_, current.data_region_offset, pool.pool, publications);
		}
		locations_.prepareUpsertPages(batch, publishedPages);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::materializeNewVectorAdjacencyPagesAcrossPools(
			std::span<const VectorAdjacencyMaterializationPoolRequest> pools, uint64_t indexId,
			LogSequenceNumber checkpointLsn, const VectorAdjacencyMultiPoolMaterializationPreparer &preparer,
			const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(pools.empty()) | static_cast<uint8_t>(indexId == 0U) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable adjacency materialization requires pools, an index, an LSN, and a preparer");
		}

		size_t totalPageCount = 0U;
		for (const VectorAdjacencyMaterializationPoolRequest &request: pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!request.first_slot.isValid()) |
										static_cast<uint8_t>(!request.last_slot.isValid()) |
										static_cast<uint8_t>(request.first_slot > request.last_slot) |
										static_cast<uint8_t>(request.page_count == 0U) |
										static_cast<uint8_t>(request.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			const uint8_t pageCountOverflows =
					static_cast<uint8_t>(request.page_count > std::numeric_limits<size_t>::max() - totalPageCount);
			if ((invalidPool | pageCountOverflows) != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable adjacency materialization has an invalid pool range or page count");
			}
			totalPageCount += request.page_count;
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		std::vector<VectorAdjacencyMaterializationPoolBatch> batches;
		std::vector<CatalogEntry> catalogEntries;
		batches.reserve(pools.size());
		catalogEntries.reserve(pools.size());
		for (const VectorAdjacencyMaterializationPoolRequest &request: pools) {
			const std::optional<PhysicalSlotId> physicalSlot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable adjacency materialization has no free physical slot in a requested range");
			}
			CatalogEntry pool;
			pool.segment_id = batch.allocateLogicalSegmentId();
			pool.physical_slot = *physicalSlot;
			pool.kind = SegmentKind::SGK_VECTOR_ADJACENCY_POOL;
			pool.owner_id = indexId;
			pool.generation = Generation(1);
			pool.state = SegmentState::SGS_SEALED;
			pool.committed_lsn = checkpointLsn;
			catalogEntries.push_back(pool);
			batches.push_back({pool, {}, {}});
		}
		catalog_.prepareUpsert(batch, catalogEntries);

		for (size_t poolOrdinal = 0; poolOrdinal < batches.size(); ++poolOrdinal) {
			VectorAdjacencyMaterializationPoolBatch &pool = batches[poolOrdinal];
			const size_t pageCount = pools[poolOrdinal].page_count;
			pool.page_ids.reserve(pageCount);
			pool.locations.reserve(pageCount);
			for (size_t pageOrdinal = 0; pageOrdinal < pageCount; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId)
							.has_value()) { // ZYX_COV_EXCL_LINE: durable IDs are never reused.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable adjacency materialization allocated an already reachable Page ID");
				}
				pool.page_ids.push_back(pageId);
				pool.locations.push_back({pageId, pool.pool.segment_id, 0, pool.pool.generation, 0});
			}
		}

		preparer(batches);
		std::set<PageId> uniquePageIds;
		std::set<std::pair<uint64_t, uint16_t>> uniqueFrames;
		std::vector<PageLocation> publishedPages;
		publishedPages.reserve(totalPageCount);
		for (const VectorAdjacencyMaterializationPoolBatch &pool: batches) {
			if (pool.locations.size() !=
				pool.page_ids.size()) { // ZYX_COV_EXCL_LINE: typed preparation preserves cardinality.
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable adjacency materialization preparer changed a page batch cardinality");
			}
			for (size_t pageOrdinal = 0; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
				const PageLocation &location = pool.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
				const std::pair<uint64_t, uint16_t> frame{location.segment_id.value(), location.page_index};
				if (invalidLocation != 0U || !uniquePageIds.insert(location.page_id).second ||
					!uniqueFrames.insert(frame)
							 .second) { // ZYX_COV_EXCL_LINE: typed preparation assigns one unique frame per page.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable adjacency materialization returned invalid or duplicate page locations");
				}
				publishedPages.push_back(location);
			}
		}
		std::ranges::sort(publishedPages, {}, &PageLocation::page_id);
		const uint64_t adjacencyOwnerId = VectorAdjacencyPageStore::derivedOwnerId(indexId);
		for (const VectorAdjacencyMaterializationPoolBatch &pool: batches) {
			const std::vector<SharedPagePoolPreparedPublication> publications = makePreparedVectorPublications(
					pool.locations, adjacencyOwnerId, PageOwnerKind::POK_VECTOR_ADJACENCY, checkpointLsn);
			SharedPagePoolAllocator::validatePreparedPublicationsUnderWrite(
					*io_, current.data_region_offset, pool.pool, publications);
		}
		locations_.prepareUpsertPages(batch, publishedPages);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

} // namespace graph::storage
