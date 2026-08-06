/**
 * @file test_RelocatableMetadataStore.cpp
 * @brief End-to-end serialized-operation tests for the relocatable metadata facade.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <algorithm>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"

TEST_F(RelocatableMetadataStoreTest, SerializesCatalogPublicationSnapshotsReclamationAndReopen) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const Superblock generationTwo = store->upsertCatalog(std::vector{entry(100, 2, 2)}, LogSequenceNumber(2));
	ASSERT_EQ(store->pagePoolStats().allocated_pages, 4U);

	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(store->activeSnapshotCount(), 1U);
	const Superblock generationThree = store->upsertCatalog(std::vector{entry(101, 3, 3)}, LogSequenceNumber(3));
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 6U);
	EXPECT_EQ(store->findCatalog(snapshot, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(100));
	MetadataSnapshotPin currentSnapshot = store->openSnapshot();
	EXPECT_EQ(currentSnapshot.superblock().generation, generationThree.generation);
	EXPECT_EQ(store->findCatalog(currentSnapshot, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(101));

	snapshot.release();
	currentSnapshot.release();
	store->collectMetadataGarbage();
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 4U);
	EXPECT_EQ(store->activeSnapshotCount(), 0U);

	auto reopened = RelocatableMetadataStore::open(io);
	EXPECT_EQ(reopened->pagePoolStats().allocated_pages, 4U);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(reopenedSnapshot, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(101));
	EXPECT_EQ(generationTwo.generation, Generation(2));
}

TEST_F(RelocatableMetadataStoreTest, ReopenFollowsPublishedRootsInsteadOfScanningAnUnreachableTailSlot) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const Superblock published = store->upsertCatalog(std::vector{entry(100, 2, 2)}, LogSequenceNumber(2));
	std::array<std::byte, RSS_SEGMENT_SIZE> unreachable{};
	unreachable.front() = std::byte{0xD3U};
	const uint64_t unreachableTailOffset = RSS_FILE_BOOTSTRAP_BYTES + 2U * RSS_SEGMENT_SIZE;
	EXPECT_EQ(io->append(unreachable.data(), unreachable.size()), unreachableTailOffset);
	io->sync();
	store.reset();

	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	auto nativeIo = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
	bool readUnreachableTail = false;
	{
		StorageIO::ScopedNativeReadHookForTesting noUnreachableTailScan(
				[&readUnreachableTail](file_handle_t fd, void *bytes, size_t byteCount, pwrite_off_t offset) {
					const uint64_t readOffset = static_cast<uint64_t>(offset);
					const uint64_t tailEnd = unreachableTailOffset + RSS_SEGMENT_SIZE;
					if (readOffset < tailEnd &&
						(readOffset >= unreachableTailOffset || byteCount > unreachableTailOffset - readOffset)) {
						readUnreachableTail = true;
					}
					return portable_pread(fd, bytes, byteCount, static_cast<int64_t>(offset));
				});
		auto reopened = RelocatableMetadataStore::open(nativeIo);
		const MetadataSnapshotPin snapshot = reopened->openSnapshot();
		EXPECT_EQ(snapshot.superblock(), published);
		EXPECT_EQ(reopened->findCatalog(snapshot, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
				  PhysicalSlotId(100));
	}
	EXPECT_FALSE(readUnreachableTail);
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableMetadataStoreTest, SerializesAllocatorReservationAndReusesReleasedSnapshotPages) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const std::optional<AllocatorReservation> first =
			store->reserveFirstFreeSlot(PhysicalSlotId(40), PhysicalSlotId(41), LogSequenceNumber(2));
	ASSERT_TRUE(first.has_value());
	EXPECT_EQ(first->physical_slot, PhysicalSlotId(40));
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 4U);

	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock generationThree = store->setPhysicalSlotAllocated(PhysicalSlotId(41), true, LogSequenceNumber(3));
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 6U);
	EXPECT_EQ(snapshot.superblock().allocator_root, first->superblock.allocator_root);
	EXPECT_TRUE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(41)));
	MetadataSnapshotPin currentSnapshot = store->openSnapshot();
	EXPECT_TRUE(store->isPhysicalSlotAllocated(currentSnapshot, PhysicalSlotId(40)));
	EXPECT_TRUE(store->isPhysicalSlotAllocated(currentSnapshot, PhysicalSlotId(41)));
	EXPECT_TRUE(generationThree.allocator_root.isValid());

	snapshot.release();
	currentSnapshot.release();
	store->collectMetadataGarbage();
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 4U);
	EXPECT_FALSE(store->reserveFirstFreeSlot(PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(4)).has_value());
}

TEST_F(RelocatableMetadataStoreTest, AllocatesLogicalNodeAndEdgeIdsDurablyAndIndependently) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	EXPECT_EQ(store->allocateLogicalNodeId(LogSequenceNumber(2)), 1U);
	EXPECT_EQ(store->allocateLogicalEdgeId(LogSequenceNumber(3)), 1U);
	EXPECT_EQ(store->allocateLogicalNodeId(LogSequenceNumber(4)), 2U);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().next_logical_node_id, 3U);
	EXPECT_EQ(snapshot.superblock().next_logical_edge_id, 2U);
	snapshot.release();

	auto reopened = RelocatableMetadataStore::open(io);
	EXPECT_EQ(reopened->allocateLogicalEdgeId(LogSequenceNumber(5)), 2U);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedSnapshot.superblock().next_logical_node_id, 3U);
	EXPECT_EQ(reopenedSnapshot.superblock().next_logical_edge_id, 3U);
}

TEST_F(RelocatableMetadataStoreTest, ReservesDerivedPageAndSegmentIdentityBatchesDurably) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	EXPECT_EQ(store->allocateLogicalPageIds(3U, LogSequenceNumber(2)),
			  (std::vector<PageId>{PageId(1U), PageId(2U), PageId(3U)}));
	EXPECT_EQ(store->allocateLogicalSegmentIds(2U, LogSequenceNumber(3)),
			  (std::vector<SegmentId>{SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
									  SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U)}));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().next_logical_page_id, 4U);
	EXPECT_EQ(snapshot.superblock().next_logical_segment_id, RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 2U);
	snapshot.release();

	expectFormatError([&] { static_cast<void>(store->allocateLogicalPageIds(0U, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store->allocateLogicalSegmentIds(0U, LogSequenceNumber(4))); },
					  FormatError::FERR_INVALID_HEADER);

	auto reopened = RelocatableMetadataStore::open(io);
	EXPECT_EQ(reopened->allocateLogicalPageIds(2U, LogSequenceNumber(4)),
			  (std::vector<PageId>{PageId(4U), PageId(5U)}));
	EXPECT_EQ(reopened->allocateLogicalSegmentIds(1U, LogSequenceNumber(5)),
			  (std::vector<SegmentId>{SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 2U)}));
}

TEST_F(RelocatableMetadataStoreTest, AllocatesOnlineVectorAndTokenIdsDurablyAndIndependently) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	EXPECT_EQ(store->allocateLogicalVectorId(LogSequenceNumber(2)), 1U);
	EXPECT_EQ(store->allocatePropertyKeyToken(LogSequenceNumber(3)), 1U);
	EXPECT_EQ(store->allocateSchemaTokenId(LogSequenceNumber(4)), 1U);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().next_logical_vector_id, 2U);
	EXPECT_EQ(snapshot.superblock().next_property_key_token, 2U);
	EXPECT_EQ(snapshot.superblock().next_schema_token_id, 2U);
	snapshot.release();

	auto reopened = RelocatableMetadataStore::open(io);
	EXPECT_EQ(reopened->allocateLogicalVectorId(LogSequenceNumber(5)), 2U);
	EXPECT_EQ(reopened->allocatePropertyKeyToken(LogSequenceNumber(6)), 2U);
	EXPECT_EQ(reopened->allocateSchemaTokenId(LogSequenceNumber(7)), 2U);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedSnapshot.superblock().next_logical_vector_id, 3U);
	EXPECT_EQ(reopenedSnapshot.superblock().next_property_key_token, 3U);
	EXPECT_EQ(reopenedSnapshot.superblock().next_schema_token_id, 3U);
}

TEST_F(RelocatableMetadataStoreTest, EntityPropertyPoolPublicationIsAtomicAcrossCatalogAndDirectories) {
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 71U};
	const EntityPropertyPagePoolPreparer preparer = [](const CatalogEntry &pool, PageId pageId, PageLocation &page,
													   EntityPropertyLocation &property) {
		page = {pageId, pool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, pool.generation, 0U};
		property = {property.key, pageId, 0U, 1U};
	};

	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	EXPECT_THROW(
			static_cast<void>(store->materializeEntityPropertyPagePool(
					PhysicalSlotId(701U).value(), key, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2),
					preparer, withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
			BootstrapPublishInterrupted);
	MetadataSnapshotPin before = store->openSnapshot();
	EXPECT_EQ(before.superblock().generation, Generation(1));
	EXPECT_FALSE(store->findPageLocationOptional(before, PageId(1)).has_value());
	EXPECT_FALSE(store->findEntityPropertyLocationOptional(before, key).has_value());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(before, PhysicalSlotId(40)));
	before.release();

	store.reset();
	auto interrupted = RelocatableMetadataStore::open(io);
	EXPECT_THROW(
			static_cast<void>(interrupted->materializeEntityPropertyPagePool(
					PhysicalSlotId(701U).value(), key, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2),
					preparer, withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
			BootstrapPublishInterrupted);
	interrupted.reset();

	auto recovered = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin after = recovered->openSnapshot();
	EXPECT_EQ(after.superblock().generation, Generation(2));
	EXPECT_EQ(recovered->findCatalog(after, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(40));
	EXPECT_EQ(recovered->findPageLocation(after, PageId(1)),
			  (PageLocation{PageId(1), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), RSS_PAGE_POOL_FIRST_DATA_FRAME,
							Generation(1), 0U}));
	EXPECT_EQ(recovered->findEntityPropertyLocation(after, key), (EntityPropertyLocation{key, PageId(1), 0U, 1U}));
	EXPECT_TRUE(recovered->isPhysicalSlotAllocated(after, PhysicalSlotId(40)));
	after.release();
}

TEST_F(RelocatableMetadataStoreTest, ExistingPoolAppendRejectsMissingCallbacksStalePoolsAndInvalidPageIds) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40), true, LogSequenceNumber(2)));
	CatalogEntry pool = entry(40, 1, 3);
	pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	static_cast<void>(store->upsertCatalog(std::span(&pool, 1), LogSequenceNumber(3)));
	MetadataSnapshotPin current = store->openSnapshot();
	pool = store->findCatalog(current, SegmentId(1));
	current.release();
	const PageLocation page{PageId(1), SegmentId(1), 1, Generation(1)};
	expectFormatError(
			[&] {
				static_cast<void>(
						store->appendPageToExistingPool(pool, page, LogSequenceNumber(4), ExistingPoolPagePreparer{}));
			},
			FormatError::FERR_INVALID_HEADER);
	CatalogEntry stale = pool;
	stale.generation = Generation(2);
	expectFormatError(
			[&] {
				static_cast<void>(store->appendPageToExistingPool(stale, page, LogSequenceNumber(4),
																  [](const CatalogEntry &, PageLocation &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	PageLocation invalidPage = page;
	invalidPage.page_id = PageId{};
	expectFormatError(
			[&] {
				static_cast<void>(store->appendPageToExistingPool(pool, invalidPage, LogSequenceNumber(4),
																  [](const CatalogEntry &, PageLocation &) {}));
			},
			FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin invalidSnapshot;
	expectFormatError([&] { static_cast<void>(store->findPageLocationOptional(invalidSnapshot, PageId(1))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, ExistingPoolReplacementRejectsInvalidInputsAndPublishesOnlyCallbackFrame) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40), true, LogSequenceNumber(2)));
	CatalogEntry pool = entry(40, 1, 3);
	pool.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	static_cast<void>(store->upsertCatalog(std::span(&pool, 1), LogSequenceNumber(3)));
	MetadataSnapshotPin current = store->openSnapshot();
	pool = store->findCatalog(current, SegmentId(1));
	current.release();
	const PageLocation currentLocation{PageId(1), SegmentId(1), 1, Generation(1)};
	static_cast<void>(store->upsertLocations(std::span(&currentLocation, 1), {}, LogSequenceNumber(4)));

	PageLocation replacement{PageId(1), SegmentId(1), 0, Generation(1)};
	expectFormatError(
			[&] {
				static_cast<void>(store->replacePageInExistingPool(pool, currentLocation, replacement,
																   LogSequenceNumber(5),
																   ExistingPoolPageReplacementPreparer{}));
			},
			FormatError::FERR_INVALID_HEADER);
	CatalogEntry stale = pool;
	++stale.owner_id;
	expectFormatError(
			[&] {
				static_cast<void>(store->replacePageInExistingPool(
						stale, currentLocation, replacement, LogSequenceNumber(5),
						[](const CatalogEntry &, const PageLocation &, PageLocation &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	PageLocation wrongCurrent = currentLocation;
	wrongCurrent.page_index = 2;
	expectFormatError(
			[&] {
				static_cast<void>(store->replacePageInExistingPool(
						pool, wrongCurrent, replacement, LogSequenceNumber(5),
						[](const CatalogEntry &, const PageLocation &, PageLocation &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	PageLocation invalidCandidate = replacement;
	invalidCandidate.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
	expectFormatError(
			[&] {
				static_cast<void>(store->replacePageInExistingPool(
						pool, currentLocation, invalidCandidate, LogSequenceNumber(5),
						[](const CatalogEntry &, const PageLocation &, PageLocation &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(store->replacePageInExistingPool(
						pool, currentLocation, replacement, LogSequenceNumber(5),
						[](const CatalogEntry &, const PageLocation &, PageLocation &candidate) {
							candidate.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	const Superblock published = store->replacePageInExistingPool(
			pool, currentLocation, replacement, LogSequenceNumber(5),
			[](const CatalogEntry &, const PageLocation &, PageLocation &candidate) { candidate.page_index = 2; });
	EXPECT_EQ(published.generation, Generation(5));
	MetadataSnapshotPin replaced = store->openSnapshot();
	EXPECT_EQ(store->findPageLocation(replaced, PageId(1)).page_index, 2U);
}

TEST_F(RelocatableMetadataStoreTest, EvacuatesOnePageAcrossCurrentPoolsInOneDirectoryGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40), true, LogSequenceNumber(2)));
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(41), true, LogSequenceNumber(3)));
	CatalogEntry source = entry(40, 1, 4);
	source.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	CatalogEntry destination = entry(41, 1, 4);
	destination.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U);
	destination.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	static_cast<void>(store->upsertCatalog(std::vector{source, destination}, LogSequenceNumber(4)));
	MetadataSnapshotPin catalogSnapshot = store->openSnapshot();
	source = store->findCatalog(catalogSnapshot, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID));
	destination = store->findCatalog(catalogSnapshot, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U));
	catalogSnapshot.release();
	const PageLocation sourceLocation{PageId(1), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), 1, Generation(1), 32};
	static_cast<void>(store->upsertLocations(std::span(&sourceLocation, 1), {}, LogSequenceNumber(5)));
	MetadataSnapshotPin oldSnapshot = store->openSnapshot();
	const PageLocation expectedSource = store->findPageLocation(oldSnapshot, PageId(1));
	PageLocation target{PageId(1), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U), 0, Generation(1), 32};
	const CrossPoolPageEvacuationPreparer validPreparer = [](const CatalogEntry &, const PageLocation &,
															 const CatalogEntry &,
															 PageLocation &replacement) { replacement.page_index = 2; };
	expectFormatError(
			[&] {
				static_cast<void>(store->evacuatePageAcrossPools(CatalogEntry{}, expectedSource, destination, target,
																 LogSequenceNumber(6), validPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->evacuatePageAcrossPools(source, expectedSource, CatalogEntry{}, target,
																 LogSequenceNumber(6), validPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->evacuatePageAcrossPools(source, expectedSource, destination, target,
																 LogSequenceNumber(6),
																 CrossPoolPageEvacuationPreparer{}));
			},
			FormatError::FERR_INVALID_HEADER);
	CatalogEntry staleDestination = destination;
	++staleDestination.owner_id;
	expectFormatError(
			[&] {
				static_cast<void>(store->evacuatePageAcrossPools(source, expectedSource, staleDestination, target,
																 LogSequenceNumber(6), validPreparer));
			},
			FormatError::FERR_INVALID_STATE);
	PageLocation staleSource = expectedSource;
	staleSource.page_index = 2;
	expectFormatError(
			[&] {
				static_cast<void>(store->evacuatePageAcrossPools(source, staleSource, destination, target,
																 LogSequenceNumber(6), validPreparer));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(store->evacuatePageAcrossPools(
						source, expectedSource, destination, target, LogSequenceNumber(6),
						[](const CatalogEntry &, const PageLocation &, const CatalogEntry &, PageLocation &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	bool prepared = false;
	const Superblock published = store->evacuatePageAcrossPools(
			source, expectedSource, destination, target, LogSequenceNumber(6),
			[&prepared](const CatalogEntry &visibleSource, const PageLocation &visibleLocation,
						const CatalogEntry &visibleDestination, PageLocation &replacement) {
				prepared = true;
				EXPECT_EQ(visibleSource.segment_id, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID));
				EXPECT_EQ(visibleLocation.page_index, 1U);
				EXPECT_EQ(visibleDestination.segment_id, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U));
				replacement.page_index = 2;
			});
	EXPECT_TRUE(prepared);
	EXPECT_EQ(store->findPageLocation(oldSnapshot, PageId(1)), expectedSource);
	MetadataSnapshotPin currentSnapshot = store->openSnapshot();
	EXPECT_EQ(store->findPageLocation(currentSnapshot, PageId(1)),
			  (PageLocation{PageId(1), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U), 2, Generation(1), 32}));
	EXPECT_GT(published.generation, oldSnapshot.superblock().generation);
	oldSnapshot.release();
	currentSnapshot.release();

	expectFormatError(
			[&] {
				static_cast<void>(store->evacuatePageAcrossPools(source, expectedSource, source, target,
																 LogSequenceNumber(7), validPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, RetiresADurableVectorManifestWithItsPageDirectoryMapping) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest request;
	request.segment_id = SegmentId(31);
	request.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
	request.owner_id = 17;
	request.segment_generation = Generation(1);
	request.state = SegmentState::SGS_SEALED;
	request.require_absent = true;
	const PageLocation location{PageId(91), SegmentId(31), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)};
	const VectorManifestLocation manifest{17, PageId(91), Generation(1)};
	const auto reservation = store->reserveFirstFreeAndUpsertCatalogAndVectorManifests(
			std::span(&request, 1), std::span(&location, 1), std::span(&manifest, 1), PhysicalSlotId(40),
			PhysicalSlotId(40), LogSequenceNumber(2), [](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(reservation.has_value());
	const CatalogEntry pool = reservation->entries.front();
	expectFormatError(
			[&] {
				static_cast<void>(store->retireVectorManifestPageInExistingPool(
						pool, location, manifest, LogSequenceNumber(3), ExistingPoolPageRetirementPreparer{}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->retireVectorManifestPageInExistingPool(
						pool, location, manifest, LogSequenceNumber{},
						[](const CatalogEntry &, const PageLocation &) {}));
			},
			FormatError::FERR_INVALID_HEADER);
	const VectorManifestLocation mismatchedManifest{17, PageId(92), Generation(1)};
	expectFormatError(
			[&] {
				static_cast<void>(store->retireVectorManifestPageInExistingPool(
						pool, location, mismatchedManifest, LogSequenceNumber(3),
						[](const CatalogEntry &, const PageLocation &) {}));
			},
			FormatError::FERR_INVALID_HEADER);
	CatalogEntry stalePool = pool;
	++stalePool.owner_id;
	expectFormatError(
			[&] {
				static_cast<void>(store->retireVectorManifestPageInExistingPool(
						stalePool, location, manifest, LogSequenceNumber(3),
						[](const CatalogEntry &, const PageLocation &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	PageLocation staleLocation = location;
	staleLocation.page_index = 2;
	expectFormatError(
			[&] {
				static_cast<void>(store->retireVectorManifestPageInExistingPool(
						pool, staleLocation, manifest, LogSequenceNumber(3),
						[](const CatalogEntry &, const PageLocation &) {}));
			},
			FormatError::FERR_INVALID_STATE);

	bool prepared = false;
	MetadataSnapshotPin oldSnapshot = store->openSnapshot();
	static_cast<void>(store->retireVectorManifestPageInExistingPool(
			pool, location, manifest, LogSequenceNumber(3),
			[&prepared](const CatalogEntry &visiblePool, const PageLocation &visibleLocation) {
				prepared = true;
				EXPECT_EQ(visiblePool.segment_id, SegmentId(31));
				EXPECT_EQ(visibleLocation, (PageLocation{PageId(91), SegmentId(31), 1, Generation(1)}));
			}));
	EXPECT_TRUE(prepared);
	EXPECT_EQ(store->findPageLocation(oldSnapshot, PageId(91)), location);
	EXPECT_EQ(store->findVectorManifest(oldSnapshot, 17), manifest);
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_FALSE(store->findPageLocationOptional(current, PageId(91)).has_value());
	EXPECT_FALSE(store->findVectorManifestOptional(current, 17).has_value());
}

TEST_F(RelocatableMetadataStoreTest, EvacuatesAVectorManifestAndRebindsItsGenerationInOnePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest sourceRequest;
	sourceRequest.segment_id = SegmentId(31);
	sourceRequest.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
	sourceRequest.owner_id = 17;
	sourceRequest.segment_generation = Generation(1);
	sourceRequest.state = SegmentState::SGS_SEALED;
	sourceRequest.require_absent = true;
	const PageLocation sourceLocation{PageId(91), SegmentId(31), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)};
	const VectorManifestLocation sourceManifest{17, PageId(91), Generation(1)};
	const auto sourceReservation = store->reserveFirstFreeAndUpsertCatalogAndVectorManifests(
			std::span(&sourceRequest, 1), std::span(&sourceLocation, 1), std::span(&sourceManifest, 1),
			PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2), [](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(sourceReservation.has_value());
	CatalogSlotReservationRequest destinationRequest = sourceRequest;
	destinationRequest.segment_id = SegmentId(32);
	const PageLocation destinationSeed{PageId(92), SegmentId(32), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)};
	const auto destinationReservation = store->reserveFirstFreeAndUpsertCatalogAndLocations(
			std::span(&destinationRequest, 1), std::span(&destinationSeed, 1), {}, PhysicalSlotId(41),
			PhysicalSlotId(41), LogSequenceNumber(3), [](std::span<const CatalogEntry>) {});
	ASSERT_TRUE(destinationReservation.has_value());

	MetadataSnapshotPin catalogSnapshot = store->openSnapshot();
	const CatalogEntry sourcePool = store->findCatalog(catalogSnapshot, SegmentId(31));
	const CatalogEntry destinationPool = store->findCatalog(catalogSnapshot, SegmentId(32));
	catalogSnapshot.release();
	const PageLocation destination{PageId(91), SegmentId(32), 0, Generation(1)};
	const VectorManifestLocation replacementManifest{17, PageId(91), Generation(1)};
	VectorManifestLocation staleManifest = sourceManifest;
	staleManifest.generation = Generation(2);
	expectFormatError(
			[&] {
				static_cast<void>(store->evacuateVectorManifestPageAcrossPools(
						sourcePool, sourceLocation, destinationPool, destination, staleManifest, replacementManifest,
						LogSequenceNumber(4),
						[](const CatalogEntry &, const PageLocation &, const CatalogEntry &, PageLocation &candidate) {
							candidate.page_index = 2;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin oldSnapshot = store->openSnapshot();
	static_cast<void>(store->evacuateVectorManifestPageAcrossPools(
			sourcePool, sourceLocation, destinationPool, destination, sourceManifest, replacementManifest,
			LogSequenceNumber(4),
			[](const CatalogEntry &, const PageLocation &, const CatalogEntry &, PageLocation &candidate) {
				candidate.page_index = 2;
			}));
	EXPECT_EQ(store->findPageLocation(oldSnapshot, PageId(91)), sourceLocation);
	EXPECT_EQ(store->findVectorManifest(oldSnapshot, 17), sourceManifest);
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findPageLocation(current, PageId(91)),
			  (PageLocation{PageId(91), SegmentId(32), 2, Generation(1)}));
	EXPECT_EQ(store->findVectorManifest(current, 17), replacementManifest);
}

TEST_F(RelocatableMetadataStoreTest, RejectsIncompleteVectorReservationInputsBeforeAcquiringAPoolSlot) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationPreparer preparer = [](std::span<const CatalogEntry>) {};
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndVectorManifests(
						{}, {}, {}, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2), preparer));
			},
			FormatError::FERR_INVALID_HEADER);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
}

TEST_F(RelocatableMetadataStoreTest, RejectsVectorManifestReservationsOutsideTheirDedicatedPool) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest rawPool;
	rawPool.segment_id = SegmentId(31);
	rawPool.kind = SegmentKind::SGK_VECTOR_RAW_POOL;
	rawPool.owner_id = 17;
	rawPool.segment_generation = Generation(1);
	rawPool.state = SegmentState::SGS_SEALED;
	rawPool.require_absent = true;
	const PageLocation page{PageId(91), SegmentId(31), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)};
	const VectorManifestLocation manifest{17, PageId(91), Generation(1)};
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndVectorManifests(
						std::span(&rawPool, 1), std::span(&page, 1), std::span(&manifest, 1), PhysicalSlotId(40),
						PhysicalSlotId(40), LogSequenceNumber(2), [](std::span<const CatalogEntry>) {}));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
}

TEST_F(RelocatableMetadataStoreTest, RejectsEveryMalformedVectorManifestReservationTarget) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const CatalogSlotReservationRequest pool{.segment_id = SegmentId(31),
											 .kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL,
											 .owner_id = 17,
											 .segment_generation = Generation(1),
											 .state = SegmentState::SGS_SEALED,
											 .require_absent = true};
	const PageLocation page{PageId(91), SegmentId(31), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)};
	const auto expectRejected = [&](const CatalogSlotReservationRequest &request, const PageLocation &candidatePage,
									const VectorManifestLocation &manifest) {
		expectFormatError(
				[&] {
					static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndVectorManifests(
							std::span(&request, 1), std::span(&candidatePage, 1), std::span(&manifest, 1),
							PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2),
							[](std::span<const CatalogEntry>) {}));
				},
				FormatError::FERR_INVALID_STATE);
	};
	expectRejected(pool, page, {0, PageId(91), Generation(1)});
	expectRejected(pool, page, {17, {}, Generation(1)});
	expectRejected(pool, page, {17, PageId(91), {}});
	expectRejected(pool, page, {17, PageId(92), Generation(1)});
	expectRejected(pool, page, {17, PageId(91), Generation(2)});
	CatalogSlotReservationRequest wrongOwner = pool;
	wrongOwner.owner_id = 18;
	expectRejected(wrongOwner, page, {17, PageId(91), Generation(1)});
}

TEST_F(RelocatableMetadataStoreTest, RejectsRelocatedSourceReleaseWhenCatalogDestinationLostAllocationOwnership) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40), true, LogSequenceNumber(2)));
	CatalogEntry entity = entry(40, 1, 3);
	static_cast<void>(store->upsertCatalog(std::span(&entity, 1), LogSequenceNumber(3)));
	ASSERT_TRUE(store->relocateCatalogSegment(SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), Generation(1),
											  PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(4),
											  [](const CatalogEntry &, const CatalogEntry &) {})
						.has_value());
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(41), false, LogSequenceNumber(5)));
	expectFormatError(
			[&] {
				static_cast<void>(store->releaseRelocatedCatalogSource(SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
																	   Generation(2), LogSequenceNumber(6)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, PublishesPreparedLocationDataBeforeTheDirectoryRoot) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const PageLocation location{.page_id = PageId(91),
								.segment_id = SegmentId(31),
								.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME,
								.generation = Generation(1)};
	bool prepared = false;
	const Superblock published =
			store->upsertLocations(std::span(&location, 1), {}, LogSequenceNumber(2), [&] { prepared = true; });
	EXPECT_TRUE(prepared);
	EXPECT_EQ(published.generation, Generation(2));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(store->findPageLocation(snapshot, location.page_id), location);
}

TEST_F(RelocatableMetadataStoreTest, SerializesPoolReconciliationAgainstCurrentReachability) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1);
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40), PhysicalSlotId(40),
														LogSequenceNumber(2))
						.has_value());
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(41), true, LogSequenceNumber(3)));
	CatalogEntry unavailable = entry(41, 1, 4);
	unavailable.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U);
	static_cast<void>(store->upsertCatalog(std::span(&unavailable, 1), LogSequenceNumber(4)));
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(41), false, LogSequenceNumber(5)));
	bool reconciled = false;
	store->reconcileExistingPool(
			SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
			[&](const CatalogEntry &visible, const std::function<std::optional<PageLocation>(PageId)> &findPageLocation,
				const std::function<CatalogEntry(SegmentId)> &findCatalog,
				const ExistingPoolFrameVisibility &isFrameVisibleToActiveSnapshot) {
				reconciled = true;
				EXPECT_EQ(visible.physical_slot, PhysicalSlotId(40));
				EXPECT_FALSE(findPageLocation(PageId(91)).has_value());
				EXPECT_EQ(findCatalog(SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).segment_id,
						  SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID));
				EXPECT_FALSE(isFrameVisibleToActiveSnapshot(PageId(91), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), 1U,
															Generation(1)));
				expectFormatError(
						[&] { static_cast<void>(findCatalog(SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U))); },
						FormatError::FERR_INVALID_STATE);
			});
	EXPECT_TRUE(reconciled);

	ExistingPoolReconciler empty;
	expectFormatError([&] { store->reconcileExistingPool(SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), empty); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				store->reconcileExistingPool({}, [](const CatalogEntry &, const auto &, const auto &, const auto &) {});
			},
			FormatError::FERR_INVALID_HEADER);
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40), false, LogSequenceNumber(6)));
	expectFormatError(
			[&] {
				store->reconcileExistingPool(SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
											 [](const CatalogEntry &, const auto &, const auto &, const auto &) {});
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, ReconcilesEveryCurrentSealedCatalogEntryUnderOneWriter) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	uint64_t visited = 0;
	store->reconcileCurrentPools([&](const CatalogEntry &, std::span<const PageLocation>) { ++visited; });
	EXPECT_EQ(visited, 0U);
	const CurrentPoolReconciler empty;
	expectFormatError([&] { store->reconcileCurrentPools(empty); }, FormatError::FERR_INVALID_HEADER);

	const std::vector<CatalogSlotReservationRequest> requests =
			reservationRequests(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 2);
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(std::span<const CatalogSlotReservationRequest>(requests),
														PhysicalSlotId(40), PhysicalSlotId(41), LogSequenceNumber(2))
						.has_value());
	std::vector<SegmentId> current;
	store->reconcileCurrentPools(
			[&](const CatalogEntry &entry, std::span<const PageLocation>) { current.push_back(entry.segment_id); });
	EXPECT_EQ(current, (std::vector<SegmentId>{SegmentId(1), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
											   SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U)}));

	static_cast<void>(store->retireCatalogSegment(SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), Generation(1),
												  LogSequenceNumber(3)));
	current.clear();
	store->reconcileCurrentPools(
			[&](const CatalogEntry &entry, std::span<const PageLocation>) { current.push_back(entry.segment_id); });
	EXPECT_EQ(current, (std::vector<SegmentId>{SegmentId(1), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U)}));
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(41), false, LogSequenceNumber(4)));
	expectFormatError([&] { store->reconcileCurrentPools([](const CatalogEntry &, const auto &) {}); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, RejectsCurrentPageDirectoryLocationsOutsideSharedPools) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1);
	request.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40), PhysicalSlotId(40),
														LogSequenceNumber(2))
						.has_value());
	const PageLocation invalid{.page_id = PageId(91),
							   .segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
							   .page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME,
							   .generation = Generation(1)};
	static_cast<void>(store->upsertLocations(std::span(&invalid, 1), {}, LogSequenceNumber(3)));
	expectFormatError([&] { store->reconcileCurrentPools([](const CatalogEntry &, const auto &) {}); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, RejectsEmptyOrFailingPreparedLocationPublicationWithoutPublishing) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const PageLocation location{.page_id = PageId(92),
								.segment_id = SegmentId(31),
								.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME,
								.generation = Generation(1)};
	const LocationPublicationPreparer empty;
	expectFormatError(
			[&] {
				static_cast<void>(store->upsertLocations(std::span(&location, 1), {}, LogSequenceNumber(2), empty));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_THROW(
			static_cast<void>(store->upsertLocations(std::span(&location, 1), {}, LogSequenceNumber(2),
													 [] { throw std::runtime_error("prepared page write failed"); })),
			std::runtime_error);
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 2U);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1));
	expectFormatError([&] { static_cast<void>(store->findPageLocation(snapshot, location.page_id)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, AtomicallyPublishesNewCatalogPoolAndInitialPageLocation) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest request = reservationRequest(31, 1);
	request.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	request.require_absent = true;
	const PageLocation page{.page_id = PageId(93),
							.segment_id = request.segment_id,
							.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME,
							.generation = request.segment_generation};
	bool prepared = false;
	const std::optional<CatalogSlotReservations> reservation = store->reserveFirstFreeAndUpsertCatalogAndLocations(
			std::span(&request, 1), std::span(&page, 1), {}, PhysicalSlotId(40), PhysicalSlotId(40),
			LogSequenceNumber(2), [&](std::span<const CatalogEntry> entries) {
				prepared = entries.size() == 1U && entries.front().physical_slot == PhysicalSlotId(40);
			});
	ASSERT_TRUE(reservation.has_value());
	EXPECT_TRUE(prepared);
	EXPECT_EQ(reservation->superblock.generation, Generation(2));
	ASSERT_EQ(reservation->entries.size(), 1U);
	EXPECT_EQ(reservation->entries.front().physical_slot, PhysicalSlotId(40));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_TRUE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
	const CatalogEntry visible = store->findCatalog(snapshot, request.segment_id);
	EXPECT_EQ(visible.segment_id, request.segment_id);
	EXPECT_EQ(visible.physical_slot, reservation->entries.front().physical_slot);
	EXPECT_EQ(visible.kind, request.kind);
	EXPECT_EQ(visible.generation, request.segment_generation);
	EXPECT_EQ(store->findPageLocation(snapshot, page.page_id), page);
}

TEST_F(RelocatableMetadataStoreTest, PublishesAVectorManifestMappingForAnAlreadyDurableManifestPage) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest request = reservationRequest(31, 1);
	request.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
	request.owner_id = 17;
	request.require_absent = true;
	const PageLocation page{.page_id = PageId(93),
							.segment_id = request.segment_id,
							.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME,
							.generation = request.segment_generation};
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(
							 std::span(&request, 1), std::span(&page, 1), {}, PhysicalSlotId(40), PhysicalSlotId(40),
							 LogSequenceNumber(2), [](std::span<const CatalogEntry>) {})
						.has_value());

	MetadataSnapshotPin beforePublication = store->openSnapshot();
	const VectorManifestLocation manifest{17, page.page_id, page.generation};
	static_cast<void>(store->upsertVectorManifests(std::span(&manifest, 1), {}, LogSequenceNumber(3)));
	EXPECT_FALSE(store->findVectorManifestOptional(beforePublication, manifest.index_id).has_value());
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findVectorManifest(current, manifest.index_id), manifest);
	EXPECT_EQ(store->findPageLocation(current, manifest.manifest_page_id), page);
	beforePublication.release();
	current.release();
}

TEST_F(RelocatableMetadataStoreTest, RejectsInvalidCombinedLocationBeforePreparingOrPublishingData) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest request = reservationRequest(31, 1);
	request.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	request.require_absent = true;
	PageLocation foreign{.page_id = PageId(94),
						 .segment_id = SegmentId(32),
						 .page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME,
						 .generation = Generation(1)};
	bool prepared = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndLocations(
						std::span(&request, 1), std::span(&foreign, 1), {}, PhysicalSlotId(40), PhysicalSlotId(40),
						LogSequenceNumber(2), [&](std::span<const CatalogEntry>) { prepared = true; }));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(prepared);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));

	const CatalogSlotReservationPreparer empty;
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndLocations(
						std::span(&request, 1), std::span(&foreign, 1), {}, PhysicalSlotId(40), PhysicalSlotId(40),
						LogSequenceNumber(2), empty));
			},
			FormatError::FERR_INVALID_HEADER);

	const PageLocation mismatchedGeneration{.page_id = PageId(95),
											.segment_id = request.segment_id,
											.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME,
											.generation = Generation(2)};
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndLocations(
						std::span(&request, 1), std::span(&mismatchedGeneration, 1), {}, PhysicalSlotId(40),
						PhysicalSlotId(40), LogSequenceNumber(2),
						[&](std::span<const CatalogEntry>) { prepared = true; }));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(prepared);

	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndLocations(
						std::span(&request, 1), {}, {}, PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2),
						[&](std::span<const CatalogEntry>) { prepared = true; }));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, PublishesCombinedExtentLocationsAndRejectsForeignExtentGenerations) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest request = reservationRequest(31, 1);
	request.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	request.require_absent = true;
	const ExtentLocation extent{.extent_id = ExtentId(96),
								.segment_id = request.segment_id,
								.first_page = RSS_PAGE_POOL_FIRST_DATA_FRAME,
								.page_count = 2,
								.generation = request.segment_generation};
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(
							 std::span(&request, 1), {}, std::span(&extent, 1), PhysicalSlotId(40), PhysicalSlotId(40),
							 LogSequenceNumber(2), [](std::span<const CatalogEntry>) {})
						.has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(store->findExtentLocation(snapshot, extent.extent_id), extent);
	snapshot.release();

	CatalogSlotReservationRequest otherRequest = reservationRequest(41, 1);
	otherRequest.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	otherRequest.require_absent = true;
	ExtentLocation duplicateExtent = extent;
	duplicateExtent.segment_id = otherRequest.segment_id;
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndLocations(
						std::span(&otherRequest, 1), {}, std::span(&duplicateExtent, 1), PhysicalSlotId(41),
						PhysicalSlotId(41), LogSequenceNumber(3), [](std::span<const CatalogEntry>) {}));
			},
			FormatError::FERR_INVALID_STATE);
	ExtentLocation foreignExtent = extent;
	foreignExtent.extent_id = ExtentId(97);
	foreignExtent.segment_id = SegmentId(42);
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndLocations(
						std::span(&otherRequest, 1), {}, std::span(&foreignExtent, 1), PhysicalSlotId(40),
						PhysicalSlotId(40), LogSequenceNumber(2), [](std::span<const CatalogEntry>) {}));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, RelocatesACodebookExtentWithItsCatalogGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest request;
	request.segment_id = SegmentId(31);
	request.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	request.owner_id = 17;
	request.segment_generation = Generation(1);
	request.state = SegmentState::SGS_SEALED;
	request.require_absent = true;
	const ExtentLocation extent{ExtentId(91), request.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
								RSS_PAGE_POOL_DATA_FRAME_COUNT, request.segment_generation};
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(
							 std::span(&request, 1), {}, std::span(&extent, 1), PhysicalSlotId(40), PhysicalSlotId(40),
							 LogSequenceNumber(2), [](std::span<const CatalogEntry>) {})
						.has_value());
	bool prepared = false;
	const std::optional<CatalogSlotRelocation> relocation = store->relocateCatalogExtentSegment(
			extent, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3),
			[&prepared](const CatalogEntry &source, const CatalogEntry &destination) {
				prepared = true;
				EXPECT_EQ(source.kind, SegmentKind::SGK_CODEBOOK_EXTENT);
				EXPECT_EQ(destination.generation, Generation(2));
			});
	ASSERT_TRUE(relocation.has_value());
	EXPECT_TRUE(prepared);
	ExtentLocation secondRelocation = extent;
	secondRelocation.generation = Generation(2);
	expectFormatError(
			[&] {
				static_cast<void>(store->relocateCatalogExtentSegment(
						secondRelocation, PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(4),
						[](const CatalogEntry &, const CatalogEntry &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->findCatalog(current, request.segment_id).generation, Generation(2));
	EXPECT_EQ(store->findExtentLocation(current, extent.extent_id).generation, Generation(2));
}

TEST_F(RelocatableMetadataStoreTest, BootstrapAndEmergencyPoolsAreDurablyAllocatedAndNeverReturnedToUserReservations) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	MetadataSnapshotPin initialSnapshot = store->openSnapshot();
	EXPECT_TRUE(initialSnapshot.superblock().allocator_root.isValid());
	EXPECT_TRUE(store->isPhysicalSlotAllocated(initialSnapshot, PhysicalSlotId(1)));
	EXPECT_TRUE(store->isPhysicalSlotAllocated(initialSnapshot, PhysicalSlotId(2)));
	EXPECT_TRUE(initialSnapshot.superblock().metadata_emergency_reserve.isValid());
	EXPECT_EQ(initialSnapshot.superblock().metadata_emergency_reserve.physical_slot, PhysicalSlotId(2));
	initialSnapshot.release();

	const std::optional<AllocatorReservation> reservation =
			store->reserveFirstFreeSlot(PhysicalSlotId(1), PhysicalSlotId(3), LogSequenceNumber(2));
	ASSERT_TRUE(reservation.has_value());
	EXPECT_EQ(reservation->physical_slot, PhysicalSlotId(3));
	EXPECT_THROW(static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(1), false, LogSequenceNumber(3))),
				 FormatException);
	EXPECT_THROW(static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(2), false, LogSequenceNumber(3))),
				 FormatException);
}

TEST_F(RelocatableMetadataStoreTest, AtomicallyPublishesAndRecoversAdditionalCatalogPoolOwnership) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const Superblock published = store->upsertCatalog(entriesThatRequireAnotherMetadataPool(), LogSequenceNumber(2));
	EXPECT_EQ(store->pagePoolStats().pools, 3U);
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 33U);

	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_TRUE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(1)));
	EXPECT_TRUE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(2)));
	EXPECT_TRUE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(3)));
	snapshot.release();
	EXPECT_FALSE(store->reserveFirstFreeSlot(PhysicalSlotId(1), PhysicalSlotId(3), LogSequenceNumber(3)).has_value());

	auto reopened = RelocatableMetadataStore::open(io);
	EXPECT_EQ(reopened->pagePoolStats().pools, 3U);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopenedSnapshot.superblock().generation, published.generation);
	EXPECT_EQ(reopenedSnapshot.superblock().checkpoint_lsn, published.checkpoint_lsn);
	EXPECT_EQ(reopenedSnapshot.superblock().catalog_root, published.catalog_root);
	EXPECT_EQ(reopenedSnapshot.superblock().allocator_root, published.allocator_root);
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(reopenedSnapshot, PhysicalSlotId(1)));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(reopenedSnapshot, PhysicalSlotId(2)));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(reopenedSnapshot, PhysicalSlotId(3)));
}

TEST_F(RelocatableMetadataStoreTest, OpenFailsClosedWhenCatalogMetadataPoolIdentityIsCorrupted) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	static_cast<void>(store->upsertCatalog(entriesThatRequireAnotherMetadataPool(), LogSequenceNumber(2)));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	std::vector<CatalogEntry> metadataPools;
	store->visitCatalogEntries(snapshot, [&metadataPools](const CatalogEntry &entry) {
		if (entry.kind == SegmentKind::SGK_METADATA_PAGE_POOL) {
			metadataPools.push_back(entry);
		}
	});
	ASSERT_GE(metadataPools.size(), 2U);
	const CatalogEntry primary = metadataPools.front();
	const CatalogEntry alternate = metadataPools.back();
	ASSERT_EQ(primary.segment_id, SegmentId(1));
	ASSERT_NE(primary.physical_slot, alternate.physical_slot);
	const Superblock current = snapshot.superblock();
	snapshot.release();
	store.reset();

	MetadataPageFile pages(io, current);
	const CatalogRadixPage root = pages.readCatalogRadix(current.catalog_root);
	ASSERT_EQ(root.level, 0U);
	const PhysicalPageRef leafReference = CatalogRadixPageCodec::childFor(root, primary.segment_id);
	const CatalogLeafPage originalLeaf = pages.readCatalogLeaf(leafReference);
	const auto locatePrimary = [](CatalogLeafPage &leaf) -> CatalogEntry & {
		for (CatalogEntry &entry: leaf.entries) {
			if (entry.segment_id == SegmentId(1)) {
				return entry;
			}
		}
		throw std::logic_error("bootstrap metadata pool is missing from the Catalog leaf");
	};
	CatalogLeafPage verificationLeaf = originalLeaf;
	ASSERT_EQ(locatePrimary(verificationLeaf).physical_slot, primary.physical_slot);

	const auto expectCatalogCorruption = [&](const std::function<void(CatalogEntry &)> &corrupt) {
		CatalogLeafPage malformed = originalLeaf;
		corrupt(locatePrimary(malformed));
		pages.writeCatalogLeaf(leafReference, malformed);
		io->sync();
		EXPECT_THROW(static_cast<void>(RelocatableMetadataStore::open(io)), FormatException);
		pages.writeCatalogLeaf(leafReference, originalLeaf);
		io->sync();
	};

	expectCatalogCorruption([](CatalogEntry &entry) { ++entry.owner_id; });
	expectCatalogCorruption([&alternate](CatalogEntry &entry) {
		entry.physical_slot = alternate.physical_slot;
		entry.owner_id = alternate.physical_slot.value();
	});
	const Generation mismatchedGeneration =
			primary.generation == current.generation ? Generation(primary.generation.value() - 1U) : current.generation;
	expectCatalogCorruption([mismatchedGeneration](CatalogEntry &entry) { entry.generation = mismatchedGeneration; });
	const LogSequenceNumber mismatchedLsn = primary.committed_lsn == current.checkpoint_lsn
													? LogSequenceNumber(primary.committed_lsn.value() - 1U)
													: current.checkpoint_lsn;
	expectCatalogCorruption([mismatchedLsn](CatalogEntry &entry) { entry.committed_lsn = mismatchedLsn; });

	const uint64_t headerOffset =
			current.data_region_offset + (primary.physical_slot.value() - 1U) * RSS_SEGMENT_SIZE + RSS_ENVELOPE_BYTES;
	std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> originalHeaderBytes{};
	ASSERT_EQ(io->readAt(headerOffset, originalHeaderBytes.data(), originalHeaderBytes.size()),
			  originalHeaderBytes.size());
	MetadataPagePoolHeader malformedHeader = MetadataPagePoolHeaderCodec::decode(originalHeaderBytes);
	malformedHeader.segment_id = alternate.segment_id;
	const auto malformedHeaderBytes = MetadataPagePoolHeaderCodec::encode(malformedHeader);
	io->writeAt(headerOffset, malformedHeaderBytes.data(), malformedHeaderBytes.size());
	io->sync();
	EXPECT_THROW(static_cast<void>(RelocatableMetadataStore::open(io)), FormatException);
	io->writeAt(headerOffset, originalHeaderBytes.data(), originalHeaderBytes.size());
	io->sync();
}

TEST_F(RelocatableMetadataStoreTest, ReclaimsAnEmptyCatalogMetadataPoolOnlyAfterSnapshotReleaseAndReusesItsSlot) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	std::vector<MetadataSnapshotPin> retainedSnapshots = createReclaimableMetadataPool(*store);
	const uint64_t poolsBeforeReclamation = store->pagePoolStats().pools;
	ASSERT_GE(poolsBeforeReclamation, 2U);

	const MetadataPagePoolReclamation protectedReclamation = store->reclaimEmptyMetadataPagePools(LogSequenceNumber(7));
	EXPECT_EQ(protectedReclamation.reclaimed_pool_slots, 0U);

	for (MetadataSnapshotPin &snapshot: retainedSnapshots) {
		snapshot.release();
	}
	const MetadataPagePoolReclamation reclamation = store->reclaimEmptyMetadataPagePools(LogSequenceNumber(7));
	ASSERT_GE(reclamation.reclaimed_pool_slots, 2U);
	ASSERT_EQ(reclamation.released_pool_slots.size(), reclamation.reclaimed_pool_slots);
	EXPECT_LT(store->pagePoolStats().pools, poolsBeforeReclamation);

	MetadataSnapshotPin reclaimedSnapshot = store->openSnapshot();
	for (const PhysicalSlotId releasedSlot: reclamation.released_pool_slots) {
		EXPECT_FALSE(store->isPhysicalSlotAllocated(reclaimedSnapshot, releasedSlot));
		bool staleCatalogOwnership = false;
		store->visitCatalogEntries(
				reclaimedSnapshot, [releasedSlot, &staleCatalogOwnership](const CatalogEntry &entry) {
					if (entry.kind == SegmentKind::SGK_METADATA_PAGE_POOL && entry.physical_slot == releasedSlot) {
						staleCatalogOwnership = true;
					}
				});
		EXPECT_FALSE(staleCatalogOwnership);
	}
	reclaimedSnapshot.release();

	const PhysicalSlotId reclaimedSlot = reclamation.released_pool_slots.front();
	const CatalogSlotReservationRequest request = reservationRequest(10000U, 1U);
	const std::optional<CatalogSlotReservation> reused =
			store->reserveFirstFreeAndUpsertCatalog(request, reclaimedSlot, reclaimedSlot, LogSequenceNumber(8));
	ASSERT_TRUE(reused.has_value());
	EXPECT_EQ(reused->entry.physical_slot, reclaimedSlot);

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(reopenedSnapshot, request.segment_id).physical_slot, reclaimedSlot);
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(reopenedSnapshot, reclaimedSlot));
}

TEST_F(RelocatableMetadataStoreTest, EvacuatesASparseMetadataPoolThenReclaimsAndReusesItsSlot) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const std::vector<CatalogEntry> entries = entriesThatRequireAnotherMetadataPool();
	static_cast<void>(store->upsertCatalog(entries, LogSequenceNumber(2)));

	MetadataSnapshotPin pinned = store->openSnapshot();
	const MetadataPagePoolEvacuation blocked = store->evacuateSparseMetadataPagePool(LogSequenceNumber(3), 8U);
	EXPECT_EQ(blocked.superblock, pinned.superblock());
	EXPECT_FALSE(blocked.source_pool_slot.isValid());
	EXPECT_EQ(blocked.relocated_metadata_pages, 0U);
	pinned.release();

	const MetadataPagePoolEvacuation evacuation = store->evacuateSparseMetadataPagePool(LogSequenceNumber(3), 8U);
	ASSERT_TRUE(evacuation.source_pool_slot.isValid());
	EXPECT_EQ(evacuation.superblock.generation, Generation(3));
	EXPECT_GT(evacuation.relocated_metadata_pages, 0U);
	const uint64_t sourceHeaderOffset = evacuation.superblock.data_region_offset +
										(evacuation.source_pool_slot.value() - 1U) * RSS_SEGMENT_SIZE +
										RSS_ENVELOPE_BYTES;
	std::array<std::byte, RSS_METADATA_PAGE_POOL_HEADER_BYTES> sourceHeaderBytes{};
	ASSERT_EQ(io->readAt(sourceHeaderOffset, sourceHeaderBytes.data(), sourceHeaderBytes.size()),
			  sourceHeaderBytes.size());
	EXPECT_EQ(MetadataPagePoolHeaderCodec::decode(sourceHeaderBytes).allocated_frames, 0U);

	MetadataSnapshotPin evacuatedSnapshot = store->openSnapshot();
	const CatalogEntry evacuatedEntry = store->findCatalog(evacuatedSnapshot, entries.front().segment_id);
	EXPECT_EQ(evacuatedEntry.physical_slot, entries.front().physical_slot);
	EXPECT_EQ(evacuatedEntry.generation, entries.front().generation);
	EXPECT_TRUE(store->isPhysicalSlotAllocated(evacuatedSnapshot, evacuation.source_pool_slot));
	evacuatedSnapshot.release();

	const MetadataPagePoolReclamation reclamation = store->reclaimEmptyMetadataPagePools(LogSequenceNumber(4));
	EXPECT_NE(std::find(reclamation.released_pool_slots.begin(), reclamation.released_pool_slots.end(),
						evacuation.source_pool_slot),
			  reclamation.released_pool_slots.end());
	const CatalogSlotReservationRequest request = reservationRequest(10000U, 1U);
	const std::optional<CatalogSlotReservation> reused = store->reserveFirstFreeAndUpsertCatalog(
			request, evacuation.source_pool_slot, evacuation.source_pool_slot, LogSequenceNumber(5));
	ASSERT_TRUE(reused.has_value());
	EXPECT_EQ(reused->entry.physical_slot, evacuation.source_pool_slot);

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(reopenedSnapshot, request.segment_id).physical_slot, evacuation.source_pool_slot);
}

TEST_F(RelocatableMetadataStoreTest, SparseMetadataPoolEvacuationPreservesTheOldRootBeforePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const std::vector<CatalogEntry> entries = entriesThatRequireAnotherMetadataPool();
	static_cast<void>(store->upsertCatalog(entries, LogSequenceNumber(2)));

	EXPECT_THROW(static_cast<void>(store->evacuateSparseMetadataPagePool(
						 LogSequenceNumber(3), 8U,
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(2));
	EXPECT_EQ(store->findCatalog(snapshot, entries.back().segment_id).physical_slot, entries.back().physical_slot);
	snapshot.release();

	const MetadataPagePoolEvacuation published = store->evacuateSparseMetadataPagePool(LogSequenceNumber(3), 8U);
	EXPECT_EQ(published.superblock.generation, Generation(3));
}

TEST_F(RelocatableMetadataStoreTest, SparseMetadataPoolEvacuationRecoversTheDurableNewRootAfterPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const std::vector<CatalogEntry> entries = entriesThatRequireAnotherMetadataPool();
	static_cast<void>(store->upsertCatalog(entries, LogSequenceNumber(2)));

	EXPECT_THROW(static_cast<void>(store->evacuateSparseMetadataPagePool(
						 LogSequenceNumber(3), 8U,
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(3));
	EXPECT_EQ(reopened->findCatalog(snapshot, entries.front().segment_id).physical_slot, entries.front().physical_slot);
	snapshot.release();
	EXPECT_GT(reopened->reclaimEmptyMetadataPagePools(LogSequenceNumber(4)).reclaimed_pool_slots, 0U);
}

TEST_F(RelocatableMetadataStoreTest, SparseMetadataPoolEvacuationValidatesItsRequestAndSkipsBootstrapOnlyState) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const MetadataPagePoolEvacuation bootstrapOnly = store->evacuateSparseMetadataPagePool(LogSequenceNumber(2), 1U);
	EXPECT_EQ(bootstrapOnly.superblock.generation, Generation(1));
	EXPECT_FALSE(bootstrapOnly.source_pool_slot.isValid());
	expectFormatError([&] { static_cast<void>(store->evacuateSparseMetadataPagePool(LogSequenceNumber(2), 0U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->evacuateSparseMetadataPagePool(LogSequenceNumber(2),
																		RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U));
			},
			FormatError::FERR_INVALID_HEADER);

	static_cast<void>(store->upsertCatalog(std::vector{entry(100, 1, 2)}, LogSequenceNumber(2)));
	store.reset();
	auto readOnly = RelocatableMetadataStore::openReadOnly(io);
	expectFormatError([&] { static_cast<void>(readOnly->evacuateSparseMetadataPagePool(LogSequenceNumber(3), 1U)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, SparseMetadataPoolEvacuationClonesEveryTypedDirectoryRoot) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const std::array requests = {
			CatalogSlotReservationRequest{.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
										  .kind = SegmentKind::SGK_ENTITY_PAGE_POOL,
										  .owner_id = 1,
										  .segment_generation = Generation(1),
										  .state = SegmentState::SGS_SEALED,
										  .require_absent = true},
			CatalogSlotReservationRequest{.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U),
										  .kind = SegmentKind::SGK_PROPERTY_PAGE_POOL,
										  .owner_id = 1,
										  .segment_generation = Generation(1),
										  .state = SegmentState::SGS_SEALED,
										  .require_absent = true},
			CatalogSlotReservationRequest{.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 2U),
										  .kind = SegmentKind::SGK_INDEX_PAGE_POOL,
										  .owner_id = 17,
										  .segment_generation = Generation(1),
										  .state = SegmentState::SGS_SEALED,
										  .require_absent = true},
			CatalogSlotReservationRequest{.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 3U),
										  .kind = SegmentKind::SGK_VECTOR_RAW_POOL,
										  .owner_id = 17,
										  .segment_generation = Generation(1),
										  .state = SegmentState::SGS_SEALED,
										  .require_absent = true},
			CatalogSlotReservationRequest{.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 4U),
										  .kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL,
										  .owner_id = 17,
										  .segment_generation = Generation(1),
										  .state = SegmentState::SGS_SEALED,
										  .require_absent = true},
			CatalogSlotReservationRequest{.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 5U),
										  .kind = SegmentKind::SGK_STATE_PAGE_POOL,
										  .owner_id = 1,
										  .segment_generation = Generation(1),
										  .state = SegmentState::SGS_SEALED,
										  .require_absent = true},
	};
	const std::array pages = {
			PageLocation{PageId(90), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID), RSS_PAGE_POOL_FIRST_DATA_FRAME,
						 Generation(1)},
			PageLocation{PageId(91), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U), RSS_PAGE_POOL_FIRST_DATA_FRAME,
						 Generation(1)},
			PageLocation{PageId(92), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 2U), RSS_PAGE_POOL_FIRST_DATA_FRAME,
						 Generation(1)},
			PageLocation{PageId(93), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 3U), RSS_PAGE_POOL_FIRST_DATA_FRAME,
						 Generation(1)},
			PageLocation{PageId(94), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 4U), RSS_PAGE_POOL_FIRST_DATA_FRAME,
						 Generation(1)},
			PageLocation{PageId(95), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 5U), RSS_PAGE_POOL_FIRST_DATA_FRAME,
						 Generation(1)},
	};
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(requests, PhysicalSlotId(40), PhysicalSlotId(45),
																					 LogSequenceNumber(2))
						.has_value());
	static_cast<void>(store->upsertLocations(pages, {}, LogSequenceNumber(3)));
	const ExtentLocation extent{ExtentId(1), SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U),
								RSS_PAGE_POOL_FIRST_DATA_FRAME, 1, Generation(1)};
	static_cast<void>(store->upsertLocations({}, std::span(&extent, 1), LogSequenceNumber(4)));
	const EntityPageLocation node{1, PageId(90), 0, 1};
	const EntityPageLocation edge{1, PageId(90), 1, 1};
	static_cast<void>(
			store->upsertEntityPageLocations(std::span(&node, 1), {}, std::span(&edge, 1), {}, LogSequenceNumber(5)));
	const EntityPropertyLocation entityProperty{{EntityPropertyOwnerKind::EPOK_NODE, 1}, PageId(91), 1, 1};
	static_cast<void>(store->upsertEntityPropertyLocations(std::span(&entityProperty, 1), {}, LogSequenceNumber(6)));
	const StateLocation state{1, PageId(95), 0, 1};
	static_cast<void>(store->upsertStateLocations(std::span(&state, 1), {}, LogSequenceNumber(7)));
	const VectorLocation vector{1001, PageId(93), 0, 1};
	static_cast<void>(store->upsertVectorLocations(std::span(&vector, 1), {}, LogSequenceNumber(8)));
	const VectorManifestLocation manifest{17, PageId(94), Generation(1)};
	static_cast<void>(store->upsertVectorManifests(std::span(&manifest, 1), {}, LogSequenceNumber(9)));
	// This test deliberately publishes synthetic Page Directory entries to make
	// every metadata directory nonempty. They have no backing typed pool image,
	// so disable startup typed-pool reconciliation while constructing the one
	// real B+Tree root needed to exercise the Index Root Directory path.
	SlottedPagePool slottedPages(io, *store, SharedPagePoolOpenMode::SPPOM_READ_ONLY);
	BTreePageStore indexes(slottedPages, *store);
	const SlottedPagePoolAllocationRequest indexPageRequest{
			.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 7U),
			.pool_kind = SegmentKind::SGK_INDEX_PAGE_POOL,
			.pool_owner_id = 17,
			.generation = Generation(1),
			.page_id = PageId(100),
			.page_owner_id = 17,
			.page_owner_kind = PageOwnerKind::POK_INDEX,
	};
	const PageOwnerHeader indexOwner{
			.page_id = indexPageRequest.page_id,
			.owner_id = indexPageRequest.page_owner_id,
			.kind = indexPageRequest.page_owner_kind,
			.payload_bytes = RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
			.generation = indexPageRequest.generation,
			.committed_lsn = LogSequenceNumber(10),
	};
	const BTreePageHeader indexHeader{
			.index_id = indexPageRequest.page_owner_id,
			.page_id = indexPageRequest.page_id,
			.generation = indexPageRequest.generation,
	};
	ASSERT_TRUE(indexes.allocate(indexPageRequest, BTreePage::create(indexOwner, indexHeader), PhysicalSlotId(48),
								 PhysicalSlotId(48), LogSequenceNumber(10))
						.has_value());
	const IndexRootLocation indexRoot{17, indexPageRequest.page_id, indexPageRequest.generation};
	static_cast<void>(indexes.publishRoots(std::span(&indexRoot, 1), {}, LogSequenceNumber(11)));
	const CatalogSlotReservationRequest retiredRequest = reservationRequest(100U, 1U);
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(retiredRequest, PhysicalSlotId(47), PhysicalSlotId(47),
														LogSequenceNumber(12))
						.has_value());
	static_cast<void>(store->retireCatalogSegment(retiredRequest.segment_id, retiredRequest.segment_generation,
												  LogSequenceNumber(13)));

	const MetadataPagePoolEvacuation evacuation =
			store->evacuateSparseMetadataPagePool(LogSequenceNumber(14), RSS_PAGE_POOL_DATA_FRAME_COUNT);
	ASSERT_TRUE(evacuation.source_pool_slot.isValid());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(store->findCatalog(snapshot, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(40));
	EXPECT_EQ(store->findPageLocation(snapshot, PageId(91)), pages[1]);
	EXPECT_EQ(store->findExtentLocation(snapshot, extent.extent_id), extent);
	EXPECT_EQ(store->findNodePageLocation(snapshot, node.entity_id), node);
	EXPECT_EQ(store->findEdgePageLocation(snapshot, edge.entity_id), edge);
	EXPECT_EQ(store->findEntityPropertyLocation(snapshot, entityProperty.key), entityProperty);
	EXPECT_EQ(store->findStateLocation(snapshot, state.state_id), state);
	EXPECT_EQ(store->findVectorLocation(snapshot, vector.vector_id), vector);
	EXPECT_EQ(store->findVectorManifest(snapshot, manifest.index_id), manifest);
	EXPECT_EQ(store->findIndexRoot(snapshot, indexRoot.index_id), indexRoot);
	EXPECT_TRUE(store->findRetireQueueEntryOptional(snapshot, PhysicalSlotId(47)).has_value());
}

TEST_F(RelocatableMetadataStoreTest, ReopeningBeforeMetadataPoolReclamationPublicationRestoresItsOwnership) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	std::vector<MetadataSnapshotPin> retainedSnapshots = createReclaimableMetadataPool(*store);
	const uint64_t poolsBeforeReclamation = store->pagePoolStats().pools;
	for (MetadataSnapshotPin &snapshot: retainedSnapshots) {
		snapshot.release();
	}

	EXPECT_THROW(static_cast<void>(store->reclaimEmptyMetadataPagePools(
						 LogSequenceNumber(7),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	uint64_t sealedPools = 0U;
	reopened->visitCatalogEntries(snapshot, [&reopened, &snapshot, &sealedPools](const CatalogEntry &entry) {
		if (entry.kind != SegmentKind::SGK_METADATA_PAGE_POOL) {
			return;
		}
		EXPECT_EQ(entry.state, SegmentState::SGS_SEALED);
		EXPECT_TRUE(reopened->isPhysicalSlotAllocated(snapshot, entry.physical_slot));
		++sealedPools;
	});
	EXPECT_GE(sealedPools, 2U);
	EXPECT_EQ(reopened->pagePoolStats().pools, poolsBeforeReclamation);
}

TEST_F(RelocatableMetadataStoreTest, ReopeningAfterMetadataPoolReclamationPublicationKeepsTheSlotFree) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	std::vector<MetadataSnapshotPin> retainedSnapshots = createReclaimableMetadataPool(*store);
	const uint64_t poolsBeforeReclamation = store->pagePoolStats().pools;
	for (MetadataSnapshotPin &snapshot: retainedSnapshots) {
		snapshot.release();
	}

	EXPECT_THROW(
			static_cast<void>(store->reclaimEmptyMetadataPagePools(
					LogSequenceNumber(7), withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
			BootstrapPublishInterrupted);

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	uint64_t sealedPools = 0U;
	reopened->visitCatalogEntries(snapshot, [&sealedPools](const CatalogEntry &entry) {
		if (entry.kind != SegmentKind::SGK_METADATA_PAGE_POOL) {
			return;
		}
		EXPECT_EQ(entry.state, SegmentState::SGS_SEALED);
		++sealedPools;
	});
	EXPECT_GE(sealedPools, 1U);
	EXPECT_LT(reopened->pagePoolStats().pools, poolsBeforeReclamation);
}

TEST_F(RelocatableMetadataStoreTest, DiscardsUnpublishedAdditionalCatalogPoolAndReopensThePriorGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	EXPECT_THROW(static_cast<void>(store->upsertCatalog(
						 entriesThatRequireAnotherMetadataPool(), LogSequenceNumber(2),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(store->pagePoolStats().pools, 2U);
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 2U);

	auto reopened = RelocatableMetadataStore::open(io);
	EXPECT_EQ(reopened->pagePoolStats().pools, 2U);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_FALSE(snapshot.superblock().catalog_root.isValid());
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(1)));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(2)));
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(3)));
}

TEST_F(RelocatableMetadataStoreTest, KeepsReservedUserSlotSeparateFromExistingCatalogMetadataPools) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	static_cast<void>(store->upsertCatalog(catalogEntriesForLeafCount(28U), LogSequenceNumber(2)));
	EXPECT_EQ(store->pagePoolStats().pools, 3U);
	EXPECT_GE(store->pagePoolStats().allocated_pages, 31U);

	const std::optional<AllocatorReservation> reservation =
			store->reserveFirstFreeSlot(PhysicalSlotId(4), PhysicalSlotId(4), LogSequenceNumber(3));
	ASSERT_TRUE(reservation.has_value());
	EXPECT_EQ(reservation->physical_slot, PhysicalSlotId(4));
	EXPECT_EQ(store->pagePoolStats().pools, 3U);

	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_TRUE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(4)));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(5)));
	snapshot.release();

	auto reopened = RelocatableMetadataStore::open(io);
	EXPECT_EQ(reopened->pagePoolStats().pools, 3U);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(reopenedSnapshot, PhysicalSlotId(4)));
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(reopenedSnapshot, PhysicalSlotId(5)));
}

TEST_F(RelocatableMetadataStoreTest, KeepsCombinedReservationDataSlotSeparateFromCatalogMetadataPools) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	static_cast<void>(store->upsertCatalog(catalogEntriesForLeafCount(28U), LogSequenceNumber(2)));
	EXPECT_EQ(store->pagePoolStats().pools, 3U);
	EXPECT_GE(store->pagePoolStats().allocated_pages, 31U);

	const CatalogSlotReservationRequest request = reservationRequest(10000U, 1);
	const std::optional<CatalogSlotReservation> reservation = store->reserveFirstFreeAndUpsertCatalog(
			request, PhysicalSlotId(4), PhysicalSlotId(4), LogSequenceNumber(3));
	ASSERT_TRUE(reservation.has_value());
	EXPECT_EQ(reservation->entry.physical_slot, PhysicalSlotId(4));
	EXPECT_EQ(store->pagePoolStats().pools, 3U);

	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(store->findCatalog(snapshot, request.segment_id).physical_slot, PhysicalSlotId(4));
	EXPECT_TRUE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(4)));
	// Catalog metadata remains in its existing pool while the typed-data
	// reservation claims only the requested physical slot.
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(5)));
	snapshot.release();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin reopenedSnapshot = reopened->openSnapshot();
	EXPECT_EQ(reopened->findCatalog(reopenedSnapshot, request.segment_id).physical_slot, PhysicalSlotId(4));
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(reopenedSnapshot, PhysicalSlotId(5)));
}

TEST_F(RelocatableMetadataStoreTest,
	   RecoversAReservedTailDataSlotAndItsFollowingMetadataPoolAcrossPublicationBoundaries) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	static_cast<void>(store->upsertCatalog(catalogEntriesForLeafCount(30U), LogSequenceNumber(2U)));
	ASSERT_EQ(store->pagePoolStats().pools, 3U);
	std::vector<CatalogSlotReservationRequest> requests;
	requests.reserve(30U);
	for (uint64_t ordinal = 0U; ordinal < 30U; ++ordinal) {
		requests.push_back(reservationRequest(10000U + ordinal * RSS_CATALOG_PAGE_MAX_ENTRIES, 1U));
	}
	const CatalogSlotReservationRequest request = requests.front();

	EXPECT_THROW(static_cast<void>(store->reserveFirstFreeAndUpsertCatalog(
						 requests, PhysicalSlotId(4U), PhysicalSlotId(33U), LogSequenceNumber(3U),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	MetadataSnapshotPin before = store->openSnapshot();
	EXPECT_EQ(before.superblock().generation, Generation(2U));
	EXPECT_THROW(static_cast<void>(store->findCatalog(before, request.segment_id)), FormatException);
	EXPECT_FALSE(store->isPhysicalSlotAllocated(before, PhysicalSlotId(4U)));
	before.release();
	EXPECT_EQ(store->pagePoolStats().pools, 3U);

	store.reset();
	auto interrupted = RelocatableMetadataStore::open(io);
	EXPECT_THROW(static_cast<void>(interrupted->reserveFirstFreeAndUpsertCatalog(
						 requests, PhysicalSlotId(4U), PhysicalSlotId(33U), LogSequenceNumber(3U),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	interrupted.reset();

	auto recovered = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin after = recovered->openSnapshot();
	EXPECT_EQ(after.superblock().generation, Generation(3U));
	EXPECT_EQ(recovered->findCatalog(after, request.segment_id).physical_slot, PhysicalSlotId(4U));
	for (uint64_t slot = 4U; slot <= 33U; ++slot) {
		EXPECT_TRUE(recovered->isPhysicalSlotAllocated(after, PhysicalSlotId(slot)));
	}
	std::vector<PhysicalSlotId> metadataPoolSlots;
	recovered->visitCatalogEntries(after, [&metadataPoolSlots](const CatalogEntry &entry) {
		if (entry.kind == SegmentKind::SGK_METADATA_PAGE_POOL) {
			metadataPoolSlots.push_back(entry.physical_slot);
		}
	});
	EXPECT_TRUE(std::any_of(metadataPoolSlots.begin(), metadataPoolSlots.end(),
							[](PhysicalSlotId slot) { return slot.value() > 33U; }));
	after.release();
	EXPECT_GE(recovered->pagePoolStats().pools, 4U);
}

TEST_F(RelocatableMetadataStoreTest, OpenIgnoresAnUnownedEmptyCatalogPoolLeftBeforePublication) {
	static_cast<void>(RelocatableMetadataStore::initializeNew(io, superblock(1, 1)));
	SuperblockFile bootstrap(io);
	const Superblock current = bootstrap.recover();
	MetadataPagePoolAllocator allocator(io, current);
	for (uint32_t index = 0; index + 2U < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++index) {
		static_cast<void>(allocator.allocatePage(Generation(1)));
	}
	const PhysicalPageRef orphan = allocator.allocatePage(Generation(1));
	ASSERT_EQ(orphan.physical_slot, PhysicalSlotId(3));
	allocator.abandonPage(orphan);
	io->sync();

	auto reopened = RelocatableMetadataStore::open(io);
	EXPECT_EQ(reopened->pagePoolStats().pools, 2U);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(1)));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(2)));
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(3)));
}

TEST_F(RelocatableMetadataStoreTest, ReadOnlyOpenIgnoresAnUnreachablePoolWithoutRepairingIt) {
	static_cast<void>(RelocatableMetadataStore::initializeNew(io, superblock(1, 1)));
	SuperblockFile bootstrap(io);
	const Superblock current = bootstrap.recover();
	MetadataPagePoolAllocator allocator(io, current);
	for (uint32_t index = 0; index + 2U < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++index) {
		static_cast<void>(allocator.allocatePage(Generation(1)));
	}
	const PhysicalPageRef orphan = allocator.allocatePage(Generation(1));
	ASSERT_EQ(orphan.physical_slot, PhysicalSlotId(3));
	allocator.abandonPage(orphan);
	io->sync();

	const uint64_t fileBytes = io->fileSize();
	ASSERT_LE(fileBytes, std::numeric_limits<size_t>::max());
	std::vector<std::byte> before(static_cast<size_t>(fileBytes));
	ASSERT_EQ(io->readAt(0, before.data(), before.size()), before.size());
	auto reopened = RelocatableMetadataStore::openReadOnly(io);
	EXPECT_EQ(reopened->pagePoolStats().pools, 2U);
	std::vector<std::byte> after(static_cast<size_t>(fileBytes));
	ASSERT_EQ(io->fileSize(), fileBytes);
	ASSERT_EQ(io->readAt(0, after.data(), after.size()), after.size());
	EXPECT_EQ(after, before);
}

TEST_F(RelocatableMetadataStoreTest, OpenRejectsReachableCatalogPoolWithoutFreeSpaceOwnership) {
	static_cast<void>(RelocatableMetadataStore::initializeNew(io, superblock(1, 1)));
	SuperblockFile bootstrap(io);
	const Superblock current = bootstrap.recover();
	MetadataPageFile pages(io, current);
	MetadataPagePoolAllocator allocator(io, current);
	for (uint32_t index = 0; index + 2U < RSS_PAGE_POOL_DATA_FRAME_COUNT; ++index) {
		static_cast<void>(allocator.allocatePage(Generation(1)));
	}
	CatalogTree tree(pages, allocator);
	static_cast<void>(tree.upsertAndPublish(bootstrap, current, std::vector{entry(100, 2, 2)}, LogSequenceNumber(2)));
	EXPECT_THROW(static_cast<void>(RelocatableMetadataStore::open(io)), FormatException);
}

TEST_F(RelocatableMetadataStoreTest, AtomicallyPublishesCatalogMappingAndAllocatorReservation) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const std::optional<CatalogSlotReservation> reservation =
			store->reserveFirstFreeAndUpsertCatalog(reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1),
													PhysicalSlotId(40), PhysicalSlotId(41), LogSequenceNumber(2));
	ASSERT_TRUE(reservation.has_value());
	EXPECT_EQ(reservation->superblock.generation, Generation(2));
	EXPECT_EQ(reservation->superblock.checkpoint_lsn, LogSequenceNumber(2));
	EXPECT_TRUE(reservation->superblock.catalog_root.isValid());
	EXPECT_TRUE(reservation->superblock.allocator_root.isValid());
	EXPECT_EQ(reservation->entry.physical_slot, PhysicalSlotId(40));
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 4U);

	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(2));
	EXPECT_EQ(store->findCatalog(snapshot, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(40));
	EXPECT_TRUE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
	snapshot.release();

	EXPECT_FALSE(store->reserveFirstFreeAndUpsertCatalog(reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U, 1),
														 PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(3))
						 .has_value());
	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock().generation, Generation(2));
}

TEST_F(RelocatableMetadataStoreTest, AtomicallyPublishesMultipleCatalogMappingsAndSlotReservations) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const std::vector<CatalogSlotReservationRequest> requests =
			reservationRequests(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 3);
	const std::optional<CatalogSlotReservations> reservations =
			store->reserveFirstFreeAndUpsertCatalog(std::span<const CatalogSlotReservationRequest>(requests),
													PhysicalSlotId(40), PhysicalSlotId(44), LogSequenceNumber(2));
	ASSERT_TRUE(reservations.has_value());
	EXPECT_EQ(reservations->superblock.generation, Generation(2));
	EXPECT_EQ(reservations->superblock.checkpoint_lsn, LogSequenceNumber(2));
	EXPECT_TRUE(reservations->superblock.catalog_root.isValid());
	EXPECT_TRUE(reservations->superblock.allocator_root.isValid());
	ASSERT_EQ(reservations->entries.size(), 3U);
	for (uint64_t index = 0; index < reservations->entries.size(); ++index) {
		EXPECT_EQ(reservations->entries[index].segment_id, SegmentId(index + RSS_FIRST_ALLOCATABLE_SEGMENT_ID));
		EXPECT_EQ(reservations->entries[index].physical_slot, PhysicalSlotId(40U + index));
	}

	MetadataSnapshotPin snapshot = store->openSnapshot();
	for (uint64_t index = 0; index < 3U; ++index) {
		EXPECT_EQ(store->findCatalog(snapshot, SegmentId(index + RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
				  PhysicalSlotId(40U + index));
		EXPECT_TRUE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40U + index)));
	}
}

TEST_F(RelocatableMetadataStoreTest, RunsPreparedDataWriteOnlyInsideTheUnpublishedReservationBoundary) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const CatalogSlotReservationRequest firstRequest = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1);
	bool prepared = false;
	CatalogSlotReservationPreparer preparer = [&](std::span<const CatalogEntry> entries) {
		prepared = true;
		ASSERT_EQ(entries.size(), 1U);
		EXPECT_EQ(entries.front().segment_id, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID));
		EXPECT_EQ(entries.front().physical_slot, PhysicalSlotId(40));
		EXPECT_EQ(entries.front().committed_lsn, LogSequenceNumber(2));
	};
	const std::optional<CatalogSlotReservations> reservation = store->reserveFirstFreeAndUpsertCatalog(
			std::span(&firstRequest, 1), PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2), preparer);
	ASSERT_TRUE(reservation.has_value());
	EXPECT_TRUE(prepared);

	const CatalogSlotReservationRequest secondRequest = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U, 1);
	CatalogSlotReservationPreparer empty;
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalog(std::span(&secondRequest, 1),
																		  PhysicalSlotId(41), PhysicalSlotId(41),
																		  LogSequenceNumber(3), empty));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, DiscardsPreparedReservationWhenTheDataWriteFails) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1);
	CatalogSlotReservationPreparer failingPreparer = [](std::span<const CatalogEntry>) {
		throw std::runtime_error("injected physical data write failure");
	};
	EXPECT_THROW(static_cast<void>(store->reserveFirstFreeAndUpsertCatalog(std::span(&request, 1), PhysicalSlotId(40),
																		   PhysicalSlotId(40), LogSequenceNumber(2),
																		   failingPreparer)),
				 std::runtime_error);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
}

TEST_F(RelocatableMetadataStoreTest, RejectsRelocationWithoutADataCopyPreparationCallback) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotRelocationPreparer empty;
	expectFormatError(
			[&] {
				static_cast<void>(store->relocateCatalogSegment(SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID),
																Generation(1), PhysicalSlotId(40), PhysicalSlotId(40),
																LogSequenceNumber(2), empty));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreTest, MultiSegmentReservationHasNoPartialPublicationOrInvalidRequestState) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const std::vector<CatalogSlotReservationRequest> requests =
			reservationRequests(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 3);
	EXPECT_FALSE(store->reserveFirstFreeAndUpsertCatalog(std::span<const CatalogSlotReservationRequest>(requests),
														 PhysicalSlotId(40), PhysicalSlotId(41), LogSequenceNumber(2))
						 .has_value());
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 2U);
	MetadataSnapshotPin initialSnapshot = store->openSnapshot();
	EXPECT_EQ(initialSnapshot.superblock().generation, Generation(1));
	EXPECT_FALSE(initialSnapshot.superblock().catalog_root.isValid());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(initialSnapshot, PhysicalSlotId(40)));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(initialSnapshot, PhysicalSlotId(41)));
	initialSnapshot.release();

	const std::vector<CatalogSlotReservationRequest> unordered = {
			reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U, 1),
			reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1)};
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalog(
						std::span<const CatalogSlotReservationRequest>(unordered), PhysicalSlotId(40),
						PhysicalSlotId(41), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalog(
						std::span<const CatalogSlotReservationRequest>{}, PhysicalSlotId(40), PhysicalSlotId(41),
						LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	CatalogSlotReservationRequest invalidId = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1);
	invalidId.segment_id = SegmentId{};
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalog(std::span(&invalidId, 1), PhysicalSlotId(40),
																		  PhysicalSlotId(41), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	CatalogSlotReservationRequest bootstrapId = reservationRequest(RSS_BOOTSTRAP_METADATA_SEGMENT_ID, 1);
	CatalogSlotReservationRequest metadataPool = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1);
	metadataPool.kind = SegmentKind::SGK_METADATA_PAGE_POOL;
	for (const CatalogSlotReservationRequest &reserved: {bootstrapId, metadataPool}) {
		io->resetStats();
		expectFormatError(
				[&] {
					static_cast<void>(store->reserveFirstFreeAndUpsertCatalog(
							std::span(&reserved, 1), PhysicalSlotId(40), PhysicalSlotId(41), LogSequenceNumber(2)));
				},
				FormatError::FERR_INVALID_HEADER);
		EXPECT_EQ(io->stats().write_calls, 0U);
		EXPECT_EQ(io->stats().write_bytes, 0U);
	}
}

TEST_F(RelocatableMetadataStoreTest, AtomicReservationDiscardsPrePublicationWorkAndRetainsDurableInterruptedWork) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	EXPECT_THROW(static_cast<void>(store->reserveFirstFreeAndUpsertCatalog(
						 reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1), PhysicalSlotId(40),
						 PhysicalSlotId(40), LogSequenceNumber(2),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 2U);
	MetadataSnapshotPin initialSnapshot = store->openSnapshot();
	EXPECT_FALSE(initialSnapshot.superblock().catalog_root.isValid());
	EXPECT_TRUE(initialSnapshot.superblock().allocator_root.isValid());
	initialSnapshot.release();

	EXPECT_THROW(
			static_cast<void>(store->reserveFirstFreeAndUpsertCatalog(
					reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1), PhysicalSlotId(40), PhysicalSlotId(40),
					LogSequenceNumber(2), withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
			BootstrapPublishInterrupted);
	auto recovered = RelocatableMetadataStore::open(io);
	// A crash before the publication root leaves fresh COW metadata frames
	// unreachable. They are safe but become reclaimable only by the Stage 3
	// retire/reclaimer path, so this test checks visibility rather than layout.
	EXPECT_GE(recovered->pagePoolStats().allocated_pages, 4U);
	MetadataSnapshotPin recoveredSnapshot = recovered->openSnapshot();
	EXPECT_EQ(recoveredSnapshot.superblock().generation, Generation(2));
	EXPECT_EQ(recovered->findCatalog(recoveredSnapshot, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID)).physical_slot,
			  PhysicalSlotId(40));
	EXPECT_TRUE(recovered->isPhysicalSlotAllocated(recoveredSnapshot, PhysicalSlotId(40)));
}

TEST_F(RelocatableMetadataStoreTest, LeasesLogicalGraphIdsFromDurableRangesWithoutReuse) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const NativeGraphLogicalIdReservation first = store->acquireNativeGraphLogicalIds(2U, 0U, 4U);
	ASSERT_EQ(first.node_ranges.size(), 1U);
	EXPECT_EQ(first.node_ranges.front(), (NativeGraphLogicalIdLeaseRange{1U, 2U}));
	EXPECT_TRUE(first.edge_ranges.empty());
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation(first));
	EXPECT_FALSE(store->claimNativeGraphLogicalIdReservation(first));

	const NativeGraphLogicalIdReservation second = store->acquireNativeGraphLogicalIds(2U, 1U, 4U);
	ASSERT_EQ(second.node_ranges.size(), 1U);
	ASSERT_EQ(second.edge_ranges.size(), 1U);
	EXPECT_EQ(second.node_ranges.front(), (NativeGraphLogicalIdLeaseRange{3U, 2U}));
	EXPECT_EQ(second.edge_ranges.front(), (NativeGraphLogicalIdLeaseRange{1U, 1U}));
	store->abandonNativeGraphLogicalIdReservation(second);
	EXPECT_FALSE(store->claimNativeGraphLogicalIdReservation(second));

	const NativeGraphLogicalIdLease direct = store->reserveNativeGraphLogicalIds(1U, 1U);
	EXPECT_EQ(direct.nodes, (NativeGraphLogicalIdLeaseRange{5U, 1U}));
	EXPECT_EQ(direct.edges, (NativeGraphLogicalIdLeaseRange{5U, 1U}));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().next_logical_node_id, 6U);
	EXPECT_EQ(snapshot.superblock().next_logical_edge_id, 6U);
	snapshot.release();
}
