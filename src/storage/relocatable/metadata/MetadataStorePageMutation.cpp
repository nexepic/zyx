#include "MetadataStoreOperationsInternal.hpp"

namespace graph::storage {

	Superblock RelocatableMetadataStore::materializeNewIndexTreePages(SegmentId poolSegmentId, uint64_t indexId,
																	  size_t pageCount, size_t rootPageOrdinal,
																	  LogSequenceNumber checkpointLsn,
																	  const NewIndexTreePagesPreparer &preparer,
																	  const MetadataPublishOptions &options) {
		if (!poolSegmentId.isValid() || indexId == 0U || pageCount == 0U ||
			pageCount > RSS_PAGE_POOL_DATA_FRAME_COUNT || rootPageOrdinal >= pageCount ||
			!preparer) { // ZYX_COV_EXCL_LINE:
			// only BTreePageStore reaches this private boundary after input validation.
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable B+Tree materialization requires a bounded page batch, root ordinal, and preparer");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry pool = catalog_.find(current, poolSegmentId);
		const uint8_t invalidPool = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
									static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
		if (invalidPool != 0U) { // ZYX_COV_EXCL_LINE: BTreePageStore validates the requested pool before this
			// private metadata boundary; retain the check for a relocation race while the writer is acquired.
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable B+Tree materialization requires a current allocated index page pool");
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		std::vector<PageId> pageIds;
		std::vector<PageLocation> locations;
		pageIds.reserve(pageCount);
		locations.reserve(pageCount);
		for (size_t index = 0; index < pageCount; ++index) {
			const PageId pageId = batch.allocateLogicalPageId();
			if (locations_.findPageOptional(current, pageId).has_value()) { // ZYX_COV_EXCL_LINE: every Page
				// Directory publication advances the durable high-water mark before a later allocation can reuse this
				// ID.
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable logical Page ID high-water mark collides with an existing Page Directory entry");
			}
			pageIds.push_back(pageId);
			locations.push_back({pageId, pool.segment_id, 0, pool.generation, 0});
		}
		preparer(pool, pageIds, locations);

		std::set<PageId> uniquePageIds;
		std::set<uint16_t> uniqueFrames;
		for (size_t index = 0; index < locations.size(); ++index) {
			const PageLocation &location = locations[index];
			const uint8_t invalidLocation = static_cast<uint8_t>(location.page_id != pageIds[index]) |
											static_cast<uint8_t>(location.segment_id != pool.segment_id) |
											static_cast<uint8_t>(location.generation != pool.generation) |
											static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
											static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
											static_cast<uint8_t>(location.free_bytes > RSS_PAGE_SIZE);
			if (invalidLocation != 0U || !uniquePageIds.insert(location.page_id).second ||
				!uniqueFrames.insert(location.page_index).second) { // ZYX_COV_EXCL_LINE: the private typed preparer
				// derives one location and one unique frame from every allocated logical Page ID.
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable B+Tree materialization returned invalid or duplicate Page Directory locations");
			}
		}
		const IndexRootLocation root{indexId, pageIds[rootPageOrdinal], pool.generation};
		locations_.prepareUpsertPages(batch, locations);
		locations_.prepareReplaceIndexRoots(batch, std::span(&root, 1), {});
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::materializeNewIndexTreePagesAcrossPools(
			std::span<const NewIndexTreeMaterializationPoolRequest> pools, uint64_t indexId,
			IndexTreeMaterializationPageReference rootPage, LogSequenceNumber checkpointLsn,
			const IndexTreeMultiPoolMaterializationPreparer &preparer, const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(pools.empty()) | static_cast<uint8_t>(indexId == 0U) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer) |
									 static_cast<uint8_t>(rootPage.pool_ordinal >= pools.size());
		if (invalidInput != 0U) { // ZYX_COV_EXCL_LINE: BTreePageStore validates the public build plan first.
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable bulk B+Tree materialization requires pools, an index, a root, an LSN, and a preparer");
		}

		size_t totalPageCount = 0;
		for (const NewIndexTreeMaterializationPoolRequest &request: pools) {
			const uint8_t invalidPool = static_cast<uint8_t>(!request.first_slot.isValid()) |
										static_cast<uint8_t>(!request.last_slot.isValid()) |
										static_cast<uint8_t>(request.first_slot > request.last_slot) |
										static_cast<uint8_t>(request.page_count == 0U) |
										static_cast<uint8_t>(request.page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			if (invalidPool != 0U || request.page_count > std::numeric_limits<size_t>::max() - totalPageCount) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"relocatable bulk B+Tree materialization has an invalid pool range or page count");
			}
			totalPageCount += request.page_count;
		}
		if (rootPage.page_ordinal >= pools[rootPage.pool_ordinal].page_count) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable bulk B+Tree materialization root does not name one requested page");
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);

		std::vector<IndexTreeMaterializationPoolBatch> batches;
		std::vector<CatalogEntry> catalogEntries;
		batches.reserve(pools.size());
		catalogEntries.reserve(pools.size());
		for (const NewIndexTreeMaterializationPoolRequest &request: pools) {
			const std::optional<PhysicalSlotId> physicalSlot =
					allocator_.prepareReserveFirstFree(batch, request.first_slot, request.last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable bulk B+Tree materialization has no free physical slot in a requested range");
			}
			CatalogEntry pool;
			pool.segment_id = batch.allocateLogicalSegmentId();
			pool.physical_slot = *physicalSlot;
			pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
			pool.owner_id = request.owner_id;
			pool.generation = Generation(1);
			pool.state = SegmentState::SGS_SEALED;
			pool.committed_lsn = checkpointLsn;
			catalogEntries.push_back(pool);
			batches.push_back({pool, {}, {}});
		}
		catalog_.prepareUpsert(batch, catalogEntries);

		for (size_t poolOrdinal = 0; poolOrdinal < batches.size(); ++poolOrdinal) {
			IndexTreeMaterializationPoolBatch &pool = batches[poolOrdinal];
			const size_t pageCount = pools[poolOrdinal].page_count;
			pool.page_ids.reserve(pageCount);
			pool.locations.reserve(pageCount);
			for (size_t pageOrdinal = 0; pageOrdinal < pageCount; ++pageOrdinal) {
				const PageId pageId = batch.allocateLogicalPageId();
				if (locations_.findPageOptional(current, pageId).has_value()) { // ZYX_COV_EXCL_LINE: the durable
					// high-water mark makes a freshly allocated Page ID distinct from every current mapping.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable bulk B+Tree materialization allocated an already reachable Page ID");
				}
				pool.page_ids.push_back(pageId);
				pool.locations.push_back({pageId, pool.pool.segment_id, 0, pool.pool.generation, 0});
			}
		}

		preparer(batches);
		std::set<PageId> uniquePageIds;
		std::set<std::pair<uint64_t, uint16_t>> uniqueFrames;
		std::vector<PageLocation> publishedLocations;
		publishedLocations.reserve(totalPageCount);
		for (const IndexTreeMaterializationPoolBatch &pool: batches) {
			if (pool.locations.size() != pool.page_ids.size()) { // ZYX_COV_EXCL_LINE: BTreePageStore preserves each
				// metadata-assigned batch cardinality while writing its corresponding physical frames.
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable bulk B+Tree materialization preparer changed a page batch cardinality");
			}
			for (size_t pageOrdinal = 0; pageOrdinal < pool.locations.size(); ++pageOrdinal) {
				const PageLocation &location = pool.locations[pageOrdinal];
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != pool.page_ids[pageOrdinal]) |
						static_cast<uint8_t>(location.segment_id != pool.pool.segment_id) |
						static_cast<uint8_t>(location.generation != pool.pool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_SIZE);
				const std::pair<uint64_t, uint16_t> frame{location.segment_id.value(), location.page_index};
				if (invalidLocation != 0U || !uniquePageIds.insert(location.page_id).second ||
					!uniqueFrames.insert(frame).second) { // ZYX_COV_EXCL_LINE: BTreePageStore derives unique Page IDs
					// and physical frames directly from one newly reserved pool batch.
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable bulk B+Tree materialization returned invalid or duplicate page locations");
				}
				publishedLocations.push_back(location);
			}
		}

