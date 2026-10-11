/**
 * @file test_RelocatableMetadataStore_Boundaries.cpp
 * @brief Crash recovery, snapshot ownership, and typed-directory boundary tests.
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

TEST_F(RelocatableMetadataStoreTest, RecoversPriorGenerationWhenCrashPrecedesInactiveSuperblockPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	EXPECT_THROW(static_cast<void>(store->reserveFirstFreeAndUpsertCatalog(
						 reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1), PhysicalSlotId(40),
						 PhysicalSlotId(40), LogSequenceNumber(2),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	SuperblockFile bootstrap(io);
	EXPECT_EQ(bootstrap.recover().generation, Generation(1));

	auto recovered = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = recovered->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(1));
	EXPECT_FALSE(snapshot.superblock().catalog_root.isValid());
	EXPECT_FALSE(recovered->isPhysicalSlotAllocated(snapshot, PhysicalSlotId(40)));
}

TEST_F(RelocatableMetadataStoreTest, RecoversEveryGenerationWhoseInactiveSuperblockIsDurable) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	EXPECT_THROW(static_cast<void>(store->setPhysicalSlotAllocated(
						 PhysicalSlotId(50), true, LogSequenceNumber(2),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	auto recovered = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin generationTwo = recovered->openSnapshot();
	EXPECT_EQ(generationTwo.superblock().generation, Generation(2));
	EXPECT_TRUE(recovered->isPhysicalSlotAllocated(generationTwo, PhysicalSlotId(50)));
	generationTwo.release();

	EXPECT_THROW(static_cast<void>(recovered->setPhysicalSlotAllocated(
						 PhysicalSlotId(51), true, LogSequenceNumber(3),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(SuperblockFile(io).recover().generation, Generation(3));
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin generationThree = reopened->openSnapshot();
	EXPECT_EQ(generationThree.superblock().generation, Generation(3));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(generationThree, PhysicalSlotId(50)));
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(generationThree, PhysicalSlotId(51)));
}

TEST_F(RelocatableMetadataStoreTest, RequiresInitializedDataAndNonNullStorage) {
	expectFormatError([&] { static_cast<void>(RelocatableMetadataStore::open(io)); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_THROW(static_cast<void>(RelocatableMetadataStore::initializeNew(nullptr, superblock(1, 1))),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(RelocatableMetadataStore::open(nullptr)), std::invalid_argument);
}

TEST_F(RelocatableMetadataStoreTest, RefusesNonEmptyIncompleteAndAlreadyInitializedDataFiles) {
	const std::array<std::byte, 64U> nonNativeBytes{};
	io->writeAt(0, nonNativeBytes.data(), nonNativeBytes.size());
	io->sync();
	expectFormatError([&] { static_cast<void>(RelocatableMetadataStore::initializeNew(io, superblock(1, 1))); },
					  FormatError::FERR_INVALID_STATE);

	io.reset();
	file->close();
	file = std::make_shared<std::fstream>(testFilePath,
										  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(file->is_open());
	io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	const std::array<std::byte, sizeof(uint32_t)> partialMagic = {std::byte{0x5A}, std::byte{0x59}, std::byte{0x58},
																  std::byte{0x53}};
	io->writeAt(0, partialMagic.data(), partialMagic.size());
	io->sync();
	expectFormatError([&] { static_cast<void>(RelocatableMetadataStore::open(io)); }, FormatError::FERR_INVALID_HEADER);

	io.reset();
	file->close();
	file = std::make_shared<std::fstream>(testFilePath,
										  std::ios::in | std::ios::out | std::ios::binary | std::ios::trunc);
	ASSERT_TRUE(file->is_open());
	io = std::make_shared<StorageIO>(file, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
	auto initialized = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	initialized.reset();
	expectFormatError([&] { static_cast<void>(RelocatableMetadataStore::initializeNew(io, superblock(1, 1))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, RejectsInvalidSnapshotPinsAcrossEveryDirectoryFacade) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	MetadataSnapshotPin invalid;
	expectFormatError([&] { static_cast<void>(store->findCurrentIndexPagePools(invalid)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findVectorLocation(invalid, 1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findVectorLocationOptional(invalid, 1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findVectorManifest(invalid, 1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findVectorManifestOptional(invalid, 1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findPageLocationsForSegment(invalid, SegmentId(1))); },
					  FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin active = store->openSnapshot();
	expectFormatError([&] { static_cast<void>(store->findPageLocationsForSegment(active, SegmentId{})); },
					  FormatError::FERR_INVALID_STATE);
	active.release();

	auto foreignStore = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin foreign = foreignStore->openSnapshot();
	expectFormatError([&] { static_cast<void>(store->findCurrentIndexPagePools(foreign)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findVectorLocation(foreign, 1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findVectorManifest(foreign, 1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findPageLocationsForSegment(foreign, SegmentId(1))); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, RejectsNoOpTransitionsAndDoesNotRetainFailedPublications) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	expectFormatError([&] { static_cast<void>(store->upsertVectorLocations({}, {}, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store->upsertVectorManifests({}, {}, LogSequenceNumber(2))); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(1), false, LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(3), false, LogSequenceNumber(2))); },
			FormatError::FERR_INVALID_STATE);
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 2U);
	MetadataSnapshotPin invalidSnapshot;
	expectFormatError([&] { static_cast<void>(store->findCatalog(invalidSnapshot, SegmentId(1))); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findNodePageLocation(invalidSnapshot, 1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findEdgePageLocation(invalidSnapshot, 1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findPageLocation(invalidSnapshot, PageId(1))); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findExtentLocation(invalidSnapshot, ExtentId(1))); },
					  FormatError::FERR_INVALID_STATE);
	auto otherStore = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin foreignSnapshot = otherStore->openSnapshot();
	expectFormatError([&] { static_cast<void>(store->findNodePageLocation(foreignSnapshot, 1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findEdgePageLocation(foreignSnapshot, 1)); },
					  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->isPhysicalSlotAllocated(foreignSnapshot, PhysicalSlotId(1))); },
					  FormatError::FERR_INVALID_STATE);

	EXPECT_THROW(static_cast<void>(store->upsertCatalog(
						 std::vector{entry(100, 2, 2)}, LogSequenceNumber(2),
						 withBootstrapFailpoint(BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE))),
				 BootstrapPublishInterrupted);
	EXPECT_EQ(store->pagePoolStats().allocated_pages, 2U);
}

TEST_F(RelocatableMetadataStoreTest, PublishesEveryTypedPageDirectoryReferenceThroughItsDedicatedDirectory) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	std::vector<CatalogSlotReservationRequest> requests{
			{.segment_id = SegmentId(31),
			 .kind = SegmentKind::SGK_VECTOR_RAW_POOL,
			 .owner_id = 17,
			 .segment_generation = Generation(1),
			 .state = SegmentState::SGS_SEALED,
			 .require_absent = true},
			{.segment_id = SegmentId(32),
			 .kind = SegmentKind::SGK_VECTOR_PQ_POOL,
			 .owner_id = 17,
			 .segment_generation = Generation(1),
			 .state = SegmentState::SGS_SEALED,
			 .require_absent = true},
			{.segment_id = SegmentId(33),
			 .kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL,
			 .owner_id = 17,
			 .segment_generation = Generation(1),
			 .state = SegmentState::SGS_SEALED,
			 .require_absent = true},
			{.segment_id = SegmentId(34),
			 .kind = SegmentKind::SGK_INDEX_PAGE_POOL,
			 .owner_id = 19,
			 .segment_generation = Generation(1),
			 .state = SegmentState::SGS_SEALED,
			 .require_absent = true},
	};
	const std::array pages = {
			PageLocation{PageId(91), SegmentId(31), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)},
			PageLocation{PageId(92), SegmentId(32), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)},
			PageLocation{PageId(93), SegmentId(33), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)},
			PageLocation{PageId(94), SegmentId(34), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)},
	};
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(requests, pages, {}, PhysicalSlotId(40),
																	PhysicalSlotId(43), LogSequenceNumber(2),
																	[](std::span<const CatalogEntry>) {})
						.has_value());
	const std::array vectorLocations = {
			VectorLocation{101, PageId(91), 0, 1},
			VectorLocation{102, PageId(92), 0, 1},
	};
	static_cast<void>(store->upsertVectorLocations(vectorLocations, {}, LogSequenceNumber(3)));
	const VectorManifestLocation manifest{17, PageId(93), Generation(1)};
	static_cast<void>(store->upsertVectorManifests(std::span(&manifest, 1), {}, LogSequenceNumber(4)));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(store->findVectorLocation(snapshot, 101), vectorLocations[0]);
	EXPECT_EQ(store->findVectorLocation(snapshot, 102), vectorLocations[1]);
	EXPECT_EQ(store->findVectorManifest(snapshot, manifest.index_id), manifest);
}

TEST_F(RelocatableMetadataStoreTest, ResumesVectorManifestTraversalThroughTheMetadataStoreBoundary) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const std::array requests = {
			CatalogSlotReservationRequest{
					.segment_id = SegmentId(31),
					.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL,
					.owner_id = 9U,
					.segment_generation = Generation(1),
					.state = SegmentState::SGS_SEALED,
					.require_absent = true,
			},
			CatalogSlotReservationRequest{
					.segment_id = SegmentId(32),
					.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL,
					.owner_id = 17U,
					.segment_generation = Generation(1),
					.state = SegmentState::SGS_SEALED,
					.require_absent = true,
			},
	};
	const std::array pages = {
			PageLocation{PageId(91), requests[0].segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
						 requests[0].segment_generation},
			PageLocation{PageId(92), requests[1].segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
						 requests[1].segment_generation},
	};
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(requests, pages, {}, PhysicalSlotId(40),
																	PhysicalSlotId(41), LogSequenceNumber(2),
																	[](std::span<const CatalogEntry>) {})
						.has_value());
	const std::array manifests = {
			VectorManifestLocation{9U, pages[0].page_id, Generation(1U)},
			VectorManifestLocation{17U, pages[1].page_id, Generation(1U)},
	};
	static_cast<void>(store->upsertVectorManifests(manifests, {}, LogSequenceNumber(3)));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	std::vector<VectorManifestLocation> resumed;
	size_t metadataPages = 0U;
	EXPECT_FALSE(store->visitVectorManifestLocationsAfter(
			snapshot, manifests.front().index_id, 2U,
			[&resumed](const VectorManifestLocation &location) {
				resumed.push_back(location);
				return false;
			},
			&metadataPages));
	EXPECT_EQ(resumed, (std::vector<VectorManifestLocation>{manifests.back()}));
	EXPECT_EQ(metadataPages, 2U);

	resumed.clear();
	EXPECT_TRUE(store->visitVectorManifestLocationsAfter(snapshot, manifests.back().index_id, 2U,
														 [&resumed](const VectorManifestLocation &location) {
															 resumed.push_back(location);
															 return true;
														 }));
	EXPECT_TRUE(resumed.empty());

	MetadataSnapshotPin invalidSnapshot;
	expectFormatError(
			[&] {
				static_cast<void>(store->visitVectorManifestLocationsAfter(
						invalidSnapshot, 0U, 1U, [](const VectorManifestLocation &) { return true; }));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(store->visitVectorManifestLocationsAfter(
						snapshot, 0U, 0U, [](const VectorManifestLocation &) { return true; }));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->visitVectorManifestLocationsAfter(snapshot, 0U, 1U, {})); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, RejectsInvalidLargeValueManifestResumeTraversalArguments) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	MetadataSnapshotPin invalidSnapshot;
	expectFormatError(
			[&] {
				static_cast<void>(store->visitLargeValueManifestLocationsAfter(
						invalidSnapshot, 0U, 1U, [](const LargeValueManifestLocation &) { return true; }));
			},
			FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin snapshot = store->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(store->visitLargeValueManifestLocationsAfter(
						snapshot, 0U, 0U, [](const LargeValueManifestLocation &) { return true; }));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->visitLargeValueManifestLocationsAfter(snapshot, 0U, 1U, {})); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, RejectsInvalidLargeValueOwnerResumeTraversalArguments) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	MetadataSnapshotPin invalidSnapshot;
	expectFormatError(
			[&] {
				static_cast<void>(store->visitEntityPropertyLocationsAfter(
						invalidSnapshot, 0U, 1U, [](const EntityPropertyLocation &) { return true; }));
			},
			FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin snapshot = store->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(store->visitEntityPropertyLocationsAfter(
						snapshot, 0U, 0U, [](const EntityPropertyLocation &) { return true; }));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(
						store->visitStateLocationsAfter(snapshot, 0U, 0U, [](const StateLocation &) { return true; }));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, PublishesTypedDirectoryErasuresWithoutRequiringReplacementEntries) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	const std::array requests = {
			CatalogSlotReservationRequest{.segment_id = SegmentId(31),
										  .kind = SegmentKind::SGK_VECTOR_RAW_POOL,
										  .owner_id = 17,
										  .segment_generation = Generation(1),
										  .state = SegmentState::SGS_SEALED,
										  .require_absent = true},
			CatalogSlotReservationRequest{.segment_id = SegmentId(32),
										  .kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL,
										  .owner_id = 17,
										  .segment_generation = Generation(1),
										  .state = SegmentState::SGS_SEALED,
										  .require_absent = true},
	};
	const std::array pages = {
			PageLocation{PageId(91), SegmentId(31), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)},
			PageLocation{PageId(92), SegmentId(32), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)},
	};
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(requests, pages, {}, PhysicalSlotId(40),
																	PhysicalSlotId(41), LogSequenceNumber(2),
																	[](std::span<const CatalogEntry>) {})
						.has_value());
	const VectorLocation vector{101, PageId(91), 0, 1};
	static_cast<void>(store->upsertVectorLocations(std::span(&vector, 1), {}, LogSequenceNumber(3)));
	const VectorManifestLocation manifest{17, PageId(92), Generation(1)};
	static_cast<void>(store->upsertVectorManifests(std::span(&manifest, 1), {}, LogSequenceNumber(4)));
	const std::array<uint64_t, 1> erasedVectorIds = {101};
	static_cast<void>(store->upsertVectorLocations({}, erasedVectorIds, LogSequenceNumber(5)));
	const std::array<uint64_t, 1> erasedIndexIds = {17};
	static_cast<void>(store->upsertVectorManifests({}, erasedIndexIds, LogSequenceNumber(6)));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_FALSE(store->findVectorLocationOptional(snapshot, vector.vector_id).has_value());
	EXPECT_FALSE(store->findVectorManifestOptional(snapshot, manifest.index_id).has_value());
}

TEST_F(RelocatableMetadataStoreTest, RejectsVectorLocationsPointingAtNonVectorPools) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest indexPool;
	indexPool.segment_id = SegmentId(31);
	indexPool.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	indexPool.owner_id = 17;
	indexPool.segment_generation = Generation(1);
	indexPool.state = SegmentState::SGS_SEALED;
	indexPool.require_absent = true;
	const PageLocation page{PageId(91), SegmentId(31), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1)};
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(
							 std::span(&indexPool, 1), std::span(&page, 1), {}, PhysicalSlotId(40), PhysicalSlotId(40),
							 LogSequenceNumber(2), [](std::span<const CatalogEntry>) {})
						.has_value());
	const VectorLocation invalid{101, page.page_id, 0, 1};
	expectFormatError(
			[&] { static_cast<void>(store->upsertVectorLocations(std::span(&invalid, 1), {}, LogSequenceNumber(3))); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, RejectsPageDirectoryLocationsTargetingFullSegmentExtents) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40), true, LogSequenceNumber(2)));
	CatalogEntry codebookExtent = entry(40, 1, 3);
	codebookExtent.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	static_cast<void>(store->upsertCatalog(std::vector{codebookExtent}, LogSequenceNumber(3)));

	const PageLocation invalidPage{PageId(91), codebookExtent.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
								   codebookExtent.generation};
	static_cast<void>(store->upsertLocations(std::span(&invalidPage, 1), {}, LogSequenceNumber(4)));
	expectFormatError([&] { store->reconcileCurrentPools([](const CatalogEntry &, std::span<const PageLocation>) {}); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, RejectsTypedReservationMappingsWithoutTheirRequestedPages) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1, 1));
	CatalogSlotReservationRequest manifestRequest;
	manifestRequest.segment_id = SegmentId(31);
	manifestRequest.kind = SegmentKind::SGK_VECTOR_MANIFEST_POOL;
	manifestRequest.owner_id = 17;
	manifestRequest.segment_generation = Generation(1);
	manifestRequest.state = SegmentState::SGS_SEALED;
	manifestRequest.require_absent = true;
	const PageLocation manifestPage{PageId(91), manifestRequest.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
									manifestRequest.segment_generation};
	const VectorManifestLocation missingManifestPage{17, PageId(92), Generation(1)};
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndVectorManifests(
						std::span(&manifestRequest, 1), std::span(&manifestPage, 1), std::span(&missingManifestPage, 1),
						PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2),
						[](std::span<const CatalogEntry>) {}));
			},
			FormatError::FERR_INVALID_STATE);
}
