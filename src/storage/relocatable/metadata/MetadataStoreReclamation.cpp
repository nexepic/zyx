#include "MetadataStoreOperationsInternal.hpp"

namespace graph::storage {

	Superblock RelocatableMetadataStore::retireEntityPageInExistingPool(
			const CatalogEntry &expectedPool, const PageLocation &expectedLocation,
			std::span<const uint64_t> erasedNodeIds, std::span<const uint64_t> erasedEdgeIds,
			LogSequenceNumber checkpointLsn, const ExistingPoolPageRetirementPreparer &preparer,
			const MetadataPublishOptions &options) {
		if (!preparer || !checkpointLsn.isValid() || (erasedNodeIds.empty() == erasedEdgeIds.empty())) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Entity page retirement requires one record kind, a checkpoint LSN, and a preparer");
		}
		const auto validateErasures = [](std::span<const uint64_t> entityIds) {
			uint64_t previous = 0U;
			for (const uint64_t entityId: entityIds) {
				if (entityId == 0U || entityId <= previous) {
					throw FormatException(FormatError::FERR_INVALID_HEADER,
										  "relocatable Entity page retirement requires canonical Entity IDs");
				}
				previous = entityId;
			}
		};
		validateErasures(erasedNodeIds);
		validateErasures(erasedEdgeIds);

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
				static_cast<uint8_t>(expectedLocation.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT);
		if (invalidSource != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Entity page retirement has a stale pool, page, or physical frame");
		}
		const auto validateCurrentMappings = [this, &current, pageId = expectedLocation.page_id](
													 std::span<const uint64_t> entityIds, bool nodesMap) {
			for (const uint64_t entityId: entityIds) {
				const std::optional<EntityPageLocation> mapping =
						nodesMap ? locations_.findNodePageLocationOptional(current, entityId)
								 : locations_.findEdgePageLocationOptional(current, entityId);
				if (!mapping.has_value() || mapping->page_id != pageId) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable Entity page retirement would erase an absent or foreign Entity Map entry");
				}
			}
		};
		validateCurrentMappings(erasedNodeIds, true);
		validateCurrentMappings(erasedEdgeIds, false);
		preparer(visible, expectedLocation);

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::array<PageLocation, 0> noPages{};
		const std::array<uint64_t, 1> erasedPageIds{{expectedLocation.page_id.value()}};
		locations_.prepareReplacePages(batch, noPages, erasedPageIds);
		if (!erasedNodeIds.empty()) {
			const std::array<EntityPageLocation, 0> noNodes{};
			locations_.prepareReplaceNodePageLocations(batch, noNodes, erasedNodeIds);
		} else {
			const std::array<EntityPageLocation, 0> noEdges{};
			locations_.prepareReplaceEdgePageLocations(batch, noEdges, erasedEdgeIds);
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::retireVectorManifestPageInExistingPool(
			const CatalogEntry &expectedPool, const PageLocation &expectedLocation,
			const VectorManifestLocation &expectedManifest, LogSequenceNumber checkpointLsn,
			const ExistingPoolPageRetirementPreparer &preparer, const MetadataPublishOptions &options) {
		const uint8_t invalidInput =
				static_cast<uint8_t>(!expectedManifest.index_id) |
				static_cast<uint8_t>(!expectedManifest.manifest_page_id.isValid()) |
				static_cast<uint8_t>(!expectedManifest.generation.isValid()) |
				static_cast<uint8_t>(expectedManifest.manifest_page_id != expectedLocation.page_id) |
				static_cast<uint8_t>(expectedManifest.generation != expectedLocation.generation) |
				static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Vector Manifest retirement requires one valid current mapping and preparer");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry visiblePool = catalog_.find(current, expectedPool.segment_id);
		const uint8_t invalidPool = static_cast<uint8_t>(visiblePool.segment_id != expectedPool.segment_id) |
									static_cast<uint8_t>(visiblePool.physical_slot != expectedPool.physical_slot) |
									static_cast<uint8_t>(visiblePool.kind != SegmentKind::SGK_VECTOR_MANIFEST_POOL) |
									static_cast<uint8_t>(visiblePool.kind != expectedPool.kind) |
									static_cast<uint8_t>(visiblePool.owner_id != expectedPool.owner_id) |
									static_cast<uint8_t>(visiblePool.generation != expectedPool.generation) |
									static_cast<uint8_t>(visiblePool.committed_lsn != expectedPool.committed_lsn) |
									static_cast<uint8_t>(visiblePool.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, visiblePool.physical_slot));
		if (invalidPool != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable Vector Manifest retirement has a stale or unavailable dedicated pool");
		}
		const PageLocation visibleLocation = locations_.findPage(current, expectedLocation.page_id);
		const std::optional<VectorManifestLocation> visibleManifest =
				locations_.findVectorManifestOptional(current, expectedManifest.index_id);
		const uint8_t invalidMapping =
				static_cast<uint8_t>(visibleLocation != expectedLocation) |
				static_cast<uint8_t>(expectedLocation.segment_id != visiblePool.segment_id) |
				static_cast<uint8_t>(expectedLocation.generation != visiblePool.generation) |
				static_cast<uint8_t>(expectedLocation.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(expectedLocation.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(!visibleManifest.has_value()) |
				static_cast<uint8_t>(visibleManifest.has_value() && *visibleManifest != expectedManifest);
		if (invalidMapping != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Manifest retirement has a stale Page or Manifest Directory mapping");
		}
		preparer(visiblePool, visibleLocation);
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::array<PageLocation, 0> noPages{};
		const std::array<uint64_t, 1> erasedPageIds{expectedLocation.page_id.value()};
		const std::array<VectorManifestLocation, 0> noManifests{};
		const std::array<uint64_t, 1> erasedIndexIds{expectedManifest.index_id};
		locations_.prepareReplacePages(batch, noPages, erasedPageIds);
		locations_.prepareReplaceVectorManifests(batch, noManifests, erasedIndexIds);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::retirePagesInExistingPools(Generation expectedMetadataGeneration,
																						std::span<const IndexRootLocation> erasedRoots,
																						std::span<const PageRetirement> retirements,
																						LogSequenceNumber checkpointLsn,
																						const ExistingPoolPagesRetirementPreparer &preparer,
																						const MetadataPublishOptions &options,
																						std::span<const uint64_t> erasedVectorIds) {
		const uint8_t invalidInput = static_cast<uint8_t>(!expectedMetadataGeneration.isValid()) |
									 static_cast<uint8_t>(retirements.empty()) |
									 static_cast<uint8_t>(!checkpointLsn.isValid()) | static_cast<uint8_t>(!preparer);
		if (invalidInput != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable page retirement requires a scan generation, nonempty plan, checkpoint "
								  "LSN, and preparer");
		}

		uint64_t previousPageId = 0U;
		for (const PageRetirement &retirement: retirements) {
			const PageLocation &location = retirement.location;
			const uint8_t invalidRetirement =
					static_cast<uint8_t>(!location.page_id.isValid()) |
					static_cast<uint8_t>(location.page_id.value() <= previousPageId) |
					static_cast<uint8_t>(!location.segment_id.isValid()) |
					static_cast<uint8_t>(!location.generation.isValid()) |
					static_cast<uint8_t>(location.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(location.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(retirement.expected_owner_id == 0U) |
					static_cast<uint8_t>(retirement.expected_owner_kind == PageOwnerKind::POK_NONE) |
					static_cast<uint8_t>(!retirement.expected_owner_lsn.isValid());
			if (invalidRetirement != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable page retirement has an invalid or non-canonical source frame");
			}
			previousPageId = location.page_id.value();
		}
		uint64_t previousRootIndexId = 0U;
		for (const IndexRootLocation &root: erasedRoots) {
			const uint8_t invalidRoot = static_cast<uint8_t>(root.index_id == 0U) |
												static_cast<uint8_t>(root.index_id <= previousRootIndexId) |
												static_cast<uint8_t>(!root.root_page_id.isValid()) |
												static_cast<uint8_t>(!root.root_generation.isValid());
			if (invalidRoot != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable page retirement has an invalid or non-canonical erased Index Root");
			}
			previousRootIndexId = root.index_id;
		}
		uint64_t previousVectorId = 0U;
		for (const uint64_t vectorId: erasedVectorIds) {
			if (vectorId == 0U || vectorId <= previousVectorId) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
									  "relocatable page retirement has non-canonical erased Vector identities");
			}
			previousVectorId = vectorId;
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (current.generation != expectedMetadataGeneration) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable page retirement scan generation is stale and must be repeated");
		}
		for (const IndexRootLocation &root: erasedRoots) {
			if (locations_.findIndexRoot(current, root.index_id) != root) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable page retirement has a stale or replaced Index Root");
			}
		}
		std::map<uint64_t, ExistingPoolPageRetirementBatch> batchesBySegment;
		for (const PageRetirement &retirement: retirements) {
			const PageLocation visibleLocation = locations_.findPage(current, retirement.location.page_id);
			const CatalogEntry pool = catalog_.find(current, retirement.location.segment_id);
			const bool largeValueData = retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_DATA;
			const bool largeValueManifest = retirement.expected_owner_kind == PageOwnerKind::POK_LARGE_VALUE_MANIFEST;
			const uint8_t invalidCurrent =
					static_cast<uint8_t>(visibleLocation != retirement.location) |
					static_cast<uint8_t>(pool.segment_id != retirement.location.segment_id) |
					static_cast<uint8_t>(pool.generation != retirement.location.generation) |
					static_cast<uint8_t>(pool.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!isSharedPagePoolSegmentKind(pool.kind)) |
					static_cast<uint8_t>(!matchesPageDirectoryOnlyPoolOwnerKind(pool.kind, retirement.expected_owner_kind)) |
					static_cast<uint8_t>(largeValueData || largeValueManifest) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, pool.physical_slot));
			if (invalidCurrent != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable page retirement has a stale, unavailable, or incompatible source frame");
			}
			auto [iterator, inserted] = batchesBySegment.try_emplace(pool.segment_id.value());
			if (inserted) {
				iterator->second.pool = pool;
			}
			iterator->second.retirements.push_back(retirement);
		}

		std::vector<ExistingPoolPageRetirementBatch> batches;
		batches.reserve(batchesBySegment.size());
		for (auto &[segmentId, batch]: batchesBySegment) {
			static_cast<void>(segmentId);
			batches.push_back(std::move(batch));
		}
		preparer(batches);

		std::vector<uint64_t> erasedPageIds;
		erasedPageIds.reserve(retirements.size());
		for (const PageRetirement &retirement: retirements) {
			erasedPageIds.push_back(retirement.location.page_id.value());
		}
		if (!erasedVectorIds.empty()) {
			const std::set<PageId> retiredPages = [&retirements] {
				std::set<PageId> result;
				for (const PageRetirement &retirement: retirements) {
					result.insert(retirement.location.page_id);
				}
				return result;
			}();
			for (const uint64_t vectorId: erasedVectorIds) {
				const VectorLocation location = locations_.findVectorLocation(current, vectorId);
				if (!retiredPages.contains(location.page_id)) {
					throw FormatException(
							FormatError::FERR_INVALID_STATE,
							"relocatable page retirement would erase a Vector Location outside its retired pages");
				}
			}
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::vector<PageLocation> noPages;
		locations_.prepareReplacePages(batch, noPages, erasedPageIds);
		std::vector<uint64_t> erasedRootIndexIds;
		erasedRootIndexIds.reserve(erasedRoots.size());
		for (const IndexRootLocation &root: erasedRoots) {
			erasedRootIndexIds.push_back(root.index_id);
		}
		if (!erasedRootIndexIds.empty()) {
			const std::array<IndexRootLocation, 0> noRoots{};
			locations_.prepareReplaceIndexRoots(batch, noRoots, erasedRootIndexIds);
		}
		if (!erasedVectorIds.empty()) {
			const std::array<VectorLocation, 0> noVectorLocations{};
			locations_.prepareReplaceVectorLocations(batch, noVectorLocations, erasedVectorIds);
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::retireVectorCodebookExtents(Generation expectedMetadataGeneration,
																	 std::span<const ExtentRetirement> retirements,
																	 LogSequenceNumber checkpointLsn,
																	 const MetadataPublishOptions &options) {
		const uint8_t invalidInput = static_cast<uint8_t>(!expectedMetadataGeneration.isValid()) |
									 static_cast<uint8_t>(retirements.empty()) |
									 static_cast<uint8_t>(!checkpointLsn.isValid());
		if (invalidInput != 0U) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Vector Codebook retirement requires a scan generation, nonempty plan, "
								  "and checkpoint LSN");
		}
		uint64_t previousExtentId = 0U;
		std::set<uint64_t> segmentIds;
		for (const ExtentRetirement &retirement: retirements) {
			const ExtentLocation &location = retirement.location;
			const uint8_t invalidRetirement =
					static_cast<uint8_t>(!location.extent_id.isValid()) |
					static_cast<uint8_t>(location.extent_id.value() <= previousExtentId) |
					static_cast<uint8_t>(!location.segment_id.isValid()) |
					static_cast<uint8_t>(!location.generation.isValid()) |
					static_cast<uint8_t>(location.first_page != RSS_PAGE_POOL_FIRST_DATA_FRAME) |
					static_cast<uint8_t>(location.page_count != RSS_PAGE_POOL_DATA_FRAME_COUNT) |
					static_cast<uint8_t>(retirement.expected_owner_id == 0U) |
					static_cast<uint8_t>(!segmentIds.insert(location.segment_id.value()).second);
			if (invalidRetirement != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_HEADER,
						"relocatable Vector Codebook retirement has an invalid or non-canonical extent plan");
			}
			previousExtentId = location.extent_id.value();
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (current.generation != expectedMetadataGeneration) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable Vector Codebook retirement scan generation is stale and must be repeated");
		}
		std::vector<CatalogEntry> retiredEntries;
		std::vector<uint64_t> erasedExtentIds;
		retiredEntries.reserve(retirements.size());
		erasedExtentIds.reserve(retirements.size());
		for (const ExtentRetirement &retirement: retirements) {
			const ExtentLocation visibleLocation = locations_.findExtent(current, retirement.location.extent_id);
			const CatalogEntry visibleSegment = catalog_.find(current, retirement.location.segment_id);
			const uint8_t invalidCurrent =
					static_cast<uint8_t>(visibleLocation != retirement.location) |
					static_cast<uint8_t>(visibleSegment.segment_id != retirement.location.segment_id) |
					static_cast<uint8_t>(visibleSegment.kind != SegmentKind::SGK_CODEBOOK_EXTENT) |
					static_cast<uint8_t>(visibleSegment.owner_id != retirement.expected_owner_id) |
					static_cast<uint8_t>(visibleSegment.generation != retirement.location.generation) |
					static_cast<uint8_t>(visibleSegment.state != SegmentState::SGS_SEALED) |
					static_cast<uint8_t>(!allocator_.isAllocated(current, visibleSegment.physical_slot));
			if (invalidCurrent != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable Vector Codebook retirement has a stale or unavailable extent mapping");
			}
			CatalogEntry retired = visibleSegment;
			retired.state = SegmentState::SGS_RETIRED;
			retiredEntries.push_back(retired);
			erasedExtentIds.push_back(retirement.location.extent_id.value());
		}
		std::ranges::sort(retiredEntries, {}, &CatalogEntry::segment_id);
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		for (const CatalogEntry &retired: retiredEntries) {
			batch.reserveDataSlot(retired.physical_slot);
		}
		catalog_.prepareUpsert(batch, retiredEntries);
		std::vector<RetireQueueEntry> queued;
		queued.reserve(retiredEntries.size());
		for (const CatalogEntry &retired: retiredEntries) {
			queued.push_back({retired.physical_slot, retired.segment_id, batch.generation(),
							  RetireQueueDisposition::RQD_RETIRED_SEGMENT});
		}
		std::ranges::sort(queued, {}, &RetireQueueEntry::physical_slot);
		retirement_.prepareQueueInsertions(batch, queued);
		const std::array<ExtentLocation, 0> noExtents{};
		locations_.prepareReplaceExtents(batch, noExtents, erasedExtentIds);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::evacuatePageAcrossPools(
			const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
			const CatalogEntry &expectedDestinationPool, PageLocation destination, LogSequenceNumber checkpointLsn,
			const CrossPoolPageEvacuationPreparer &preparer, const MetadataPublishOptions &options) {
		return evacuatePageAcrossPoolsImpl(expectedSourcePool, expectedSourceLocation, expectedDestinationPool,
										   destination, {}, {}, {}, {}, {}, {}, checkpointLsn, preparer, options);
	}

	Superblock RelocatableMetadataStore::evacuateIndexRootPageAcrossPools(
			const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
			const CatalogEntry &expectedDestinationPool, PageLocation destination,
			const IndexRootLocation &expectedRoot, const IndexRootLocation &replacementRoot,
			LogSequenceNumber checkpointLsn, const CrossPoolPageEvacuationPreparer &preparer,
			const MetadataPublishOptions &options) {
		return evacuatePageAcrossPoolsImpl(expectedSourcePool, expectedSourceLocation, expectedDestinationPool,
										   destination, expectedRoot, replacementRoot, {}, {}, {}, {}, checkpointLsn,
										   preparer, options);
	}

	Superblock RelocatableMetadataStore::evacuateVectorManifestPageAcrossPools(
			const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
			const CatalogEntry &expectedDestinationPool, PageLocation destination,
			const VectorManifestLocation &expectedManifest, const VectorManifestLocation &replacementManifest,
			LogSequenceNumber checkpointLsn, const CrossPoolPageEvacuationPreparer &preparer,
			const MetadataPublishOptions &options) {
		return evacuatePageAcrossPoolsImpl(expectedSourcePool, expectedSourceLocation, expectedDestinationPool,
										   destination, {}, {}, expectedManifest, replacementManifest, {}, {},
										   checkpointLsn, preparer, options);
	}

	Superblock RelocatableMetadataStore::evacuateLargeValueManifestPageAcrossPools(
			const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
			const CatalogEntry &expectedDestinationPool, PageLocation destination,
			const LargeValueManifestLocation &expectedManifest, const LargeValueManifestLocation &replacementManifest,
			LogSequenceNumber checkpointLsn, const CrossPoolPageEvacuationPreparer &preparer,
			const MetadataPublishOptions &options) {
		return evacuatePageAcrossPoolsImpl(expectedSourcePool, expectedSourceLocation, expectedDestinationPool,
										   destination, {}, {}, {}, {}, expectedManifest, replacementManifest,
										   checkpointLsn, preparer, options);
	}

	Superblock RelocatableMetadataStore::evacuatePageAcrossPoolsImpl(
			const CatalogEntry &expectedSourcePool, const PageLocation &expectedSourceLocation,
			const CatalogEntry &expectedDestinationPool, PageLocation destination,
			const std::optional<IndexRootLocation> &expectedRoot,
			const std::optional<IndexRootLocation> &replacementRoot,
			const std::optional<VectorManifestLocation> &expectedManifest,
			const std::optional<VectorManifestLocation> &replacementManifest,
			const std::optional<LargeValueManifestLocation> &expectedLargeValueManifest,
			const std::optional<LargeValueManifestLocation> &replacementLargeValueManifest,
			LogSequenceNumber checkpointLsn, const CrossPoolPageEvacuationPreparer &preparer,
			const MetadataPublishOptions &options) {
		// ZYX_COV_EXCL_START: the typed wrappers pass either no rebind or one complete directory pair,
		// or one complete Manifest pair; this private implementation is never exposed with half-pairs.
		const bool rootRebind = expectedRoot.has_value() || replacementRoot.has_value();
		const bool manifestRebind = expectedManifest.has_value() || replacementManifest.has_value();
		const bool largeValueManifestRebind =
				expectedLargeValueManifest.has_value() || replacementLargeValueManifest.has_value();
		if (rootRebind && (!expectedRoot.has_value() || !replacementRoot.has_value())) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable B+Tree root evacuation requires both current and replacement root mappings");
		}
		if (manifestRebind && (!expectedManifest.has_value() || !replacementManifest.has_value())) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Vector Manifest evacuation requires both current and replacement directory mappings");
		}
		if (largeValueManifestRebind &&
			(!expectedLargeValueManifest.has_value() || !replacementLargeValueManifest.has_value())) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable LargeValue Manifest evacuation requires both current and replacement "
								  "directory mappings");
		}
		const uint8_t rebindCount = static_cast<uint8_t>(rootRebind) + static_cast<uint8_t>(manifestRebind) +
									static_cast<uint8_t>(largeValueManifestRebind);
		if (rebindCount > 1U) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable cross-pool evacuation cannot rebind multiple typed directories together");
		}
		// ZYX_COV_EXCL_STOP
		if (!preparer || !expectedSourcePool.segment_id.isValid() || !expectedDestinationPool.segment_id.isValid() ||
			expectedSourcePool.segment_id == expectedDestinationPool.segment_id) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable cross-pool page evacuation requires distinct valid pools and a callback");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const CatalogEntry source = catalog_.find(current, expectedSourcePool.segment_id);
		const CatalogEntry target = catalog_.find(current, expectedDestinationPool.segment_id);
		const auto isExpectedSealedPool = [this, &current](const CatalogEntry &visible, const CatalogEntry &expected) {
			const uint8_t invalid = static_cast<uint8_t>(visible.segment_id != expected.segment_id) |
									static_cast<uint8_t>(visible.physical_slot != expected.physical_slot) |
									static_cast<uint8_t>(visible.kind != expected.kind) |
									static_cast<uint8_t>(visible.owner_id != expected.owner_id) |
									static_cast<uint8_t>(visible.generation != expected.generation) |
									static_cast<uint8_t>(visible.committed_lsn != expected.committed_lsn) |
									static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot));
			return invalid == 0U;
		};
		if (!isExpectedSealedPool(source, expectedSourcePool) ||
			!isExpectedSealedPool(target, expectedDestinationPool) || source.physical_slot == target.physical_slot ||
			source.kind != target.kind) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable cross-pool page evacuation has a stale, unavailable, or incompatible pool");
		}
		const PageLocation currentLocation = locations_.findPage(current, expectedSourceLocation.page_id);
		const uint8_t invalidSource =
				static_cast<uint8_t>(currentLocation != expectedSourceLocation) |
				static_cast<uint8_t>(expectedSourceLocation.segment_id != source.segment_id) |
				static_cast<uint8_t>(expectedSourceLocation.generation != source.generation) |
				static_cast<uint8_t>(expectedSourceLocation.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(expectedSourceLocation.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT) |
				static_cast<uint8_t>(destination.page_id != expectedSourceLocation.page_id) |
				static_cast<uint8_t>(destination.segment_id != target.segment_id) |
				static_cast<uint8_t>(destination.generation != target.generation) |
				static_cast<uint8_t>(destination.page_index != 0U);
		if (invalidSource != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable cross-pool page evacuation has an invalid source or destination location");
		}
		if (rootRebind) {
			const std::optional<IndexRootLocation> currentRoot =
					locations_.findIndexRootOptional(current, expectedRoot->index_id);
			const uint8_t invalidRoot =
					static_cast<uint8_t>(expectedRoot->index_id == 0U) |
					static_cast<uint8_t>(expectedRoot->root_page_id != expectedSourceLocation.page_id) |
					static_cast<uint8_t>(expectedRoot->root_generation != source.generation) |
					static_cast<uint8_t>(replacementRoot->index_id != expectedRoot->index_id) |
					static_cast<uint8_t>(replacementRoot->root_page_id != expectedSourceLocation.page_id) |
					static_cast<uint8_t>(replacementRoot->root_generation != target.generation) |
					static_cast<uint8_t>(!currentRoot.has_value()) |
					static_cast<uint8_t>(currentRoot.has_value() && *currentRoot != *expectedRoot);
			if (invalidRoot != 0U) {
				throw FormatException(
						FormatError::FERR_INVALID_STATE,
						"relocatable B+Tree root evacuation has a stale or inconsistent Index Root mapping");
			}
		}
		if (manifestRebind) {
			const std::optional<VectorManifestLocation> currentManifest =
					locations_.findVectorManifestOptional(current, expectedManifest->index_id);
			const uint8_t invalidManifest =
					static_cast<uint8_t>(expectedManifest->index_id == 0U) |
					static_cast<uint8_t>(expectedManifest->manifest_page_id != expectedSourceLocation.page_id) |
					static_cast<uint8_t>(expectedManifest->generation != source.generation) |
					static_cast<uint8_t>(replacementManifest->index_id != expectedManifest->index_id) |
					static_cast<uint8_t>(replacementManifest->manifest_page_id != expectedSourceLocation.page_id) |
					static_cast<uint8_t>(replacementManifest->generation != target.generation) |
					static_cast<uint8_t>(!currentManifest.has_value()) |
					static_cast<uint8_t>(currentManifest.has_value() && *currentManifest != *expectedManifest);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Vector Manifest evacuation has a stale or inconsistent Manifest "
									  "Directory mapping");
			}
		}
		if (largeValueManifestRebind) {
			const std::optional<LargeValueManifestLocation> currentManifest =
					locations_.findLargeValueManifestOptional(current, expectedLargeValueManifest->value_id);
			const uint8_t invalidManifest =
					static_cast<uint8_t>(!expectedLargeValueManifest->value_id.isValid()) |
					static_cast<uint8_t>(expectedLargeValueManifest->manifest_page_id !=
										 expectedSourceLocation.page_id) |
					static_cast<uint8_t>(expectedLargeValueManifest->generation != source.generation) |
					static_cast<uint8_t>(replacementLargeValueManifest->value_id !=
										 expectedLargeValueManifest->value_id) |
					static_cast<uint8_t>(replacementLargeValueManifest->manifest_page_id !=
										 expectedSourceLocation.page_id) |
					static_cast<uint8_t>(replacementLargeValueManifest->generation != target.generation) |
					static_cast<uint8_t>(!currentManifest.has_value()) |
					static_cast<uint8_t>(currentManifest.has_value() &&
										 *currentManifest != *expectedLargeValueManifest);
			if (invalidManifest != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable LargeValue Manifest evacuation has a stale or inconsistent Manifest "
									  "Directory mapping");
			}
		}
		preparer(source, expectedSourceLocation, target, destination);
		const uint8_t invalidDestination =
				static_cast<uint8_t>(destination.page_id != expectedSourceLocation.page_id) |
				static_cast<uint8_t>(destination.segment_id != target.segment_id) |
				static_cast<uint8_t>(destination.generation != target.generation) |
				static_cast<uint8_t>(destination.page_index < RSS_PAGE_POOL_FIRST_DATA_FRAME) |
				static_cast<uint8_t>(destination.page_index > RSS_PAGE_POOL_DATA_FRAME_COUNT);
		if (invalidDestination != 0U) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable cross-pool page evacuation callback returned an invalid destination frame");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		locations_.prepareUpsertPages(batch, std::span(&destination, 1));
		if (rootRebind) {
			locations_.prepareReplaceIndexRoots(batch, std::span(&*replacementRoot, 1), {});
		}
		if (manifestRebind) {
			locations_.prepareReplaceVectorManifests(batch, std::span(&*replacementManifest, 1), {});
		}
		if (largeValueManifestRebind) {
			locations_.prepareReplaceLargeValueManifests(batch, std::span(&*replacementLargeValueManifest, 1), {});
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	void RelocatableMetadataStore::reconcileExistingPool(SegmentId segmentId,
														 const ExistingPoolReconciler &reconciler) {
		if (!segmentId.isValid() || !reconciler) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable pool reconciliation requires a valid Segment ID and callback");
		}
		MetadataWriteGuard writer = beginWrite(true);
		const Superblock current = bootstrap_.recover();
        MetadataPageReadContext::Scope recoveryReads(snapshots_.createWriterReadContext(writer, current), *io_);
		const CatalogEntry visible = catalog_.find(current, segmentId);
        const bool nativeActive = RSS_HYBRID_PROPERTY_FORMAT_ENABLED && current.hybrid_property_descriptor_root.isValid() &&
            std::ranges::any_of(pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current).descriptor.roots,
                [](const auto &root) { return root.page_id.isValid(); });
        // Complete scan writes retain the ordinary immutable Entity/Index
        // COW planners. Capacity reclamation consults both the current page
        // directory and every active pin before reusing a frame. These pools
        // contain no native field pages, which require their separate joined
        // reconciliation path.
        const bool scanCowPool = RSS_SCAN_PATH_FORMAT_ENABLED &&
            (visible.kind == SegmentKind::SGK_ENTITY_PAGE_POOL || visible.kind == SegmentKind::SGK_INDEX_PAGE_POOL);
        if (nativeActive && !isLargeValueMaintenancePool(visible) && !scanCowPool)
            throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                "native physical reconciliation requires an immutable LargeValue or content-index pool");
		const uint8_t unavailable = static_cast<uint8_t>(visible.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, visible.physical_slot));
		if (unavailable != 0U) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable pool reconciliation requires one allocated sealed Catalog generation");
		}
		const auto findPage = [this, &current](PageId pageId) -> std::optional<PageLocation> {
			return locations_.findPageOptional(current, pageId);
		};
		const auto findCatalog = [this, &current](SegmentId targetSegmentId) {
			const CatalogEntry target = catalog_.find(current, targetSegmentId);
			const uint8_t unavailable = static_cast<uint8_t>(target.state != SegmentState::SGS_SEALED) |
										static_cast<uint8_t>(!allocator_.isAllocated(current, target.physical_slot));
			if (unavailable != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable remote pool reconciliation target is not current and allocated");
			}
			return target;
		};
		const ExistingPoolFrameVisibility isFrameVisibleToActiveSnapshot =
				[this, &writer](PageId pageId, SegmentId segmentId, uint16_t pageIndex, Generation generation) {
					return snapshots_.anyActiveSnapshot(writer, [this, pageId, segmentId, pageIndex,
																 generation](const Superblock &snapshot) {
						const std::optional<PageLocation> location = locations_.findPageOptional(snapshot, pageId);
						return location.has_value() && location->segment_id == segmentId &&
							   location->page_index == pageIndex && location->generation == generation;
					});
				};
		reconciler(visible, findPage, findCatalog, isFrameVisibleToActiveSnapshot);
	}

	void RelocatableMetadataStore::reconcileCurrentPools(const CurrentPoolReconciler &reconciler) {
		if (!reconciler) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable current-pool reconciliation requires a callback");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
        MetadataPageReadContext::Scope recoveryReads(snapshots_.createWriterReadContext(writer, current), *io_);
		std::map<uint64_t, std::vector<PageLocation>> pagesBySegment;
		locations_.visitPages(current, [this, &current, &pagesBySegment](const PageLocation &location) {
			const CatalogEntry target = catalog_.find(current, location.segment_id);
			const bool isSharedPool = isSharedPagePoolSegmentKind(target.kind);
			const uint8_t invalidTarget = static_cast<uint8_t>(target.state != SegmentState::SGS_SEALED) |
										  static_cast<uint8_t>(target.generation != location.generation) |
										  static_cast<uint8_t>(!isSharedPool) |
										  static_cast<uint8_t>(!allocator_.isAllocated(current, target.physical_slot));
			if (invalidTarget != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable Page Directory entry does not target a current allocated pool");
			}
			pagesBySegment[location.segment_id.value()].push_back(location);
		});
		catalog_.visit(current, [this, &current, &pagesBySegment, &reconciler](const CatalogEntry &visible) {
			if (visible.state != SegmentState::SGS_SEALED) {
				return;
			}
			if (!allocator_.isAllocated(current, visible.physical_slot)) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable sealed metadata pool has no free-space ownership");
			}
			const auto pages = pagesBySegment.find(visible.segment_id.value());
			if (pages == pagesBySegment.end()) {
				reconciler(visible, {});
				return;
			}
			reconciler(visible, pages->second);
		});
	}

	Superblock RelocatableMetadataStore::upsertLocationsImpl(std::span<const PageLocation> pages,
															 std::span<const ExtentLocation> extents,
															 LogSequenceNumber checkpointLsn,
															 const LocationPublicationPreparer *preparer,
															 const MetadataPublishOptions &options) {
		if (pages.empty() && extents.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable location publication requires a Page or Extent mutation");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		if (!pages.empty()) {
			locations_.prepareUpsertPages(batch, pages);
		}
		if (!extents.empty()) {
			locations_.prepareUpsertExtents(batch, extents);
		}
		if (preparer != nullptr) {
			(*preparer)();
		}
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::setPhysicalSlotAllocated(PhysicalSlotId slot, bool allocated,
																  LogSequenceNumber checkpointLsn,
																  const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		if (!allocated && metadata_page_pool_.isMetadataPoolSlot(slot)) {
			throw FormatException(
					FormatError::FERR_INVALID_STATE,
					"relocatable metadata page pool ownership cannot be released through free-space mutation");
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		if (allocated) {
			batch.reserveDataSlot(slot);
		}
		allocator_.prepareSetAllocated(batch, slot, allocated);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	std::optional<AllocatorReservation>
	RelocatableMetadataStore::reserveFirstFreeSlot(PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
												   LogSequenceNumber checkpointLsn,
												   const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::optional<PhysicalSlotId> physicalSlot =
				allocator_.prepareReserveFirstFree(batch, firstSlot, lastSlot);
		if (!physicalSlot.has_value()) {
			return std::nullopt;
		}
		publishBatch(batch, writer, options);
		return AllocatorReservation{batch.candidate(), *physicalSlot};
	}

	std::optional<CatalogSlotReservations> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalog(
			std::span<const CatalogSlotReservationRequest> requests, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		return reserveFirstFreeAndUpsertCatalogImpl(requests, {}, {}, {}, {}, {}, firstSlot, lastSlot, checkpointLsn,
													nullptr, nullptr, {}, options);
	}

	std::optional<CatalogSlotReservations> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalog(
			std::span<const CatalogSlotReservationRequest> requests, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const CatalogSlotReservationPreparer &preparer,
			const MetadataPublishOptions &options) {
		if (!preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable prepared Catalog reservation requires a data preparation callback");
		}
		return reserveFirstFreeAndUpsertCatalogImpl(requests, {}, {}, {}, {}, {}, firstSlot, lastSlot, checkpointLsn,
													&preparer, nullptr, {}, options);
	}

	std::optional<CatalogSlotReservations> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalogAndLocations(
			std::span<const CatalogSlotReservationRequest> requests, std::span<const PageLocation> pages,
			std::span<const ExtentLocation> extents, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const CatalogSlotReservationPreparer &preparer,
			const MetadataPublishOptions &options) {
		if (pages.empty() && extents.empty()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable combined Catalog and location reservation requires a location mutation");
		}
		if (!preparer) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable combined Catalog and location reservation requires a data preparation callback");
		}
		return reserveFirstFreeAndUpsertCatalogImpl(requests, pages, extents, {}, {}, {}, firstSlot, lastSlot,
													checkpointLsn, &preparer, nullptr, {}, options);
	}

	std::optional<CatalogSlotReservations> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalogAndVectorManifests(
			std::span<const CatalogSlotReservationRequest> requests, std::span<const PageLocation> pages,
			std::span<const VectorManifestLocation> manifests, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const CatalogSlotReservationPreparer &preparer,
			const MetadataPublishOptions &options) {
		if (pages.empty() || manifests.empty() || !preparer) {
			throw FormatException(
					FormatError::FERR_INVALID_HEADER,
					"relocatable Vector Manifest reservation needs pages, manifests, and a data preparer");
		}
		return reserveFirstFreeAndUpsertCatalogImpl(requests, pages, {}, {}, {}, manifests, firstSlot, lastSlot,
													checkpointLsn, &preparer, nullptr, {}, options);
	}

	std::optional<CatalogSlotReservations>
	RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalogAndEntityPageLocations(
			std::span<const CatalogSlotReservationRequest> requests, std::span<PageLocation> pages,
			std::span<const EntityPageLocation> nodes, std::span<const EntityPageLocation> edges,
			PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
			const EntityPagePoolReservationPreparer &preparer, const MetadataPublishOptions &options) {
		if (pages.empty() || (nodes.empty() == edges.empty()) || !preparer) {
			throw FormatException(FormatError::FERR_INVALID_HEADER, "relocatable Entity page reservation needs pages, "
																	"one record kind, and a data preparation callback");
		}
		return reserveFirstFreeAndUpsertCatalogImpl(requests, pages, {}, nodes, edges, {}, firstSlot, lastSlot,
													checkpointLsn, nullptr, &preparer, pages, options);
	}

	std::optional<CatalogSlotReservations> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalogImpl(
			std::span<const CatalogSlotReservationRequest> requests, std::span<const PageLocation> pages,
			std::span<const ExtentLocation> extents, std::span<const EntityPageLocation> entityPageNodes,
			std::span<const EntityPageLocation> entityPageEdges, std::span<const VectorManifestLocation> manifests,
			PhysicalSlotId firstSlot, PhysicalSlotId lastSlot, LogSequenceNumber checkpointLsn,
			const CatalogSlotReservationPreparer *preparer, const EntityPagePoolReservationPreparer *entityPagePreparer,
			std::span<PageLocation> mutableEntityPages, const MetadataPublishOptions &options) {
        // Only fresh homogeneous private scratch can use this physical path
        // with native groups. No public page/extent/entity/Vector route is added.
        const bool scratch = !requests.empty() && requests.size() <= 64U && preparer && !entityPagePreparer &&
            pages.empty() && extents.empty() && entityPageNodes.empty() && entityPageEdges.empty() &&
            manifests.empty() && mutableEntityPages.empty() &&
            (requests.front().kind == SegmentKind::SGK_QUERY_SCRATCH || requests.front().kind == SegmentKind::SGK_BULK_SCRATCH) &&
            std::ranges::all_of(requests, [&](const auto &entry) {
                return entry.kind == requests.front().kind && entry.owner_id && entry.owner_id == requests.front().owner_id &&
                    entry.segment_generation == Generation(1) && entry.state == SegmentState::SGS_SEALED && entry.require_absent;
            });
		MetadataWriteGuard writer = beginWrite(scratch);
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
        if (scratch && current.hybrid_property_descriptor_root.isValid()) {
            pages_.prepareHybridPropertyDescriptorRelocation(batch, current);
            batch.hybrid_property_publication_ = true;
        }
		const CatalogReservationPublicationRequest request{
				requests,  pages,	 extents,  entityPageNodes,	   entityPageEdges,	  manifests,
				firstSlot, lastSlot, preparer, entityPagePreparer, mutableEntityPages};
		std::optional<std::vector<CatalogEntry>> entries = reservation_coordinator_.prepare(batch, request);
		if (!entries.has_value()) {
			return std::nullopt;
		}
		publishBatch(batch, writer, options);
		return CatalogSlotReservations{batch.candidate(), std::move(*entries)};
	}

	std::optional<CatalogSlotReservation> RelocatableMetadataStore::reserveFirstFreeAndUpsertCatalog(
			CatalogSlotReservationRequest request, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const MetadataPublishOptions &options) {
		const std::optional<CatalogSlotReservations> reservations =
				reserveFirstFreeAndUpsertCatalog(std::span(&request, 1), firstSlot, lastSlot, checkpointLsn, options);
		if (!reservations.has_value()) {
			return std::nullopt;
		}
		return CatalogSlotReservation{reservations->superblock, reservations->entries.front()};
	}

	Superblock RelocatableMetadataStore::retireCatalogSegment(SegmentId segmentId, Generation expectedGeneration,
															  LogSequenceNumber checkpointLsn,
															  const MetadataPublishOptions &options) {
		const std::array requests{CatalogSegmentRetirementRequest{segmentId, expectedGeneration}};
		return retireCatalogSegments(requests, checkpointLsn, options);
	}

	Superblock
	RelocatableMetadataStore::retireCatalogSegments(std::span<const CatalogSegmentRetirementRequest> requests,
													LogSequenceNumber checkpointLsn,
													const MetadataPublishOptions &options) {
		if (requests.empty() || !checkpointLsn.isValid()) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable Catalog retirement batch requires segments and a checkpoint LSN");
		}
		MetadataWriteGuard writer = beginWrite(true);
		const Superblock current = bootstrap_.recover();
		const bool nativeActive = RSS_HYBRID_PROPERTY_FORMAT_ENABLED && current.hybrid_property_descriptor_root.isValid() &&
            std::ranges::any_of(pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current).descriptor.roots,
                [](const auto &root) { return root.page_id.isValid(); });
        if (nativeActive) {
            if (requests.size() > 64U)
                throw FormatException(FormatError::FERR_INVALID_HEADER, "native immutable pool retirement exceeds its bound");
            std::set<uint64_t> scratch;
            for (const auto &request : requests) {
                const auto entry = catalog_.find(current, request.segment_id);
                if (entry.kind == SegmentKind::SGK_BULK_SCRATCH) scratch.insert(entry.segment_id.value());
                else if (!isLargeValueMaintenancePool(entry))
                    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                        "native retirement requires an immutable maintenance pool or unreachable bulk scratch");
            }
            if (!scratch.empty()) {
                locations_.visitPages(current, [&](const PageLocation &page) {
                    if (scratch.contains(page.segment_id.value())) throw FormatException(FormatError::FERR_INVALID_STATE,
                        "native bulk scratch retirement found a public Page route");
                });
                locations_.visitExtents(current, [&](const ExtentLocation &extent) {
                    if (scratch.contains(extent.segment_id.value())) throw FormatException(FormatError::FERR_INVALID_STATE,
                        "native bulk scratch retirement found a public Extent route");
                });
            }
        }
        MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
        if (nativeActive) batch.hybrid_property_publication_ = true;
		segment_retirement_coordinator_.prepareRetirements(batch, requests);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	Superblock RelocatableMetadataStore::releaseQueryScratchSegments(
		std::span<const CatalogSegmentRetirementRequest> requests, uint64_t owner,
		LogSequenceNumber checkpoint, const MetadataPublishOptions &options) {
		if(requests.empty() || requests.size()>64U || owner==0U || !checkpoint.isValid())
			throw FormatException(FormatError::FERR_INVALID_HEADER,"query scratch release requires bounded owner identities");
		// The authenticated query-kind release below cannot change graph/native fields.
		MetadataWriteGuard writer=beginWrite(true);const Superblock current=bootstrap_.recover();
		std::vector<CatalogEntry> released;released.reserve(requests.size());std::set<uint64_t> ids;
		for(const auto &request:requests){
			const auto entry=catalog_.find(current,request.segment_id);
			if(!ids.insert(entry.segment_id.value()).second || entry.kind!=SegmentKind::SGK_QUERY_SCRATCH ||
				entry.owner_id!=owner || entry.generation!=request.expected_generation || entry.state!=SegmentState::SGS_SEALED ||
				!allocator_.isAllocated(current,entry.physical_slot))
				throw FormatException(FormatError::FERR_INVALID_STATE,"query scratch release rejects stale, foreign or user segments");
			auto obsolete=entry;obsolete.state=SegmentState::SGS_OBSOLETE;released.push_back(obsolete);
		}
		// A corrupted registration must not turn public data into disposable scratch.
		locations_.visitPages(current,[&](const PageLocation &page){if(ids.contains(page.segment_id.value()))
			throw FormatException(FormatError::FERR_INVALID_STATE,"query scratch is reachable from a public Page Directory");});
		locations_.visitExtents(current,[&](const ExtentLocation &extent){if(ids.contains(extent.segment_id.value()))
			throw FormatException(FormatError::FERR_INVALID_STATE,"query scratch is reachable from a public Extent Directory");});
		MetadataWriteBatch batch(metadata_page_pool_,current,checkpoint);
        if (current.hybrid_property_descriptor_root.isValid()) {
            pages_.prepareHybridPropertyDescriptorRelocation(batch, current);
            batch.hybrid_property_publication_ = true;
        }
		std::ranges::sort(released,{},&CatalogEntry::segment_id);catalog_.prepareUpsert(batch,released);
		for(const auto &entry:released){batch.reserveDataSlot(entry.physical_slot);allocator_.prepareSetAllocated(batch,entry.physical_slot,false);}
		publishBatch(batch,writer,options);return batch.candidate();
	}

	Superblock RelocatableMetadataStore::releaseRetiredCatalogSegment(SegmentId segmentId,
																	  Generation expectedGeneration,
																	  LogSequenceNumber checkpointLsn,
																	  const MetadataPublishOptions &options) {
        auto result = tryReleaseRetiredCatalogSegment(segmentId, expectedGeneration, checkpointLsn, options);
        if (!result) throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable retired slot release is blocked by a snapshot that can resolve the retired slot");
        return *result;
    }

	std::optional<Superblock> RelocatableMetadataStore::tryReleaseRetiredCatalogSegment(SegmentId segmentId,
																	  Generation expectedGeneration,
																	  LogSequenceNumber checkpointLsn,
																	  const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const RetireQueueEntry retirement =
				segment_retirement_coordinator_.validateRetiredRelease(current, segmentId, expectedGeneration);
		if (retirement_.isVisibleToActiveSnapshot(writer, retirement)) {
            return std::nullopt;
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		segment_retirement_coordinator_.prepareRetiredRelease(batch, retirement);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	std::optional<Superblock> RelocatableMetadataStore::tryReleaseRetiredCatalogSegments(
			std::span<const CatalogSegmentRetirementRequest> requests, LogSequenceNumber checkpointLsn,
			const MetadataPublishOptions &options) {
		if (requests.empty() || !checkpointLsn.isValid())
			throw FormatException(FormatError::FERR_INVALID_HEADER,
				"relocatable retired slot release requires a nonempty batch and checkpoint LSN");
		MetadataWriteGuard writer = beginWrite(true);
		const Superblock current = bootstrap_.recover();
        const bool nativeActive = RSS_HYBRID_PROPERTY_FORMAT_ENABLED && current.hybrid_property_descriptor_root.isValid() &&
            std::ranges::any_of(pages_.readHybridPropertyDescriptor(current.hybrid_property_descriptor_root, current).descriptor.roots,
                [](const auto &root) { return root.page_id.isValid(); });
        if (nativeActive && requests.size() > 64U)
            throw FormatException(FormatError::FERR_INVALID_HEADER, "native retired-pool release exceeds its 64-pool bound");
		utils::OwnedVector<uint64_t> identities;
		identities.reserve(requests.size());
		for (const auto &request : requests) {
			if (!request.segment_id.isValid() || !request.expected_generation.isValid())
				throw FormatException(FormatError::FERR_INVALID_HEADER,
					"relocatable retired slot release has an invalid segment identity");
			identities.push_back(request.segment_id.value());
		}
		std::ranges::sort(identities);
		if (std::adjacent_find(identities.begin(), identities.end()) != identities.end())
			throw FormatException(FormatError::FERR_INVALID_HEADER,
				"relocatable retired slot release has a duplicate segment");
		utils::OwnedVector<RetireQueueEntry> retirements;
		retirements.reserve(requests.size());
		bool blocked = false;
		for (const auto &request : requests) {
			const auto retirement = segment_retirement_coordinator_.validateRetiredRelease(
				current, request.segment_id, request.expected_generation);
			blocked = retirement_.isVisibleToActiveSnapshot(writer, retirement) || blocked;
			retirements.push_back(retirement);
		}
		if (blocked) return std::nullopt;
        if (nativeActive) {
            // Whole-pool release never reconciles or overwrites field frames.
            // The durable retire queue and pin barrier establish eligibility;
            // probe every frame owner and reject a current route, even if its allocation bit is clear.
            const auto source = snapshots_.pinCurrentUnderWrite(writer, current);
            SharedPagePoolAllocator sourcePools(io_, *this, source);
            std::set<uint64_t> scratch;
            for (const auto &entry : retirements)
                if (catalog_.find(current, entry.segment_id).kind == SegmentKind::SGK_BULK_SCRATCH)
                    scratch.insert(entry.segment_id.value());
            if (!scratch.empty()) {
                locations_.visitPages(current, [&](const PageLocation &page) {
                    if (scratch.contains(page.segment_id.value())) throw FormatException(FormatError::FERR_INVALID_STATE,
                        "native retired bulk scratch retains a public Page route");
                });
                locations_.visitExtents(current, [&](const ExtentLocation &extent) {
                    if (scratch.contains(extent.segment_id.value())) throw FormatException(FormatError::FERR_INVALID_STATE,
                        "native retired bulk scratch retains a public Extent route");
                });
            }
            for (const auto &entry : retirements) {
                const auto pool = catalog_.find(current, entry.segment_id);
                if (pool.kind == SegmentKind::SGK_BULK_SCRATCH) continue;
                if (pool.kind != SegmentKind::SGK_PROPERTY_PAGE_POOL && pool.kind != SegmentKind::SGK_INDEX_PAGE_POOL && pool.kind != SegmentKind::SGK_ENTITY_PAGE_POOL && pool.kind != SegmentKind::SGK_STATE_PAGE_POOL && !isLargeValueMaintenancePool(pool))
                    throw FormatException(FormatError::FERR_UNSUPPORTED_VERSION,
                        "native retired-pool release requires a supported detached shared pool");
                const auto header = SharedPagePoolAllocator::readPoolHeader(*io_, current.data_region_offset, pool);
                for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
                    const auto owner = hybrid_property_detail::currentPoolFrameOwner(pool, header, frame,
                        [&](uint8_t index) { return sourcePools.readPageOwner(pool, index); },
                        [&](PageId id) { return locations_.findPageOptional(current, id); });
                    if (owner)
                        throw FormatException(FormatError::FERR_INVALID_STATE,
                            "native retired-pool release still has a current page route");
                }
            }
        }
		std::ranges::sort(retirements, {}, &RetireQueueEntry::physical_slot);
        // A reused slot must not retain one permanent Catalog owner per release.
        // Only older, already-obsolete shared-pool owners of these exact slots
        // are eligible. Two old owners per released slot bound this transaction
        // and gradually drain history created by earlier engine versions.
        if (requests.size() > std::numeric_limits<size_t>::max() / 2U)
            throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
                "relocatable Catalog history cleanup bound overflow");
        const size_t maximumHistory = requests.size() * 2U;
        utils::OwnedVector<SegmentId> obsoleteOwners;
        obsoleteOwners.reserve(maximumHistory);
        if (!nativeActive) catalog_.visit(current, [&](const CatalogEntry &entry) {
            if (obsoleteOwners.size() == maximumHistory || entry.state != SegmentState::SGS_OBSOLETE ||
                !isSharedPagePoolSegmentKind(entry.kind)) return;
            const auto selected = std::lower_bound(retirements.begin(), retirements.end(), entry.physical_slot,
                [](const RetireQueueEntry &retirement, PhysicalSlotId slot) { return retirement.physical_slot < slot; });
            if (selected != retirements.end() && selected->physical_slot == entry.physical_slot)
                obsoleteOwners.push_back(entry.segment_id);
        });
        if (!obsoleteOwners.empty()) {
            std::ranges::sort(obsoleteOwners);
            locations_.visitPages(current, [&](const PageLocation &location) {
                if (std::binary_search(obsoleteOwners.begin(), obsoleteOwners.end(), location.segment_id))
                    throw FormatException(FormatError::FERR_INVALID_STATE,
                        "relocatable obsolete Catalog history still has a current Page Directory reference");
            });
        }
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		retire_queue_reclaimer_.prepareReclamation(batch, retirements);
        if (nativeActive) {
            // Exact retired identities have no older reachable pins or current
            // data routes. Avoid accumulating an obsolete owner per reuse cycle.
            utils::OwnedVector<SegmentId> erased;
            erased.reserve(retirements.size());
            for (const auto &entry : retirements) erased.push_back(entry.segment_id);
            std::ranges::sort(erased); catalog_.prepareErase(batch, erased);
            batch.hybrid_property_publication_ = true;
        }
        if (!obsoleteOwners.empty()) catalog_.prepareErase(batch, obsoleteOwners);
		publishBatch(batch, writer, options);
		return batch.candidate();
	}

	std::optional<CatalogSlotRelocation> RelocatableMetadataStore::relocateCatalogSegment(
			SegmentId segmentId, Generation expectedGeneration, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const CatalogSlotRelocationPreparer &preparer,
			const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::optional<CatalogSlotRelocationPlan> plan = segment_relocation_coordinator_.prepareSegmentRelocation(
				batch, segmentId, expectedGeneration, firstSlot, lastSlot, preparer);
		if (!plan.has_value()) {
			return std::nullopt;
		}
		publishBatch(batch, writer, options);
		return CatalogSlotRelocation{batch.candidate(), plan->source, plan->destination};
	}

	std::optional<CatalogSlotRelocation> RelocatableMetadataStore::relocateCatalogExtentSegment(
			const ExtentLocation &expectedExtent, PhysicalSlotId firstSlot, PhysicalSlotId lastSlot,
			LogSequenceNumber checkpointLsn, const CatalogSlotRelocationPreparer &preparer,
			const MetadataPublishOptions &options) {
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const std::optional<CatalogSlotRelocationPlan> plan = segment_relocation_coordinator_.prepareExtentRelocation(
				batch, expectedExtent, firstSlot, lastSlot, preparer);
		if (!plan.has_value()) {
			return std::nullopt;
		}
		publishBatch(batch, writer, options);
		return CatalogSlotRelocation{batch.candidate(), plan->source, plan->destination};
	}

	Superblock RelocatableMetadataStore::releaseRelocatedCatalogSource(SegmentId segmentId,
																	   Generation expectedGeneration,
																	   LogSequenceNumber checkpointLsn,
																	   const MetadataPublishOptions &options) {
        auto result = tryReleaseRelocatedCatalogSource(segmentId, expectedGeneration, checkpointLsn, options);
        if (!result) throw FormatException(FormatError::FERR_INVALID_STATE, "relocatable relocated source release is blocked by a snapshot that can resolve the source");
        return *result;
    }

	std::optional<Superblock> RelocatableMetadataStore::tryReleaseRelocatedCatalogSource(SegmentId segmentId,
																	   Generation expectedGeneration,
																	   LogSequenceNumber checkpointLsn,
																	   const MetadataPublishOptions &options) {
        std::optional<Superblock> privateSource;
        if constexpr (RSS_SCAN_PATH_FORMAT_ENABLED) {
            const auto observed = bootstrap_.recover();
            if (observed.flags & RSS_SUPERBLOCK_FLAG_SCAN_BUILDING) privateSource = observed;
        }
        MetadataWriteGuard writer = beginWrite(privateSource.has_value());
        const Superblock current = bootstrap_.recover();
        if (privateSource && current != *privateSource)
            throw FormatException(FormatError::FERR_INVALID_STATE, "private relocated-source release lost its BUILDING root fence");
		const RetireQueueEntry retirement =
				segment_relocation_coordinator_.validateRelocatedSourceRelease(current, segmentId, expectedGeneration);
		if (retirement_.isVisibleToActiveSnapshot(writer, retirement)) {
            return std::nullopt;
		}
		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
        segment_relocation_coordinator_.prepareRelocatedSourceRelease(batch, retirement);
        if (privateSource) batch.hybrid_property_publication_ = true;
        publishBatch(batch, writer, options);
		return batch.candidate();
	}

	void RelocatableMetadataStore::collectMetadataGarbage() {
		MetadataWriteGuard writer = beginWrite();
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, bootstrap_.recover());
	}

	MetadataPagePoolReclamation
	RelocatableMetadataStore::reclaimEmptyMetadataPagePools(LogSequenceNumber checkpointLsn,
															const MetadataPublishOptions &options) {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot reclaim metadata page pools");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		// This reconciles page-frame bitmaps against current and pinned roots while
		// the writer barrier prevents a new snapshot from observing an unprotected
		// frame. A selected pool therefore has no metadata frame reachable from any
		// generation that can still read this process.
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, current);

		utils::OwnedVector<CatalogEntry> candidates;
		catalog_.visit(current, [this, &candidates, &current](const CatalogEntry &entry) {
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
			if (metadata_page_pool_.isMetadataPoolEmpty(entry.physical_slot)) {
				candidates.push_back(entry);
			}
		});
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

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
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
	}

	MetadataPagePoolEvacuation
	RelocatableMetadataStore::evacuateSparseMetadataPagePool(LogSequenceNumber checkpointLsn, uint32_t maxLiveFrames,
															 const MetadataPublishOptions &options) {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot evacuate a metadata page pool");
		}
		if (!checkpointLsn.isValid() || maxLiveFrames == 0U || maxLiveFrames > RSS_PAGE_POOL_DATA_FRAME_COUNT) {
			throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "relocatable metadata-pool evacuation requires a valid sparse-pool threshold");
		}

		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, current);
		if (!current.catalog_root.isValid()) {
			// The bootstrap-only new-file state has no durable pool ownership entry.
			// The first normal publication establishes it before evacuation is allowed.
			return MetadataPagePoolEvacuation{current, {}, 0U};
		}

		std::optional<CatalogEntry> source;
		std::optional<MetadataPagePoolHeader> sourceHeader;
		uint32_t sourceLiveFrames = 0U;
		std::optional<CatalogEntry> pageLocalSource;
		std::optional<MetadataPagePoolHeader> pageLocalSourceHeader;
		uint32_t pageLocalSourceLiveFrames = 0U;
		std::optional<CatalogEntry> mixedRoutedSource;
		std::optional<MetadataPagePoolHeader> mixedRoutedSourceHeader;
		uint32_t mixedRoutedSourceLiveFrames = 0U;
		std::optional<CatalogEntry> routeBranchSource;
		std::optional<MetadataPagePoolHeader> routeBranchSourceHeader;
		uint32_t routeBranchSourceLiveFrames = 0U;
		std::optional<CatalogEntry> bootstrapHeaderSource;
		std::optional<MetadataPagePoolHeader> bootstrapHeaderSourceHeader;
		uint32_t bootstrapHeaderSourceLiveFrames = 0U;
		catalog_.visit(current, [this, &current, maxLiveFrames, &source, &sourceHeader, &sourceLiveFrames,
								 &pageLocalSource, &pageLocalSourceHeader, &pageLocalSourceLiveFrames,
								 &mixedRoutedSource, &mixedRoutedSourceHeader, &mixedRoutedSourceLiveFrames,
								 &routeBranchSource, &routeBranchSourceHeader, &routeBranchSourceLiveFrames,
								 &bootstrapHeaderSource, &bootstrapHeaderSourceHeader,
								 &bootstrapHeaderSourceLiveFrames](const CatalogEntry &entry) {
			if (entry.kind != SegmentKind::SGK_METADATA_PAGE_POOL || entry.state == SegmentState::SGS_OBSOLETE) {
				return;
			}
			const uint8_t invalid = static_cast<uint8_t>(entry.state != SegmentState::SGS_SEALED) |
									static_cast<uint8_t>(entry.owner_id != entry.physical_slot.value()) |
									static_cast<uint8_t>(!allocator_.isAllocated(current, entry.physical_slot)) |
									static_cast<uint8_t>(!metadata_page_pool_.isMetadataPoolSlot(entry.physical_slot));
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata-pool evacuation found invalid durable ownership");
			}
			const MetadataPagePoolHeader header = metadata_page_pool_.poolHeader(entry.physical_slot);
			if (header.segment_id != entry.segment_id || header.generation != entry.generation ||
				header.committed_lsn != entry.committed_lsn) {
				throw FormatException(FormatError::FERR_INVALID_STATE,
									  "relocatable metadata-pool evacuation found a Catalog/header identity mismatch");
			}
			const uint32_t liveFrames = std::popcount(header.allocated_frames);
			if (liveFrames == 0U || liveFrames > maxLiveFrames) {
				return;
			}
			const auto shouldReplace = [&entry, liveFrames](const std::optional<CatalogEntry> &selected,
															uint32_t selectedFrames) {
				return !selected.has_value() || liveFrames < selectedFrames ||
					   (liveFrames == selectedFrames && entry.physical_slot < selected->physical_slot);
			};
			if (shouldReplace(source, sourceLiveFrames)) {
				source = entry;
				sourceHeader = header;
				sourceLiveFrames = liveFrames;
			}
			if (canRelocateRoutedMetadataPoolPages(pages_, current, entry.physical_slot, header.allocated_frames) &&
				shouldReplace(pageLocalSource, pageLocalSourceLiveFrames)) {
				pageLocalSource = entry;
				pageLocalSourceHeader = header;
				pageLocalSourceLiveFrames = liveFrames;
			}
			if (canEvacuateMixedRoutedMetadataPool(pages_, current, entry.physical_slot, header.allocated_frames) &&
				shouldReplace(mixedRoutedSource, mixedRoutedSourceLiveFrames)) {
				mixedRoutedSource = entry;
				mixedRoutedSourceHeader = header;
				mixedRoutedSourceLiveFrames = liveFrames;
			}
			if (canRefreshMetadataRouteBranchForEvacuation(pages_, current, entry.physical_slot,
														   header.allocated_frames) &&
				shouldReplace(routeBranchSource, routeBranchSourceLiveFrames)) {
				routeBranchSource = entry;
				routeBranchSourceHeader = header;
				routeBranchSourceLiveFrames = liveFrames;
			}
			if (canRefreshBootstrapHeaderForEvacuation(pages_, current, entry.physical_slot, header.allocated_frames) &&
				shouldReplace(bootstrapHeaderSource, bootstrapHeaderSourceLiveFrames)) {
				bootstrapHeaderSource = entry;
				bootstrapHeaderSourceHeader = header;
				bootstrapHeaderSourceLiveFrames = liveFrames;
			}
		});
		if (pageLocalSource.has_value()) {
			source = pageLocalSource;
			sourceHeader = pageLocalSourceHeader;
			sourceLiveFrames = pageLocalSourceLiveFrames;
		} else if (mixedRoutedSource.has_value()) {
			source = mixedRoutedSource;
			sourceHeader = mixedRoutedSourceHeader;
			sourceLiveFrames = mixedRoutedSourceLiveFrames;
		} else if (routeBranchSource.has_value()) {
			source = routeBranchSource;
			sourceHeader = routeBranchSourceHeader;
			sourceLiveFrames = routeBranchSourceLiveFrames;
		} else if (bootstrapHeaderSource.has_value()) {
			source = bootstrapHeaderSource;
			sourceHeader = bootstrapHeaderSourceHeader;
			sourceLiveFrames = bootstrapHeaderSourceLiveFrames;
		}
		if (!source.has_value() || !sourceHeader.has_value()) {
			return MetadataPagePoolEvacuation{current, {}, 0U};
		}
		// A snapshot-pinned reader can retain either stable PageId targets or an
		// old physical route branch in its pinned bootstrap header. The legacy
		// whole-directory clone still relies on mixed physical roots, so keep it blocked.
		const bool hasActiveSnapshots = snapshots_.activeSnapshotCount() != 0U;
		if (hasActiveSnapshots && !pageLocalSource.has_value() && !mixedRoutedSource.has_value() &&
			!routeBranchSource.has_value() && !bootstrapHeaderSource.has_value()) {
			return MetadataPagePoolEvacuation{current, {}, 0U};
		}
		if (hasActiveSnapshots) {
			metadata_page_pool_.protectPoolUntilSnapshotsDrain(source->physical_slot);
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		// Exclude the source before the first COW allocation. In particular the
		// Catalog or Allocator root rewrite must never refill the pool it evicts.
		batch.reserveDataSlot(source->physical_slot);
		const std::optional<uint32_t> pageLocalRelocation = relocateRoutedMetadataPoolPages(
				pages_, batch, current, source->physical_slot, sourceHeader->allocated_frames);
		const std::optional<uint32_t> mixedRoutedRelocation =
				pageLocalRelocation.has_value()
						? std::nullopt
						: evacuateMixedRoutedMetadataPool(pages_, batch, current, source->physical_slot,
														  sourceHeader->allocated_frames);
		const std::optional<uint32_t> routeBranchRefresh =
				pageLocalRelocation.has_value() || mixedRoutedRelocation.has_value()
						? std::nullopt
						: refreshMetadataRouteBranchForEvacuation(pages_, batch, current, source->physical_slot,
																  sourceHeader->allocated_frames);
		const std::optional<uint32_t> bootstrapHeaderRefresh =
				pageLocalRelocation.has_value() || mixedRoutedRelocation.has_value() || routeBranchRefresh.has_value()
						? std::nullopt
						: refreshBootstrapHeaderForEvacuation(pages_, batch, current, source->physical_slot,
															  sourceHeader->allocated_frames);
		if (!pageLocalRelocation.has_value() && !mixedRoutedRelocation.has_value() && !routeBranchRefresh.has_value() &&
			!bootstrapHeaderRefresh.has_value()) {
			// Pre-bootstrap roots and any directory with an unpublishable route
			// identity still use the conservative clone.
			cloneMetadataDirectoryRootsForEvacuation(pages_, batch, current);
		}
		pool_ownership_.preparePublication(batch);
		catalog_.prepareLogicalRootChildren(batch);
		allocator_.prepareLogicalRootChildren(batch);
		locations_.prepareLogicalRootChildren(batch);
		prepareBootstrapRoutes(batch);
		try {
			batch.publish(bootstrap_, options.bootstrap_failpoint);
        synchronized_source_ = batch.candidate();
		} catch (...) {
			// A final durable fence can report an uncertain outcome after its complete
			// candidate Superblock is readable. Reclaim only after this independent
			// recovery check confirms that the new root became current.
			if (bootstrap_.recover() == batch.candidate()) {
				snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
			}
			throw;
		}
		// The evacuation clone can preserve v5 logical children. Bind their
		// resolver to the published route branch before any subsequent read.
		pages_.rebindSuperblock(batch.candidate());
		// Only the durable new root makes the source frames unreachable. This
		// reconciliation is the publication-safe transition to an empty pool;
		// the next reclaimer transaction removes its Catalog ownership and bit.
		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		const bool sourceEmpty = metadata_page_pool_.isMetadataPoolEmpty(source->physical_slot);
		if (!hasActiveSnapshots && !sourceEmpty) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata-pool evacuation left reachable frames in its source pool");
		}
		if (hasActiveSnapshots && sourceEmpty) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata-pool evacuation reclaimed frames visible to a snapshot");
		}
		return MetadataPagePoolEvacuation{
				batch.candidate(), source->physical_slot,
				pageLocalRelocation.value_or(mixedRoutedRelocation.value_or(
						routeBranchRefresh.value_or(bootstrapHeaderRefresh.value_or(sourceLiveFrames))))};
	}

	MetadataEmergencyReserveRotation
	RelocatableMetadataStore::rotateMetadataEmergencyReserve(LogSequenceNumber checkpointLsn,
															 const MetadataPublishOptions &options) {
		if (read_only_) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable read-only metadata store cannot rotate the emergency reserve");
		}
		MetadataWriteGuard writer = beginWrite();
		const Superblock current = bootstrap_.recover();
		const MetadataEmergencyReserveDescriptor previous = current.metadata_emergency_reserve;
		if (!previous.isValid() || metadata_page_pool_.emergencyReserve() != previous) {
			throw FormatException(FormatError::FERR_INVALID_STATE,
								  "relocatable metadata reserve descriptor disagrees with the open pool inventory");
		}
		if (!metadata_page_pool_.emergencyReserveHasAllocatedFrames()) {
			return MetadataEmergencyReserveRotation{current, previous, previous, false};
		}

		MetadataWriteBatch batch(metadata_page_pool_, current, checkpointLsn);
		const SegmentId successorToken = batch.allocateLogicalSegmentId();
		MetadataEmergencyReserveDescriptor successor;
		try {
			const uint64_t fileSize = metadata_page_pool_.physicalFileSize();
			const uint8_t invalidTail =
					static_cast<uint8_t>(fileSize < current.data_region_offset) |
					static_cast<uint8_t>((fileSize - current.data_region_offset) % RSS_SEGMENT_SIZE != 0U) |
					static_cast<uint8_t>(fileSize - current.data_region_offset >
										 (std::numeric_limits<uint64_t>::max() - 1U) / RSS_SEGMENT_SIZE);
			if (invalidTail != 0U) {
				throw FormatException(FormatError::FERR_ARITHMETIC_OVERFLOW,
									  "relocatable metadata emergency reserve has an invalid physical tail");
			}
			const uint64_t tailSlotValue = (fileSize - current.data_region_offset) / RSS_SEGMENT_SIZE;
			PhysicalSlotId successorSlot(tailSlotValue + 1U);
			if (tailSlotValue != 0U) {
				const PhysicalSlotId physicalTail(tailSlotValue);
				// A root-free final slot can only be an already-reclaimed tail or an
				// interrupted unpublished candidate. Reusing it avoids making a
				// permanent single-file hole while never overwriting reachable data.
				if (allocator_.findFirstFree(current, physicalTail, physicalTail).has_value()) {
					successorSlot = physicalTail;
				}
			}
			successor = metadata_page_pool_.rotateEmergencyReserve(successorToken, batch.generation(), checkpointLsn,
																   successorSlot);
			batch.trackUnpublishedPool(successor.physical_slot);
			batch.setMetadataEmergencyReserve(successor);
			// The successor has no Catalog entry by design, but its physical slot is
			// already part of the candidate allocator root before publication.
			allocator_.prepareSetAllocated(batch, successor.physical_slot, true);
			// The former reserve retains its permanent token and is now added as a
			// normal Catalog-owned pool in this same COW generation.
			pool_ownership_.preparePublication(batch);
			prepareBootstrapRoutes(batch);
			batch.publish(bootstrap_, options.bootstrap_failpoint);
        synchronized_source_ = batch.candidate();
		} catch (const BootstrapPublishInterrupted &) {
			if (bootstrap_.recover() != batch.candidate()) {
				metadata_page_pool_.restoreEmergencyReserve(previous, successor);
			} else {
				snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
			}
			throw;
		} catch (...) {
			metadata_page_pool_.restoreEmergencyReserve(previous, successor);
			throw;
		}

		snapshots_.reclaimAfterPublication(writer, metadata_page_pool_, batch.candidate());
		return MetadataEmergencyReserveRotation{batch.candidate(), previous, successor, true};
	}

} // namespace graph::storage