		const IndexTreeMaterializationPoolBatch &rootPool = batches[rootPage.pool_ordinal];
		const IndexRootLocation root{indexId, rootPool.page_ids[rootPage.page_ordinal], rootPool.pool.generation};
		std::ranges::sort(publishedLocations, {}, &PageLocation::page_id);
		locations_.prepareUpsertPages(batch, publishedLocations);
		locations_.prepareReplaceIndexRoots(batch, std::span(&root, 1), {});
		publishBatch(batch, writer, options);
		return batch.candidate();
	}


	Superblock RelocatableMetadataStore::mutateIndexTreePagesAcrossPools(
			std::span<const IndexTreeMutationPoolRequest> requests,
			std::optional<NewIndexTreePoolRequest> newPoolRequest, const IndexRootLocation &expectedRoot,
			std::optional<IndexTreeMutationAppendedPageReference> replacementRoot,
			std::optional<PageId> stableReplacementRoot, LogSequenceNumber checkpointLsn,
			const IndexTreeMultiPoolMutationPreparer &preparer, const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(requests.empty()) |
									 static_cast<uint8_t>(expectedRoot.index_id == 0U) |
									 static_cast<uint8_t>(!expectedRoot.root_page_id.isValid()) |
									 static_cast<uint8_t>(!expectedRoot.root_generation.isValid()) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		// ZYX_COV_EXCL_START: BTreePageStore is the sole caller and validates this immutable plan input.
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable multi-pool B+Tree mutation requires pools, a root, an LSN, and a preparer");
		}
		// ZYX_COV_EXCL_STOP

		size_t totalAppended = 0;
		std::set<SegmentId> requestedPools;
		std::set<PageId> replacementIds;
		std::set<PageId> retirementIds;
		for (const IndexTreeMutationPoolRequest &request: requests) {
			const uint8_t invalidRequest =
					static_cast<uint8_t>(!request.segment_id.isValid()) |
					static_cast<uint8_t>(request.expected_replacements.empty() && request.appended_page_count == 0U &&
											 request.expected_retirements.empty()) |
					static_cast<uint8_t>(request.replacement_segment_id.isValid() &&
											 request.expected_replacements.empty());
			// ZYX_COV_EXCL_START: the B+Tree planner groups each validated pool exactly once.
			if (invalidRequest != 0U || !requestedPools.insert(request.segment_id).second) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"relocatable multi-pool B+Tree mutation has an invalid or duplicate pool request");
			}
			// ZYX_COV_EXCL_STOP
			// ZYX_COV_EXCL_START: B+Tree plans are bounded by the shared-pool frame count.
			if (request.appended_page_count > std::numeric_limits<size_t>::max() - totalAppended) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable multi-pool B+Tree mutation page count overflows host memory");
			}
			// ZYX_COV_EXCL_STOP
			totalAppended += request.appended_page_count;
			for (const PageLocation &expected: request.expected_replacements) {
				// ZYX_COV_EXCL_START: replacement IDs come from one deduplicated pinned mutation path.
				if (!expected.page_id.isValid() || !replacementIds.insert(expected.page_id).second ||
					retirementIds.contains(expected.page_id)) {
					throw FormatException(
							FormatError::FERR_INVALID_HEADER,
							"relocatable multi-pool B+Tree mutation has an invalid or duplicate replacement Page ID");
				}
				// ZYX_COV_EXCL_STOP
			}
			for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
				// ZYX_COV_EXCL_START: retirements are emitted only from the same deduplicated mutation path.
				if (!retirement.location.page_id.isValid() || !retirement.expected_owner_lsn.isValid() ||
					!retirementIds.insert(retirement.location.page_id).second ||
					replacementIds.contains(retirement.location.page_id)) {
					throw FormatException(
							FormatError::FERR_INVALID_HEADER,
							"relocatable multi-pool B+Tree mutation has an invalid, duplicate, or replaced "
							"retirement Page ID");
				}
				// ZYX_COV_EXCL_STOP
			}
		}
		if (newPoolRequest.has_value()) {
			const uint8_t invalidNewPool =
					static_cast<uint8_t>(!newPoolRequest->first_slot.isValid()) |
					static_cast<uint8_t>(!newPoolRequest->last_slot.isValid()) |
					static_cast<uint8_t>(newPoolRequest->first_slot > newPoolRequest->last_slot) |
					static_cast<uint8_t>(newPoolRequest->appended_page_count == 0U) |
					static_cast<uint8_t>(newPoolRequest->appended_page_count > RSS_PAGE_POOL_DATA_FRAME_COUNT);
			// ZYX_COV_EXCL_START: BTreePageStore constructs growth requests after validation.
			if (invalidNewPool != 0U ||
				newPoolRequest->appended_page_count > std::numeric_limits<size_t>::max() - totalAppended) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"relocatable new index pool request requires a valid range and bounded append batch");
			}
			// ZYX_COV_EXCL_STOP
			totalAppended += newPoolRequest->appended_page_count;
		}
		const size_t batchCount = requests.size() + (newPoolRequest.has_value() ? 1U : 0U);
		const bool hasPageMutation = !replacementIds.empty() || !retirementIds.empty() || totalAppended != 0U;
		// ZYX_COV_EXCL_START: root references are derived from this private plan's append ordinals.
		if ((replacementRoot.has_value() && stableReplacementRoot.has_value()) ||
			(stableReplacementRoot.has_value() && !stableReplacementRoot->isValid()) || !hasPageMutation ||
			(replacementRoot.has_value() &&
			 (replacementRoot->pool_ordinal >= batchCount ||
			  (replacementRoot->pool_ordinal < requests.size() &&
			   replacementRoot->appended_page_ordinal >= requests[replacementRoot->pool_ordinal].appended_page_count) ||
			  (replacementRoot->pool_ordinal == requests.size() &&
			   (!newPoolRequest.has_value() ||
				replacementRoot->appended_page_ordinal >= newPoolRequest->appended_page_count))))) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable multi-pool B+Tree mutation has an invalid appended root reference");
		}
		// ZYX_COV_EXCL_STOP

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const std::optional<IndexRootLocation> visibleRoot =
				locations_.findIndexRootOptional(current, expectedRoot.index_id);
		if (!visibleRoot.has_value() || *visibleRoot != expectedRoot) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable multi-pool B+Tree mutation has a stale or unavailable Index root mapping");
		}
		const PageLocation visibleRootLocation = locations_.findPage(current, expectedRoot.root_page_id);
		const CatalogEntry visibleRootPool = catalog_.find(current, visibleRootLocation.segment_id);
		const uint8_t invalidVisibleRoot =
				static_cast<uint8_t>(visibleRootPool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
				static_cast<uint8_t>(visibleRootPool.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(visibleRootLocation.generation != expectedRoot.root_generation) |
				static_cast<uint8_t>(visibleRootPool.generation != expectedRoot.root_generation) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, visibleRootPool.physical_slot));
		if (invalidVisibleRoot != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable multi-pool B+Tree mutation root does not resolve to one current index pool");
		}

		std::vector<IndexTreeMutationPoolBatch> batches;
		batches.reserve(requests.size());
		for (const IndexTreeMutationPoolRequest &request: requests) {
			const CatalogEntry pool = catalog_.find(current, request.segment_id);
			const uint8_t invalidPool = static_cast<uint8_t>(pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
										static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalidPool != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable multi-pool B+Tree mutation requires current allocated index page pools");
			}
			CatalogEntry replacementPool = pool;
			if (request.replacement_segment_id.isValid()) {
				replacementPool = catalog_.find(current, request.replacement_segment_id);
				const uint8_t invalidReplacementPool =
						static_cast<uint8_t>(replacementPool.segment_id == pool.segment_id) |
						static_cast<uint8_t>(replacementPool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
						static_cast<uint8_t>(replacementPool.state != SegmentState::SGS_SEALED) |
						static_cast<uint8_t>(replacementPool.owner_id != pool.owner_id) |
						static_cast<uint8_t>(!allocator_.isAllocated(current, replacementPool.physical_slot));
				if (invalidReplacementPool != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable multi-pool B+Tree mutation has an unavailable cross-pool replacement destination");
				}
			}
			for (const PageLocation &expected: request.expected_replacements) {
				const PageLocation visible = locations_.findPage(current, expected.page_id);
				const uint8_t invalidReplacement =
						static_cast<uint8_t>(visible != expected) |
						static_cast<uint8_t>(expected.segment_id != pool.segment_id) |
						static_cast<uint8_t>(expected.generation != pool.generation) |
						static_cast<uint8_t>(expected.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(expected.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT);
				if (invalidReplacement != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable multi-pool B+Tree mutation has a stale, foreign, or invalid replacement page");
				}
			}
			for (const IndexTreePageRetirement &retirement: request.expected_retirements) {
				const PageLocation visible = locations_.findPage(current, retirement.location.page_id);
				const PageLocation &expected = retirement.location;
				const uint8_t invalidRetirement =
						static_cast<uint8_t>(visible != expected) |
						static_cast<uint8_t>(expected.segment_id != pool.segment_id) |
						static_cast<uint8_t>(expected.generation != pool.generation) |
						static_cast<uint8_t>(expected.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(expected.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT);
				if (invalidRetirement != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable multi-pool B+Tree mutation has a stale, foreign, or invalid retirement page");
				}
			}
			IndexTreeMutationPoolBatch poolBatch;
			poolBatch.pool = pool;
			poolBatch.expected_replacements = request.expected_replacements;
			poolBatch.replacement_locations = request.expected_replacements;
			poolBatch.expected_retirements = request.expected_retirements;
			poolBatch.replacement_pool = replacementPool;
			batches.push_back(std::move(poolBatch));
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		if (newPoolRequest.has_value()) {
			const std::optional<PhysicalSlotId> physicalSlot =
					allocator_.prepareReserveFirstFree(batch, newPoolRequest->first_slot, newPoolRequest->last_slot);
			if (!physicalSlot.has_value()) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable new index pool request has no free physical slot in its reservation range");
			}
			CatalogEntry pool;
			pool.segment_id = batch.allocateLogicalSegmentId();
			pool.physical_slot = *physicalSlot;
			pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
			pool.owner_id = newPoolRequest->owner_id;
			pool.generation = Generation(1);
			pool.state = SegmentState::SGS_SEALED;
			pool.committed_lsn = checkpointLsn;
			catalog_.prepareUpsert(batch, std::span(&pool, 1));
			IndexTreeMutationPoolBatch poolBatch;
			poolBatch.pool = pool;
			poolBatch.newly_reserved = true;
			poolBatch.replacement_pool = pool;
			batches.push_back(std::move(poolBatch));
		}
		for (size_t poolIndex = 0; poolIndex < batches.size(); ++poolIndex) {
			IndexTreeMutationPoolBatch &poolBatch = batches[poolIndex];
			const size_t appendedCount = poolIndex < requests.size() ? requests[poolIndex].appended_page_count
																	 : newPoolRequest->appended_page_count;
			poolBatch.appended_page_ids.reserve(appendedCount);
			poolBatch.appended_locations.reserve(appendedCount);
			for (size_t index = 0; index < appendedCount; ++index) {
				const PageId pageId = batch.allocateLogicalPageId();
				// ZYX_COV_EXCL_START: the metadata writer allocates fresh Page IDs for this private B+Tree plan.
				if (replacementIds.contains(pageId) || retirementIds.contains(pageId) ||
					locations_.findPageOptional(current, pageId).has_value()) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable multi-pool B+Tree mutation allocated an already reachable Page ID");
				}
				// ZYX_COV_EXCL_STOP
				poolBatch.appended_page_ids.push_back(pageId);
				poolBatch.appended_locations.push_back(
						{pageId, poolBatch.pool.segment_id, 0, poolBatch.pool.generation, 0});
			}
		}

		preparer(batches);
		std::set<PageId> publishedIds;
		std::set<std::pair<uint64_t, uint16_t>> publishedFrames;
		std::set<std::pair<uint64_t, uint16_t>> retiredFrames;
		std::vector<PageLocation> publishedLocations;
		publishedLocations.reserve(replacementIds.size() + totalAppended);
		std::vector<uint64_t> retiredPageIds;
		retiredPageIds.reserve(retirementIds.size());
		for (const IndexTreeMutationPoolBatch &poolBatch: batches) {
			const uint8_t invalidBatch =
					static_cast<uint8_t>(poolBatch.replacement_locations.size() !=
										 poolBatch.expected_replacements.size()) |
					static_cast<uint8_t>(poolBatch.appended_locations.size() != poolBatch.appended_page_ids.size());
			// ZYX_COV_EXCL_START: the private BTreePageStore preparer preserves batch cardinality.
			if (invalidBatch != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable multi-pool B+Tree preparer changed its batch cardinality");
			}
			// ZYX_COV_EXCL_STOP
			auto validatePublishedLocation = [&](const PageLocation &location, PageId expectedPageId,
																	const CatalogEntry &expectedPool) {
				const uint8_t invalidLocation =
						static_cast<uint8_t>(location.page_id != expectedPageId) |
						static_cast<uint8_t>(location.segment_id != expectedPool.segment_id) |
						static_cast<uint8_t>(location.generation != expectedPool.generation) |
						static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
						static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
						static_cast<uint8_t>(location.free_bytes > RSS_PAGE_SIZE);
				const std::pair<uint64_t, uint16_t> frame{location.segment_id.value(), location.page_index};
				// ZYX_COV_EXCL_START: BTreePageStore derives locations from Metadata-assigned unique Page IDs and
				// frames.
				if (invalidLocation != 0U || !publishedIds.insert(location.page_id).second ||
					!publishedFrames.insert(frame).second) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "relocatable multi-pool B+Tree mutation returned invalid or duplicate Page "
										  "Directory locations");
				}
				// ZYX_COV_EXCL_STOP
			};
			for (size_t index = 0; index < poolBatch.replacement_locations.size(); ++index) {
				validatePublishedLocation(poolBatch.replacement_locations[index],
										  poolBatch.expected_replacements[index].page_id,
										  poolBatch.replacement_pool.segment_id.isValid() ? poolBatch.replacement_pool : poolBatch.pool);
				publishedLocations.push_back(poolBatch.replacement_locations[index]);
			}
			for (size_t index = 0; index < poolBatch.appended_locations.size(); ++index) {
				validatePublishedLocation(poolBatch.appended_locations[index], poolBatch.appended_page_ids[index],
										  poolBatch.pool);
				publishedLocations.push_back(poolBatch.appended_locations[index]);
			}
			for (const IndexTreePageRetirement &retirement: poolBatch.expected_retirements) {
				const PageLocation &location = retirement.location;
				const std::pair<uint64_t, uint16_t> frame{location.segment_id.value(), location.page_index};
				// ZYX_COV_EXCL_START: the private plan cannot both retire and publish one logical page or frame.
				if (publishedIds.contains(location.page_id) || !retiredFrames.insert(frame).second) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable multi-pool B+Tree mutation retirement overlaps a published page or frame");
				}
				// ZYX_COV_EXCL_STOP
				retiredPageIds.push_back(location.page_id.value());
			}
		}

		IndexRootLocation publishedRoot = expectedRoot;
		if (replacementRoot.has_value()) {
			const IndexTreeMutationPoolBatch &rootBatch = batches[replacementRoot->pool_ordinal];
			publishedRoot.root_page_id = rootBatch.appended_locations[replacementRoot->appended_page_ordinal].page_id;
			publishedRoot.root_generation = rootBatch.pool.generation;
		} else if (stableReplacementRoot.has_value()) {
			// ZYX_COV_EXCL_START: erase plans never retain a root that they placed in their retirement set.
			if (retirementIds.contains(*stableReplacementRoot)) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable multi-pool B+Tree mutation cannot publish a retired stable root Page ID");
			}
			// ZYX_COV_EXCL_STOP
			const auto location = std::ranges::find(publishedLocations, *stableReplacementRoot, &PageLocation::page_id);
			const PageLocation rootLocation = location == publishedLocations.end()
													  ? locations_.findPage(current, *stableReplacementRoot)
													  : *location;
			const CatalogEntry rootPool = catalog_.find(current, rootLocation.segment_id);
			const uint8_t invalidRoot = static_cast<uint8_t>(rootPool.kind != SegmentKind::SGK_INDEX_PAGE_POOL) |
										static_cast<uint8_t>(rootPool.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(rootPool.generation != rootLocation.generation) |
										static_cast<uint8_t>(!allocator_.isAllocated(current, rootPool.physical_slot));
			if (invalidRoot != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable multi-pool B+Tree mutation stable root does not resolve to one "
									  "current index pool");
			}
			publishedRoot.root_page_id = *stableReplacementRoot;
			publishedRoot.root_generation = rootLocation.generation;
		}
		std::ranges::sort(publishedLocations, {}, &PageLocation::page_id);
		std::ranges::sort(retiredPageIds);
		if (retiredPageIds.empty()) {
			locations_.prepareUpsertPages(batch, publishedLocations);
		} else {
			locations_.prepareReplacePages(batch, publishedLocations, retiredPageIds);
		}
		locations_.prepareReplaceIndexRoots(batch, std::span(&publishedRoot, 1), {});
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::appendPageToExistingPool(const CatalogEntry &expectedPool, PageLocation page,
																  LogSequenceNumber checkpointLsn,
																  const ExistingPoolPagePreparer &preparer,
																  const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable existing-pool page append requires a data preparation callback");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry visible = catalog_.find(current, expectedPool.segment_id);
		const uint8_t invalidPool = static_cast<uint8_t>(visible.segment_id != expectedPool.segment_id) |
									static_cast<uint8_t>(visible.physical_slot != expectedPool.physical_slot) |
									static_cast<uint8_t>(visible.kind != expectedPool.kind) |
									static_cast<uint8_t>(visible.owner_id != expectedPool.owner_id) |
									static_cast<uint8_t>(visible.generation != expectedPool.generation) |
									static_cast<uint8_t>(visible.committed_lsn != expectedPool.committed_lsn) |
									static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(page.segment_id != visible.segment_id) |
									static_cast<uint8_t>(page.generation != visible.generation) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot));
		if (invalidPool != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable existing-pool page append has a stale or unavailable Catalog generation");
		}
		if (!page.page_id.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable existing-pool page append requires a valid Page ID");
		}
		if (locations_.findPageOptional(current, page.page_id).has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable existing-pool page append cannot replace an existing Page ID");
		}
		preparer(visible, page);
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareUpsertPages(batch, std::span(&page, 1));
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::appendEntityPageToExistingPool(
			const CatalogEntry &expectedPool, PageLocation page, std::span<const EntityPageLocation> nodes,
			std::span<const EntityPageLocation> edges, LogSequenceNumber checkpointLsn,
			const ExistingPoolPagePreparer &preparer, const MetadataPublishOptions &options) {
		if (!preparer || !checkpointLsn.isValid() || (nodes.empty() == edges.empty())) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Entity page append requires one record kind, a checkpoint LSN, and a preparer");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry visible = catalog_.find(current, expectedPool.segment_id);
		const uint8_t invalidPool = static_cast<uint8_t>(visible.segment_id != expectedPool.segment_id) |
									static_cast<uint8_t>(visible.physical_slot != expectedPool.physical_slot) |
									static_cast<uint8_t>(visible.kind != expectedPool.kind) |
									static_cast<uint8_t>(visible.owner_id != expectedPool.owner_id) |
									static_cast<uint8_t>(visible.generation != expectedPool.generation) |
									static_cast<uint8_t>(visible.committed_lsn != expectedPool.committed_lsn) |
									static_cast<uint8_t>(visible.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
									static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(page.segment_id != visible.segment_id) |
									static_cast<uint8_t>(page.generation != visible.generation) |
									static_cast<uint8_t>(!page.page_id.isValid()) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot));
		if (invalidPool != 0U || locations_.findPageOptional(current, page.page_id).has_value()) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page append has a stale pool or an already mapped Page ID");
		}
		const auto validateNewLocations = [this, &current, pageId = page.page_id](
												  std::span<const EntityPageLocation> locations, bool nodesMap) {
			uint64_t previous = 0U;
			for (const EntityPageLocation &location: locations) {
				const std::optional<EntityPageLocation> existing =
						nodesMap ? locations_.findNodePageLocationOptional(current, location.entity_id)
								 : locations_.findEdgePageLocationOptional(current, location.entity_id);
				const uint8_t invalid = static_cast<uint8_t>(location.entity_id == 0U) |
										static_cast<uint8_t>(location.entity_id <= previous) |
										static_cast<uint8_t>(location.page_id != pageId) |
										static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
										static_cast<uint8_t>(location.slot_generation == 0U) |
										static_cast<uint8_t>(existing.has_value());
				if (invalid != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable Entity page append has invalid, duplicate, or published Entity Map entries");
				}
				previous = location.entity_id;
			}
		};
		validateNewLocations(nodes, true);
		validateNewLocations(edges, false);
		preparer(visible, page);
		const uint8_t invalidPrepared = static_cast<uint8_t>(page.page_id == PageId{}) |
										static_cast<uint8_t>(page.segment_id != visible.segment_id) |
										static_cast<uint8_t>(page.generation != visible.generation) |
										static_cast<uint8_t>(page.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
										static_cast<uint8_t>(page.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT);
		if (invalidPrepared != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page append callback returned an invalid frame");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		for (const EntityPageLocation &location: nodes) {
			batch.observeLogicalNodeId(location.entity_id);
		}
		for (const EntityPageLocation &location: edges) {
			batch.observeLogicalEdgeId(location.entity_id);
		}
		locations_.prepareUpsertPages(batch, std::span(&page, 1));
		if (!nodes.empty()) {
			locations_.prepareReplaceNodePageLocations(batch, nodes, {});
		} else {
			locations_.prepareReplaceEdgePageLocations(batch, edges, {});
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::replacePageInExistingPool(const CatalogEntry &expectedPool,
																   const PageLocation &expectedLocation,
																   PageLocation replacement,
																   LogSequenceNumber checkpointLsn,
																   const ExistingPoolPageReplacementPreparer &preparer,
																   const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable existing-pool page replacement requires a data preparation callback");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry visible = catalog_.find(current, expectedPool.segment_id);
		const uint8_t invalidPool = static_cast<uint8_t>(visible.segment_id != expectedPool.segment_id) |
									static_cast<uint8_t>(visible.physical_slot != expectedPool.physical_slot) |
									static_cast<uint8_t>(visible.kind != expectedPool.kind) |
									static_cast<uint8_t>(visible.owner_id != expectedPool.owner_id) |
									static_cast<uint8_t>(visible.generation != expectedPool.generation) |
									static_cast<uint8_t>(visible.committed_lsn != expectedPool.committed_lsn) |
									static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot));
		if (invalidPool != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable existing-pool page replacement has a stale or unavailable Catalog generation");
		}
		const PageLocation currentLocation = locations_.findPage(current, expectedLocation.page_id);
		const uint8_t invalidLocation =
				static_cast<uint8_t>(currentLocation != expectedLocation) |
				static_cast<uint8_t>(expectedLocation.segment_id != visible.segment_id) |
				static_cast<uint8_t>(expectedLocation.generation != visible.generation) |
				static_cast<uint8_t>(expectedLocation.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(expectedLocation.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(replacement.page_id != expectedLocation.page_id) |
				static_cast<uint8_t>(replacement.segment_id != visible.segment_id) |
				static_cast<uint8_t>(replacement.generation != visible.generation) |
				static_cast<uint8_t>(replacement.page_index != 0U);
		if (invalidLocation != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable existing-pool page replacement has an invalid current or candidate location");
		}
		preparer(visible, expectedLocation, replacement);
		const uint8_t invalidReplacement =
				static_cast<uint8_t>(replacement.page_id != expectedLocation.page_id) |
				static_cast<uint8_t>(replacement.segment_id != visible.segment_id) |
				static_cast<uint8_t>(replacement.generation != visible.generation) |
				static_cast<uint8_t>(replacement.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(replacement.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(replacement.page_index == expectedLocation.page_index);
		if (invalidReplacement != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable existing-pool page replacement callback returned an invalid frame");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareUpsertPages(batch, std::span(&replacement, 1));
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::replaceEntityPageInExistingPool(
			const CatalogEntry &expectedPool, const PageLocation &expectedLocation, PageLocation replacement,
			std::span<const EntityPageLocation> nodes, std::span<const uint64_t> erasedNodeIds,
			std::span<const EntityPageLocation> edges, std::span<const uint64_t> erasedEdgeIds,
			LogSequenceNumber checkpointLsn, const ExistingPoolPageReplacementPreparer &preparer,
			const MetadataPublishOptions &options) {
		if (!preparer || !checkpointLsn.isValid() ||
			((!nodes.empty() || !erasedNodeIds.empty()) && (!edges.empty() || !erasedEdgeIds.empty()))) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Entity page replacement requires one record kind, a checkpoint LSN, and a preparer");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry visible = catalog_.find(current, expectedPool.segment_id);
		const PageLocation currentLocation = locations_.findPage(current, expectedLocation.page_id);
		const uint8_t invalidSource =
				static_cast<uint8_t>(visible.segment_id != expectedPool.segment_id) |
				static_cast<uint8_t>(visible.physical_slot != expectedPool.physical_slot) |
				static_cast<uint8_t>(visible.kind != expectedPool.kind) |
				static_cast<uint8_t>(visible.owner_id != expectedPool.owner_id) |
				static_cast<uint8_t>(visible.generation != expectedPool.generation) |
				static_cast<uint8_t>(visible.committed_lsn != expectedPool.committed_lsn) |
				static_cast<uint8_t>(visible.kind != SegmentKind::SGK_ENTITY_PAGE_POOL) |
				static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
				static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot)) |
				static_cast<uint8_t>(currentLocation != expectedLocation) |
				static_cast<uint8_t>(expectedLocation.segment_id != visible.segment_id) |
				static_cast<uint8_t>(expectedLocation.generation != visible.generation) |
				static_cast<uint8_t>(expectedLocation.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(expectedLocation.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(replacement.page_id != expectedLocation.page_id) |
				static_cast<uint8_t>(replacement.segment_id != visible.segment_id) |
				static_cast<uint8_t>(replacement.generation != visible.generation) |
				static_cast<uint8_t>(replacement.page_index != 0U);
		if (invalidSource != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page replacement has a stale pool, page, or candidate frame");
		}
		const auto validateNewLocations = [this, &current, pageId = expectedLocation.page_id](
												  std::span<const EntityPageLocation> locations, bool nodesMap) {
			uint64_t previous = 0U;
			for (const EntityPageLocation &location: locations) {
				const std::optional<EntityPageLocation> existing =
						nodesMap ? locations_.findNodePageLocationOptional(current, location.entity_id)
								 : locations_.findEdgePageLocationOptional(current, location.entity_id);
				const uint8_t invalid = static_cast<uint8_t>(location.entity_id == 0U) |
										static_cast<uint8_t>(location.entity_id <= previous) |
										static_cast<uint8_t>(location.page_id != pageId) |
										static_cast<uint8_t>(location.slot_id == std::numeric_limits<uint16_t>::max()) |
										static_cast<uint8_t>(location.slot_generation == 0U) |
										static_cast<uint8_t>(existing.has_value());
				if (invalid != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable Entity page replacement has invalid or published Entity Map entries");
				}
				previous = location.entity_id;
			}
		};
		const auto validateErasures = [this, &current, pageId = expectedLocation.page_id](
											  std::span<const uint64_t> entityIds, bool nodesMap) {
			uint64_t previous = 0U;
			for (const uint64_t entityId: entityIds) {
				const std::optional<EntityPageLocation> existing =
						nodesMap ? locations_.findNodePageLocationOptional(current, entityId)
								 : locations_.findEdgePageLocationOptional(current, entityId);
				const uint8_t invalid = static_cast<uint8_t>(entityId == 0U) |
										static_cast<uint8_t>(entityId <= previous) |
										static_cast<uint8_t>(!existing.has_value()) |
										static_cast<uint8_t>(existing.has_value() && existing->page_id != pageId);
				if (invalid != 0U) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable Entity page replacement erases an absent or foreign Entity Map entry");
				}
				previous = entityId;
			}
		};
		validateNewLocations(nodes, true);
		validateNewLocations(edges, false);
		validateErasures(erasedNodeIds, true);
		validateErasures(erasedEdgeIds, false);
		preparer(visible, expectedLocation, replacement);
		const uint8_t invalidPrepared = static_cast<uint8_t>(replacement.page_id != expectedLocation.page_id) |
										static_cast<uint8_t>(replacement.segment_id != visible.segment_id) |
										static_cast<uint8_t>(replacement.generation != visible.generation) |
										static_cast<uint8_t>(replacement.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
										static_cast<uint8_t>(replacement.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
										static_cast<uint8_t>(replacement.page_index == expectedLocation.page_index);
		if (invalidPrepared != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page replacement callback returned an invalid frame");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		for (const EntityPageLocation &location: nodes) {
			batch.observeLogicalNodeId(location.entity_id);
		}
		for (const EntityPageLocation &location: edges) {
			batch.observeLogicalEdgeId(location.entity_id);
		}
		locations_.prepareUpsertPages(batch, std::span(&replacement, 1));
		if (!nodes.empty() || !erasedNodeIds.empty()) {
			locations_.prepareReplaceNodePageLocations(batch, nodes, erasedNodeIds);
		}
		if (!edges.empty() || !erasedEdgeIds.empty()) {
			locations_.prepareReplaceEdgePageLocations(batch, edges, erasedEdgeIds);
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::mutateEntityPages(
			std::span<const EntityPageReplacementRequest> requests,
			std::span<const EntityPageAppendBatchRequest> appendRequests,
			std::span<const EntityPageNewPoolBatchRequest> newPoolRequests, LogSequenceNumber checkpointLsn,
			const EntityPageTransactionBatchPreparer &preparer, const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Entity page transaction requires a preparation callback");
		}
		const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
		const std::array<NativeGraphPropertyPageBatchRequest, 0U> noNewPropertyPools{};
		return mutateNativeGraphPages(
				requests, appendRequests, newPoolRequests, noPropertyReplacements, noNewPropertyPools, checkpointLsn,
				[&preparer](std::span<EntityPageReplacementMaterialization> replacements,
							std::span<EntityPageAppendBatchMaterialization> appends,
							std::span<EntityPageNewPoolBatchMaterialization> newPools,
							std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
							std::span<NativeGraphPropertyPageBatchMaterialization> propertyPools) {
					if (!properties.empty() || !propertyPools.empty()) {
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable Entity page transaction unexpectedly materialized Property pages");
					}
					preparer(replacements, appends, newPools);
				},
				options);
	}

	Superblock RelocatableMetadataStore::mutateNativeGraphPages(
			std::span<const EntityPageReplacementRequest> requests,
			std::span<const EntityPageAppendBatchRequest> appendRequests,
			std::span<const EntityPageNewPoolBatchRequest> newPoolRequests,
			std::span<const NativeGraphPropertyPageReplacementRequest> propertyReplacements,
			std::span<const NativeGraphPropertyPageBatchRequest> propertyPageBatchRequests,
			LogSequenceNumber checkpointLsn, const NativeGraphPageTransactionPreparer &preparer,
			const MetadataPublishOptions &options) {
		const NativeGraphDerivedStateMutation noDerivedState{};
		const NativeGraphDerivedStatePreparer noDerivedStatePreparer{};
		return mutateNativeGraphPages(requests, appendRequests, newPoolRequests, propertyReplacements,
									  propertyPageBatchRequests, noDerivedState, checkpointLsn, preparer,
									  noDerivedStatePreparer, options);
	}

	Superblock RelocatableMetadataStore::mutateNativeGraphPages(
			std::span<const EntityPageReplacementRequest> requests,
			std::span<const EntityPageAppendBatchRequest> appendRequests,
			std::span<const EntityPageNewPoolBatchRequest> newPoolRequests,
			std::span<const NativeGraphPropertyPageReplacementRequest> propertyReplacements,
			std::span<const NativeGraphPropertyPageBatchRequest> propertyPageBatchRequests,
			const NativeGraphDerivedStateMutation &derivedState, LogSequenceNumber checkpointLsn,
			const NativeGraphPageTransactionPreparer &preparer,
			const NativeGraphDerivedStatePreparer &derivedStatePreparer, const MetadataPublishOptions &options,
			NativeGraphLogicalIdReservation logicalIdReservation) {
		const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
		const NativeGraphVectorBootstrapPreparer noVectorBootstrapPreparer{};
		const NativeGraphVectorDeltaPreparer noVectorDeltaPreparer{};
		return mutateNativeGraphPagesWithLargeValues(
				requests, appendRequests, newPoolRequests, propertyReplacements, propertyPageBatchRequests,
				noLargeValues, derivedState, checkpointLsn,
				[&preparer](std::span<EntityPageReplacementMaterialization> replacements,
							std::span<EntityPageAppendBatchMaterialization> appends,
							std::span<EntityPageNewPoolBatchMaterialization> newPools,
							std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
							std::span<NativeGraphPropertyPageBatchMaterialization> newProperties,
							std::span<LargeValueMaterializationResult> largeValues) {
					if (!largeValues.empty()) {
						throw FormatException(FormatError::FERR_INVALID_STATE,
											  "native graph page transaction unexpectedly materialized LargeValues");
					}
					preparer(replacements, appends, newPools, properties, newProperties);
				},
				derivedStatePreparer, noVectorBootstrapPreparer, noVectorDeltaPreparer, {}, {}, {}, options,
				logicalIdReservation);
	}


	Superblock RelocatableMetadataStore::mutateEntityPagesInExistingPools(
			std::span<const EntityPageReplacementRequest> requests,
			std::span<const EntityPageAppendBatchRequest> appendRequests, LogSequenceNumber checkpointLsn,
			const EntityPageMutationBatchPreparer &preparer, const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable existing-pool Entity page mutation requires a preparation callback");
		}
		const std::array<EntityPageNewPoolBatchRequest, 0U> noNewPools{};
		return mutateEntityPages(
				requests, appendRequests, noNewPools, checkpointLsn,
				[&preparer](std::span<EntityPageReplacementMaterialization> replacements,
							std::span<EntityPageAppendBatchMaterialization> appends,
							std::span<EntityPageNewPoolBatchMaterialization> newPools) {
					if (!newPools.empty()) { // this wrapper always passes a zero-length new-pool request array.
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable existing-pool Entity page mutation unexpectedly allocated a new pool");
					}
					preparer(replacements, appends);
				},
				options);
	}

	Superblock RelocatableMetadataStore::replaceEntityPagesInExistingPools(
			std::span<const EntityPageReplacementRequest> requests, LogSequenceNumber checkpointLsn,
			const EntityPageReplacementBatchPreparer &preparer, const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Entity page batch replacement requires a preparation callback");
		}
		const std::array<EntityPageAppendBatchRequest, 0> noAppends{};
		return mutateEntityPagesInExistingPools(
				requests, noAppends, checkpointLsn,
				[&preparer](std::span<EntityPageReplacementMaterialization> replacements,
							std::span<EntityPageAppendBatchMaterialization> appends) {
					if (!appends.empty()) { // this replacement-only wrapper always passes a zero-length append request array.
						throw FormatException(
								FormatError::FERR_INVALID_STATE,
								"relocatable Entity page replacement unexpectedly received append materializations");
					}
					preparer(replacements);
				},
				options);
	}

} // namespace graph::storage
