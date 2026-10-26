/**
 * @file SlottedPagePool.cpp
 * @brief Durable initial materialization of typed relocatable slotted pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/MetadataPageReadContext.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"

#include <array>
#include <tuple>
#include <utility>
#include <vector>

namespace graph::storage {

	SlottedPagePool::SlottedPagePool(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
									 SharedPagePoolOpenMode openMode) :
		pool_(std::move(io), metadata, openMode), metadata_(metadata) {}

    SlottedPagePool::SlottedPagePool(std::shared_ptr<StorageIO> io, RelocatableMetadataStore &metadata,
        const MetadataSnapshotPin &snapshot) : pool_(std::move(io), metadata, snapshot), metadata_(metadata) {}

	std::optional<SlottedPagePoolAllocation>
	SlottedPagePool::allocateEmptyPage(const SlottedPagePoolAllocationRequest &request, PhysicalSlotId firstSlot,
									   PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
									   const MetadataPublishOptions &options) {
		PageOwnerHeader owner;
		owner.page_id = request.page_id;
		owner.owner_id = request.page_owner_id;
		owner.kind = request.page_owner_kind;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = request.generation;
		owner.committed_lsn = checkpointLsn;
		SlottedPage page = SlottedPage::create(owner);
		const std::vector<std::byte> bytes = page.serialize();
		SharedPagePoolAllocationRequest poolRequest;
		poolRequest.segment_id = request.segment_id;
		poolRequest.pool_kind = request.pool_kind;
		poolRequest.pool_owner_id = request.pool_owner_id;
		poolRequest.generation = request.generation;
		poolRequest.page_id = request.page_id;
		poolRequest.page_owner_id = request.page_owner_id;
		poolRequest.page_owner_kind = request.page_owner_kind;
		poolRequest.page_free_bytes = page.stats().contiguous_free_bytes;
		poolRequest.payload = bytes;
		const std::optional<SharedPagePoolAllocation> allocation =
				pool_.allocateNewPoolPage(poolRequest, firstSlot, lastSlot, checkpointLsn, options);
		if (!allocation.has_value()) {
			return std::nullopt;
		}
		recordPublishedPage(*allocation, page.stats());
		return SlottedPagePoolAllocation{*allocation, std::move(page)};
	}

	std::optional<SlottedPagePoolAllocation>
	SlottedPagePool::allocatePage(const SlottedPagePoolAllocationRequest &request, const SlottedPage &page,
								  PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
								  const MetadataPublishOptions &options) {
		const uint8_t invalid = static_cast<uint8_t>(page.pageId() != request.page_id) |
								static_cast<uint8_t>(page.pageGeneration() != request.generation) |
								static_cast<uint8_t>(page.ownerId() != request.page_owner_id) |
								static_cast<uint8_t>(page.ownerKind() != request.page_owner_kind) |
								static_cast<uint8_t>(page.ownerCommittedLsn() != checkpointLsn);
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable populated new slotted page does not match its allocation ownership");
		}
		SharedPagePoolAllocationRequest poolRequest;
		poolRequest.segment_id = request.segment_id;
		poolRequest.pool_kind = request.pool_kind;
		poolRequest.pool_owner_id = request.pool_owner_id;
		poolRequest.generation = request.generation;
		poolRequest.page_id = request.page_id;
		poolRequest.page_owner_id = request.page_owner_id;
		poolRequest.page_owner_kind = request.page_owner_kind;
		poolRequest.page_free_bytes = page.stats().contiguous_free_bytes;
		const std::vector<std::byte> bytes = page.serialize();
		poolRequest.payload = bytes;
		const std::optional<SharedPagePoolAllocation> allocation =
				pool_.allocateNewPoolPage(poolRequest, firstSlot, lastSlot, checkpointLsn, options);
		if (!allocation.has_value()) {
			return std::nullopt;
		}
		recordPublishedPage(*allocation, page.stats());
		PageOwnerHeader owner;
		owner.page_id = request.page_id;
		owner.owner_id = request.page_owner_id;
		owner.kind = request.page_owner_kind;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = request.generation;
		owner.committed_lsn = checkpointLsn;
		return SlottedPagePoolAllocation{*allocation, SlottedPage::open(owner, bytes)};
	}

	SlottedPagePoolAllocation SlottedPagePool::appendEmptyPage(const SlottedPagePoolAppendRequest &request,
															   LogSequenceNumber checkpointLsn,
															   const MetadataPublishOptions &options) {
		PageOwnerHeader owner;
		owner.page_id = request.page_id;
		owner.owner_id = request.page_owner_id;
		owner.kind = request.page_owner_kind;
		owner.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
		owner.generation = request.generation;
		owner.committed_lsn = checkpointLsn;
		SlottedPage page = SlottedPage::create(owner);
		const std::vector<std::byte> bytes = page.serialize();
		SharedPagePoolAppendRequest poolRequest;
		poolRequest.segment_id = request.segment_id;
		poolRequest.generation = request.generation;
		poolRequest.page_id = request.page_id;
		poolRequest.page_owner_id = request.page_owner_id;
		poolRequest.page_owner_kind = request.page_owner_kind;
		poolRequest.page_free_bytes = page.stats().contiguous_free_bytes;
		poolRequest.payload = bytes;
		const SharedPagePoolAllocation allocation = pool_.appendPage(poolRequest, checkpointLsn, options);
		recordPublishedPage(allocation, page.stats());
		return {allocation, std::move(page)};
	}

	SharedPagePoolAllocation SlottedPagePool::appendPage(const SlottedPagePoolAppendRequest &request,
														 const SlottedPage &page, LogSequenceNumber checkpointLsn,
														 const MetadataPublishOptions &options) {
		const uint8_t invalid = static_cast<uint8_t>(page.pageId() != request.page_id) |
								static_cast<uint8_t>(page.pageGeneration() != request.generation) |
								static_cast<uint8_t>(page.ownerId() != request.page_owner_id) |
								static_cast<uint8_t>(page.ownerKind() != request.page_owner_kind) |
								static_cast<uint8_t>(page.ownerCommittedLsn() != checkpointLsn);
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable populated slotted page does not match its append ownership");
		}
		const SlottedPageStats stats = page.stats();
		SharedPagePoolAppendRequest poolRequest;
		poolRequest.segment_id = request.segment_id;
		poolRequest.generation = request.generation;
		poolRequest.page_id = request.page_id;
		poolRequest.page_owner_id = request.page_owner_id;
		poolRequest.page_owner_kind = request.page_owner_kind;
		poolRequest.page_free_bytes = stats.contiguous_free_bytes;
		const std::vector<std::byte> bytes = page.serialize();
		poolRequest.payload = bytes;
		const SharedPagePoolAllocation allocation = pool_.appendPage(poolRequest, checkpointLsn, options);
		recordPublishedPage(allocation, page.stats());
		return allocation;
	}

	void SlottedPagePool::prepareAppendPages(const CatalogEntry &pool,
											 std::span<const SharedPagePoolPreparedAppend> pages,
											 std::span<PageLocation> locations, LogSequenceNumber checkpointLsn) const {
		pool_.prepareAppendPages(pool, pages, locations, checkpointLsn);
	}

	void SlottedPagePool::prepareNewPoolPages(const CatalogEntry &pool,
											  std::span<const SharedPagePoolPreparedAppend> pages,
											  std::span<PageLocation> locations, LogSequenceNumber checkpointLsn,
											  size_t preparedPrefixCount) const {
		pool_.prepareNewPoolPages(pool, pages, locations, checkpointLsn, preparedPrefixCount);
	}

	void SlottedPagePool::prepareMutationPages(const CatalogEntry &pool,
											   std::span<const PageLocation> expectedReplacements,
											   std::span<const SharedPagePoolPreparedReplacement> replacements,
											   std::span<const SharedPagePoolPreparedAppend> appends,
											   std::span<PageLocation> replacementLocations,
											   std::span<PageLocation> appendLocations, LogSequenceNumber checkpointLsn,
											   std::span<const SharedPagePoolPreparedRetirement> retirements) const {
		pool_.prepareMutationPages(pool, expectedReplacements, replacements, appends, replacementLocations,
								   appendLocations, checkpointLsn, retirements);
	}

	void SlottedPagePool::prepareCrossPoolReplacementPages(
			const CatalogEntry &destinationPool,
			std::span<const SharedPagePoolPreparedCrossPoolReplacement> replacements,
			std::span<PageLocation> replacementLocations, LogSequenceNumber checkpointLsn) const {
		pool_.prepareCrossPoolReplacementPages(destinationPool, replacements, replacementLocations, checkpointLsn);
	}

	SlottedPage SlottedPagePool::read(const MetadataSnapshotPin &snapshot, PageId pageId,
									  BufferPoolReadHint hint) const {
		const SharedPagePoolContents contents = pool_.read(snapshot, pageId, hint);
		SlottedPage page = SlottedPage::open(contents.owner, contents.payload,
			utils::OwnedAllocationScope::active() ? utils::OwnedAllocationScope::owner() : (snapshot.readContext() ? snapshot.readContext()->memoryOwner() : nullptr));
		if (contents.location.free_bytes != page.stats().contiguous_free_bytes) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page free-space index disagrees with its page payload");
		}
		return page;
	}

	std::optional<SlottedPagePinnedRead>
	SlottedPagePool::readPinned(const MetadataSnapshotPin &snapshot, const PageId pageId,
									 BufferPoolReadHint hint) const {
		std::optional<SharedPagePoolPinnedView> contents = pool_.readPinned(snapshot, pageId, hint);
		if (!contents.has_value()) {
			return std::nullopt;
		}
		SlottedPageReadView page = SlottedPageReadView::open(contents->owner, contents->payload());
		if (contents->location.free_bytes != page.stats().contiguous_free_bytes) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable pinned slotted page free-space index disagrees with its page payload");
		}
		return SlottedPagePinnedRead{std::move(*contents), std::move(page)};
	}

	SharedPagePoolAllocation SlottedPagePool::replacePage(const SlottedPage &page, LogSequenceNumber checkpointLsn,
														  const MetadataPublishOptions &options) {
		const std::vector<std::byte> bytes = page.serialize();
		const LogSequenceNumber expectedOwnerLsn = page.ownerCommittedLsn();
		if (!expectedOwnerLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page replacement requires a page read from an owned frame");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const SharedPagePoolContents current = pool_.read(snapshot, page.pageId());
		if (current.owner.flags != 0U || current.owner.committed_lsn != expectedOwnerLsn) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page replacement requires the current live page owner");
		}
		const SlottedPage replacementPage = SlottedPage::open(current.owner, bytes);
		snapshot.release();
		SharedPagePoolReplacementRequest request;
		request.segment_id = current.pool.segment_id;
		request.generation = current.pool.generation;
		request.page_id = current.owner.page_id;
		request.expected_owner_lsn = expectedOwnerLsn;
		request.page_owner_id = current.owner.owner_id;
		request.page_owner_kind = current.owner.kind;
		request.page_free_bytes = replacementPage.stats().contiguous_free_bytes;
		request.payload = bytes;
		const SharedPagePoolAllocation allocation = pool_.replacePage(request, checkpointLsn, options);
		recordPublishedPage(allocation, replacementPage.stats());
		return allocation;
	}

	SharedPagePoolAllocation SlottedPagePool::evacuatePage(const SlottedPage &page, SegmentId destinationSegmentId,
														   LogSequenceNumber checkpointLsn,
														   const MetadataPublishOptions &options) {
		const LogSequenceNumber expectedOwnerLsn = page.ownerCommittedLsn();
		if (!destinationSegmentId.isValid() || !expectedOwnerLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page evacuation requires a destination and current page owner");
		}
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		const SharedPagePoolContents current = pool_.read(snapshot, page.pageId());
		const CatalogEntry destinationPool = metadata_.findCatalog(snapshot, destinationSegmentId);
		if (current.pool.segment_id == destinationPool.segment_id || current.owner.flags != 0U ||
			current.owner.committed_lsn != expectedOwnerLsn) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page evacuation requires the current live source owner");
		}
		SlottedPage relocated = SlottedPage::open(current.owner, page.serialize());
		PageOwnerHeader destinationOwner = current.owner;
		destinationOwner.generation = destinationPool.generation;
		destinationOwner.committed_lsn = checkpointLsn;
		relocated.rebindOwner(destinationOwner);
		const std::vector<std::byte> bytes = relocated.serialize();
		snapshot.release();
		SharedPagePoolEvacuationRequest request;
		request.source_segment_id = current.pool.segment_id;
		request.source_generation = current.pool.generation;
		request.destination_segment_id = destinationPool.segment_id;
		request.destination_generation = destinationPool.generation;
		request.page_id = current.owner.page_id;
		request.expected_owner_lsn = expectedOwnerLsn;
		request.page_owner_id = current.owner.owner_id;
		request.page_owner_kind = current.owner.kind;
		request.page_free_bytes = relocated.stats().contiguous_free_bytes;
		request.payload = bytes;
		const SharedPagePoolAllocation allocation = pool_.evacuatePage(request, checkpointLsn, options);
		invalidateFreeSpaceIndex(current.pool.segment_id);
		recordPublishedPage(allocation, relocated.stats());
		return allocation;
	}

	std::optional<PageId> SlottedPagePool::findBestFitPage(SegmentId segmentId,
														   uint32_t minimumContiguousFreeBytes) const {
		if (!segmentId.isValid()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable slotted page selection requires a valid pool and bounded free-byte request");
		}
		validateSelectionRequest(minimumContiguousFreeBytes, std::nullopt);
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		if (availableFrameCount(snapshot, segmentId) == 0U) {
			return std::nullopt;
		}
		const std::optional<PageLocation> location =
				findBestFitLocation(snapshot, segmentId, minimumContiguousFreeBytes, std::nullopt);
		return location.has_value() ? std::optional<PageId>(location->page_id) : std::nullopt;
	}

	std::optional<SlottedPagePlacement> SlottedPagePool::findBestFitPage(std::span<const SegmentId> segmentIds,
																		 uint32_t minimumContiguousFreeBytes) const {
		if (segmentIds.empty()) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable cross-pool page selection requires pools and a bounded free-byte request");
		}
		validateSelectionRequest(minimumContiguousFreeBytes, std::nullopt);
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		std::optional<SlottedPagePlacement> best;
		for (const SegmentId segmentId: segmentIds) {
			if (!segmentId.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable cross-pool page selection contains an invalid Segment ID");
			}
			// A current page mutation is COW: it needs one new frame in the same
			// pool even when the page itself still has record-space available.
			if (availableFrameCount(snapshot, segmentId) == 0U) {
				continue;
			}
			const std::optional<PageLocation> candidate =
					findBestFitLocation(snapshot, segmentId, minimumContiguousFreeBytes, std::nullopt);
			if (!candidate.has_value()) {
				continue;
			}
			const SlottedPagePlacement placement{candidate->segment_id, candidate->page_id, candidate->free_bytes};
			if (!best.has_value() ||
				std::tie(placement.contiguous_free_bytes, placement.segment_id, placement.page_id) <
						std::tie(best->contiguous_free_bytes, best->segment_id, best->page_id)) {
				best = placement;
			}
		}
		return best;
	}

	std::optional<PageId> SlottedPagePool::findBestFitPage(SegmentId segmentId, PageOwnerKind ownerKind,
														   uint32_t minimumContiguousFreeBytes) const {
		if (!segmentId.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable typed slotted page selection requires a valid pool");
		}
		validateSelectionRequest(minimumContiguousFreeBytes, ownerKind);
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		if (availableFrameCount(snapshot, segmentId) == 0U) {
			return std::nullopt;
		}
		const std::optional<PageLocation> location =
				findBestFitLocation(snapshot, segmentId, minimumContiguousFreeBytes, ownerKind);
		return location.has_value() ? std::optional<PageId>(location->page_id) : std::nullopt;
	}

	std::optional<SlottedPagePlacement> SlottedPagePool::findBestFitPage(std::span<const SegmentId> segmentIds,
																		 PageOwnerKind ownerKind,
																		 uint32_t minimumContiguousFreeBytes) const {
		if (segmentIds.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable typed cross-pool page selection requires candidate pools");
		}
		validateSelectionRequest(minimumContiguousFreeBytes, ownerKind);
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		std::optional<SlottedPagePlacement> best;
		for (const SegmentId segmentId: segmentIds) {
			if (!segmentId.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable typed cross-pool page selection contains an invalid Segment ID");
			}
			if (availableFrameCount(snapshot, segmentId) == 0U) {
				continue;
			}
			const std::optional<PageLocation> candidate =
					findBestFitLocation(snapshot, segmentId, minimumContiguousFreeBytes, ownerKind);
			if (!candidate.has_value()) {
				continue;
			}
			const SlottedPagePlacement placement{candidate->segment_id, candidate->page_id, candidate->free_bytes};
			if (!best.has_value() ||
				std::tie(placement.contiguous_free_bytes, placement.segment_id, placement.page_id) <
						std::tie(best->contiguous_free_bytes, best->segment_id, best->page_id)) {
				best = placement;
			}
		}
		return best;
	}

	std::optional<SlottedPagePlacement> SlottedPagePool::findBestFitPageForRecord(std::span<const SegmentId> segmentIds,
																				  PageOwnerKind ownerKind,
																				  uint32_t recordBytes) const {
		MetadataSnapshotPin snapshot = metadata_.openSnapshot();
		return findBestFitPageForRecord(snapshot, segmentIds, ownerKind, recordBytes);
	}

	std::optional<SlottedPagePlacement> SlottedPagePool::findBestFitPageForRecord(const MetadataSnapshotPin &snapshot,
																				  std::span<const SegmentId> segmentIds,
																				  PageOwnerKind ownerKind,
																				  uint32_t recordBytes) const {
		if (segmentIds.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable typed record placement requires candidate pools");
		}
		validateRecordSelectionRequest(recordBytes, ownerKind);
		// The priority pass can return early, but the whole request must still
		// satisfy the existing candidate identity contract before any selection.
		if (std::ranges::any_of(segmentIds, [](SegmentId id) { return !id.isValid(); }))
			throw FormatException(FormatError::FERR_INVALID_HEADER,
				"relocatable typed record placement contains an invalid Segment ID");
		// Completed scan images may have hundreds of native-only pools in the
		// same numeric owner namespace as ordinary ROW pages. A cold ROW writer
		// must not authenticate every native payload merely to find one usable
		// ROW page. A sampled owner is only an ordering hint: a selected pool
		// still goes through the complete existing placement validation below.
		// If the priority pass finds nothing (or I/O changes), retain the full
		// search. BUILDING keeps its global best-fit space policy unchanged.
		if (RSS_SCAN_PATH_FORMAT_ENABLED && ownerKind == PageOwnerKind::POK_PROPERTY &&
			snapshot.superblock().hybrid_property_descriptor_root.isValid() &&
			!(snapshot.superblock().flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING)) {
			const uint64_t epoch = metadata_.dataMutationEpoch();
			if (epoch != UINT64_MAX) for (const SegmentId segmentId : segmentIds) {
				if (!segmentId.isValid())
					throw FormatException(FormatError::FERR_INVALID_HEADER,
						"relocatable typed record placement contains an invalid Segment ID");
				if (availableFrameCount(snapshot, segmentId) == 0U) continue;
				bool nativeHint = false;
				try {
					const auto pool = metadata_.findCatalog(snapshot, segmentId);
					const auto header = pool_.readPoolHeader(pool);
					for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
						if (!SharedPagePoolFrameMap::isAllocated(header, frame)) continue;
						const auto sampled = pool_.readPageOwner(pool, frame);
						nativeHint = sampled.generation == pool.generation && sampled.kind == PageOwnerKind::POK_HYBRID_PROPERTY;
						break;
					}
				} catch (const FormatException &) {
					// An undecodable hint cannot exclude a pool. The ordinary index
					// builder below resolves retired/private versus damaged pages.
				}
				if (metadata_.dataMutationEpoch() != epoch) break;
				if (nativeHint) {
					debug::PerfTrace::addValue("storage.slotted.native_row_pool_hints", 1);
					continue;
				}
				const auto candidate = findBestFitRecordLocation(snapshot, segmentId, ownerKind, recordBytes);
				if (metadata_.dataMutationEpoch() != epoch) break;
				if (candidate) return SlottedPagePlacement{
					candidate->location.segment_id, candidate->location.page_id, candidate->location.free_bytes};
			}
		}
		std::optional<RecordPlacement> best;
		for (const SegmentId segmentId: segmentIds) {
			if (!segmentId.isValid()) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable typed record placement contains an invalid Segment ID");
			}
			if (availableFrameCount(snapshot, segmentId) == 0U) {
				continue;
			}
			const std::optional<RecordPlacement> candidate =
					findBestFitRecordLocation(snapshot, segmentId, ownerKind, recordBytes);
			if (!candidate.has_value()) {
				continue;
			}
			const uint32_t candidateRemaining = candidate->location.free_bytes - candidate->required_free_bytes;
			const uint32_t bestRemaining =
					best.has_value() ? best->location.free_bytes - best->required_free_bytes : 0U;
			if (!best.has_value() ||
				std::tie(candidateRemaining, candidate->location.segment_id, candidate->location.page_id) <
						std::tie(bestRemaining, best->location.segment_id, best->location.page_id)) {
				best = candidate;
			}
		}
		if (!best.has_value()) {
			return std::nullopt;
		}
		return SlottedPagePlacement{best->location.segment_id, best->location.page_id, best->location.free_bytes};
	}

	uint8_t SlottedPagePool::availableFrameCount(const MetadataSnapshotPin &snapshot, SegmentId segmentId) const {
		if (!segmentId.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable slotted page-pool capacity query requires a valid Segment ID");
		}
		return pool_.availableFrameCount(metadata_.findCatalog(snapshot, segmentId));
	}

	std::optional<PageLocation> SlottedPagePool::findBestFitLocation(const MetadataSnapshotPin &snapshot,
															 SegmentId segmentId,
															 uint32_t minimumContiguousFreeBytes,
															 std::optional<PageOwnerKind> ownerKind) const {
		{
			std::lock_guard lock(free_space_mutex_);
			const auto existing = free_space_indexes_.find(segmentId);
			if (existing != free_space_indexes_.end() &&
				existing->second.metadata_generation == snapshot.superblock().generation) {
				return selectBestFitLocation(existing->second, minimumContiguousFreeBytes, ownerKind);
			}
		}
		PoolFreeSpaceIndex rebuilt = buildFreeSpaceIndex(snapshot, segmentId);
		const std::optional<PageLocation> selection =
				selectBestFitLocation(rebuilt, minimumContiguousFreeBytes, ownerKind);
		cacheFreeSpaceIndexIfNotOlder(segmentId, std::move(rebuilt));
		return selection;
	}

	std::optional<SlottedPagePool::RecordPlacement>
	SlottedPagePool::findBestFitRecordLocation(const MetadataSnapshotPin &snapshot, SegmentId segmentId,
											   PageOwnerKind ownerKind, uint32_t recordBytes) const {
		{
			std::lock_guard lock(free_space_mutex_);
			const auto existing = free_space_indexes_.find(segmentId);
			if (existing != free_space_indexes_.end() &&
				existing->second.metadata_generation == snapshot.superblock().generation) {
				return selectBestFitRecordLocation(existing->second, ownerKind, recordBytes);
			}
		}
		PoolFreeSpaceIndex rebuilt = buildFreeSpaceIndex(snapshot, segmentId);
		const std::optional<RecordPlacement> selection = selectBestFitRecordLocation(rebuilt, ownerKind, recordBytes);
		cacheFreeSpaceIndexIfNotOlder(segmentId, std::move(rebuilt));
		return selection;
	}

	void SlottedPagePool::cacheFreeSpaceIndexIfNotOlder(SegmentId segmentId, PoolFreeSpaceIndex rebuilt) const {
		{
			std::lock_guard lock(free_space_mutex_);
			const auto existing = free_space_indexes_.find(segmentId);
			// An old snapshot may use its local index but must not replace a newer
			// cache entry that represents the current publication.
			if (existing == free_space_indexes_.end() ||
				existing->second.metadata_generation.value() <= rebuilt.metadata_generation.value()) {
				free_space_indexes_[segmentId] = std::move(rebuilt);
			}
		}
	}

	std::optional<PageLocation> SlottedPagePool::selectBestFitLocation(const PoolFreeSpaceIndex &index,
																	   uint32_t minimumContiguousFreeBytes,
																	   std::optional<PageOwnerKind> ownerKind) {
		if (!index.owner_kind.has_value() || (ownerKind.has_value() && *index.owner_kind != *ownerKind)) {
			return std::nullopt;
		}
		const auto bucket = index.pages.pages_by_free_bytes.lower_bound(minimumContiguousFreeBytes);
		if (bucket == index.pages.pages_by_free_bytes.end()) {
			return std::nullopt;
		}
		if (bucket->second.empty()) { // ZYX_COV_EXCL_LINE: cache maintenance never stores empty buckets.
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page free-space cache has an empty selection bucket");
		}
		return index.pages.pages.at(*bucket->second.begin()).location;
	}

	std::optional<SlottedPagePool::RecordPlacement>
	SlottedPagePool::selectBestFitRecordLocation(const PoolFreeSpaceIndex &index, PageOwnerKind ownerKind,
												 uint32_t recordBytes) {
		if (!index.owner_kind.has_value() || *index.owner_kind != ownerKind) {
			return std::nullopt;
		}
		std::optional<RecordPlacement> best;
		for (const auto &[pageId, page]: index.pages.pages) {
			static_cast<void>(pageId);
			const uint32_t required =
					recordBytes + (page.reusable_free_slot_count == 0U ? RSS_SLOTTED_PAGE_SLOT_BYTES : 0U);
			if (page.location.free_bytes < required) {
				continue;
			}
			const uint32_t remaining = page.location.free_bytes - required;
			const uint32_t bestRemaining =
					best.has_value() ? best->location.free_bytes - best->required_free_bytes : 0U;
			if (!best.has_value() ||
				std::tie(remaining, page.location.page_id) < std::tie(bestRemaining, best->location.page_id)) {
				best = RecordPlacement{page.location, required};
			}
		}
		return best;
	}

	void SlottedPagePool::validateSelectionRequest(uint32_t minimumContiguousFreeBytes,
												   std::optional<PageOwnerKind> ownerKind) {
		const uint8_t invalid =
				static_cast<uint8_t>(minimumContiguousFreeBytes == 0U) |
				static_cast<uint8_t>(minimumContiguousFreeBytes >
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES) |
				static_cast<uint8_t>(ownerKind.has_value() && !SlottedPage::supportsOwnerKind(*ownerKind));
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable slotted page selection has an invalid owner kind or free-byte request");
		}
	}

	void SlottedPagePool::validateRecordSelectionRequest(uint32_t recordBytes, PageOwnerKind ownerKind) {
		const uint8_t invalid =
				static_cast<uint8_t>(recordBytes == 0U) |
				static_cast<uint8_t>(recordBytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES) |
				static_cast<uint8_t>(!SlottedPage::supportsOwnerKind(ownerKind));
		if (invalid != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable typed record placement has an invalid owner kind or record size");
		}
	}

	void SlottedPagePool::reclaimRetiredFrames(SegmentId segmentId) const { pool_.reclaimRetiredFrames(segmentId); }

	void SlottedPagePool::reconcilePool(SegmentId segmentId) const { pool_.reconcilePool(segmentId); }

	Superblock SlottedPagePool::retireEmptyPool(SegmentId segmentId, Generation expectedGeneration,
												LogSequenceNumber checkpointLsn,
												const MetadataPublishOptions &options) {
		const Superblock retired = pool_.retireEmptyPool(segmentId, expectedGeneration, checkpointLsn, options);
		invalidateFreeSpaceIndex(segmentId);
		return retired;
	}

	Superblock SlottedPagePool::releaseRetiredEmptyPool(SegmentId segmentId, Generation expectedGeneration,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options) {
		const Superblock released =
				pool_.releaseRetiredEmptyPool(segmentId, expectedGeneration, checkpointLsn, options);
		invalidateFreeSpaceIndex(segmentId);
		return released;
	}

    std::optional<Superblock> SlottedPagePool::tryReleaseRetiredEmptyPool(SegmentId segmentId, Generation expectedGeneration,
														LogSequenceNumber checkpointLsn,
														const MetadataPublishOptions &options) {
        auto result = pool_.tryReleaseRetiredEmptyPool(segmentId, expectedGeneration, checkpointLsn, options);
        if (result) invalidateFreeSpaceIndex(segmentId);
        return result;
    }

	SlottedPagePool::PoolFreeSpaceIndex SlottedPagePool::buildFreeSpaceIndex(const MetadataSnapshotPin &snapshot,
																			 SegmentId segmentId) const {
		PoolFreeSpaceIndex rebuilt;
		rebuilt.metadata_generation = snapshot.superblock().generation;
        const CatalogEntry pool = metadata_.findCatalog(snapshot, segmentId);
		// A pool contains at most 31 data frames. Discover its current pages from
		// authenticated owner IDs and point mappings instead of filtering a scan
		// of every Page Directory entry in the database.
		const auto header = pool_.readPoolHeader(pool);
		std::array<PageLocation, RSS_PAGE_POOL_DATA_FRAME_COUNT> candidates{};
		std::array<PageOwnerKind, RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U> ownerKinds{};
		size_t candidateCount = 0U;
		bool authenticatedOwners = true;
		for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
			if (!SharedPagePoolFrameMap::isAllocated(header, frame)) continue;
			debug::PerfTrace::addValue("storage.slotted.free_index.pool_frames", 1);
			PageOwnerHeader owner;
			try {
				owner = pool_.readPageOwner(pool, frame);
			} catch (const FormatException &) {
				// An unreachable failed private write need not contain a valid owner.
				// Only the authoritative directory can distinguish that case from
				// a damaged live frame. Do not silently omit an undecodable owner.
				authenticatedOwners = false;
				break;
			}
			if (owner.generation != pool.generation) {
				authenticatedOwners = false;
				break;
			}
			debug::PerfTrace::addValue("storage.slotted.free_index.metadata_point_lookups", 1);
			const auto location = metadata_.findPageLocationOptional(snapshot, owner.page_id);
			// Old pins, retired COW frames and cross-pool moves can share an owner
			// ID. Only the exact frame selected by this snapshot is a candidate.
			if (!location || location->segment_id != pool.segment_id || location->page_index != frame) continue;
			if (location->generation != pool.generation) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
						"slotted free-space candidate has a stale pool generation");
			}
			candidates[candidateCount++] = *location;
			ownerKinds[frame] = owner.kind;
		}
		std::vector<PageLocation> authoritative;
		std::span<const PageLocation> current(candidates.data(), candidateCount);
		if (!authenticatedOwners) {
			debug::PerfTrace::addValue("storage.slotted.free_index.directory_fallbacks", 1);
			authoritative = metadata_.findPageLocationsForSegment(snapshot, segmentId);
			current = authoritative;
		}
		debug::PerfTrace::addValue("storage.slotted.free_index.live_candidate_pages", current.size());
		for (const PageLocation &location: current) {
			if (location.free_bytes > RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_SLOTTED_PAGE_HEADER_BYTES) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable slotted page free-space index exceeds the slotted-page payload limit");
			}
			// Numeric owners are shared by ROW and native fields. Authenticate
            // the native page through its private read route, but never offer it
            // as ordinary slotted allocation capacity.
            const bool native = RSS_HYBRID_PROPERTY_FORMAT_ENABLED &&
                pool.kind == SegmentKind::SGK_PROPERTY_PAGE_POOL &&
                (authenticatedOwners ? ownerKinds[location.page_index] : pool_.readPageOwner(pool, location.page_index).kind)
						== PageOwnerKind::POK_HYBRID_PROPERTY;
            const SharedPagePoolContents contents = pool_.readKnownLocation(
                snapshot, pool, location, BufferPoolReadHint::BPRH_NORMAL, native);
			const PageOwnerKind ownerKind = contents.owner.kind;
			if (!SlottedPage::supportsOwnerKind(ownerKind)) {
				continue;
			}
			const SlottedPageStats stats = SlottedPage::open(contents.owner, contents.payload).stats();
			if (location.free_bytes != stats.contiguous_free_bytes) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable slotted page free-space index disagrees with its page payload");
			}
			if (rebuilt.owner_kind.has_value() && *rebuilt.owner_kind != ownerKind) { // ZYX_COV_EXCL_LINE:
				// SharedPagePool validates the compatible owner-kind namespace before this cache is built.
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable slotted page pool contains incompatible owner kinds");
			}
			rebuilt.owner_kind = ownerKind;
            if (native) continue;
			if (!rebuilt.pages.pages
						 .emplace(location.page_id,
								  OwnerFreeSpaceIndex::PageInfo{location, stats.reusable_free_slot_count})
						 .second) { // ZYX_COV_EXCL_LINE: Page Directory keys are unique.
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable slotted page free-space index contains a duplicate Page ID");
			}
			rebuilt.pages.pages_by_free_bytes[location.free_bytes].insert(location.page_id);
		}
		return rebuilt;
	}

	void SlottedPagePool::recordPublishedPage(const SharedPagePoolAllocation &allocation,
											  const SlottedPageStats &stats) const {
		const PageLocation &location = allocation.location;
		if (location.free_bytes >
			RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES -
					RSS_SLOTTED_PAGE_HEADER_BYTES) { // ZYX_COV_EXCL_LINE: this private path receives only validated
													 // slotted-page stats.
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable published slotted page has an invalid contiguous free-byte count");
		}
		if (location.free_bytes != stats.contiguous_free_bytes) { // ZYX_COV_EXCL_LINE: this private path receives
			// the allocation's validated SlottedPage stats.
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable published slotted page free-space hint disagrees with its payload");
		}
		std::lock_guard lock(free_space_mutex_);
		auto iterator = free_space_indexes_.find(location.segment_id);
		if (iterator == free_space_indexes_.end() ||
			iterator->second.metadata_generation.value() + 1U != allocation.superblock.generation.value()) {
			free_space_indexes_.erase(location.segment_id);
			return;
		}
		if (!SlottedPage::supportsOwnerKind(allocation.owner.kind)) { // ZYX_COV_EXCL_LINE: allocation ownership
			// is validated by SharedPagePool before this adapter observes it.
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page publication has an unsupported owner kind");
		}
		if (iterator->second.owner_kind.has_value() &&
			*iterator->second.owner_kind != allocation.owner.kind) { // ZYX_COV_EXCL_LINE
			// One SharedPagePool accepts only its compatible slotted owner namespace.
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable slotted page publication changes a pool's slotted owner kind");
		}
		iterator->second.owner_kind = allocation.owner.kind;
		OwnerFreeSpaceIndex &ownerIndex = iterator->second.pages;
		const auto previous = ownerIndex.pages.find(location.page_id);
		if (previous !=
			ownerIndex.pages.end()) { // ZYX_COV_EXCL_LINE: a current cache tracks only this adapter's pages.
			auto bucket = ownerIndex.pages_by_free_bytes.find(previous->second.location.free_bytes);
			if (bucket == ownerIndex.pages_by_free_bytes.end() ||
				bucket->second.erase(location.page_id) != 1U) { // ZYX_COV_EXCL_LINE
				// Both maps are updated together while holding free_space_mutex_.
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable slotted page free-space cache lost its previous Page ID");
			}
			if (bucket->second.empty()) {
				ownerIndex.pages_by_free_bytes.erase(bucket);
			}
		}
		ownerIndex.pages[location.page_id] = {location, stats.reusable_free_slot_count};
		ownerIndex.pages_by_free_bytes[location.free_bytes].insert(location.page_id);
		iterator->second.metadata_generation = allocation.superblock.generation;
	}

	void SlottedPagePool::invalidateFreeSpaceIndex(SegmentId segmentId) const {
		std::lock_guard lock(free_space_mutex_);
		free_space_indexes_.erase(segmentId);
	}

} // namespace graph::storage
