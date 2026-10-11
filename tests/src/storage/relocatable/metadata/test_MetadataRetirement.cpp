/**
 * @file test_MetadataRetirement.cpp
 * @brief Direct validation for metadata physical-slot retirement.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/metadata/CatalogSegmentRetirementCoordinator.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataRetirement.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"

namespace {

	class MetadataRetirementComponents final {
	public:
		MetadataRetirementComponents(std::shared_ptr<StorageIO> storage, const Superblock &current) :
			storage_(std::move(storage)), bootstrap(storage_), pages(storage_, current),
			pools(storage_, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER), catalog(pages, pools),
			locations(pages, pools), allocator(pages, pools), snapshots(bootstrap), retirement(catalog, locations, snapshots),
			segmentRetirement(catalog, allocator, locations, retirement) {}

		std::shared_ptr<StorageIO> storage_;
		SuperblockFile bootstrap;
		MetadataPageFile pages;
		MetadataPagePoolAllocator pools;
		CatalogTree catalog;
		LocationDirectory locations;
		AllocatorDirectory allocator;
		MetadataSnapshotRegistry snapshots;
		MetadataRetirement retirement;
		CatalogSegmentRetirementCoordinator segmentRetirement;
	};

	[[nodiscard]] Superblock initializeAndRecoverRetirementStore(const std::shared_ptr<StorageIO> &io) {
		static_cast<void>(RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U)));
		SuperblockFile bootstrap(io);
		return bootstrap.recover();
	}

	[[nodiscard]] Superblock publishCatalogMappingForRetirement(const std::shared_ptr<StorageIO> &io) {
		auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		const CatalogEntry mapping = entry(40U, 1U, 2U);
		static_cast<void>(store->upsertCatalog(std::span(&mapping, 1U), LogSequenceNumber(2U)));
		MetadataSnapshotPin snapshot = store->openSnapshot();
		const Superblock current = snapshot.superblock();
		snapshot.release();
		return current;
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest, MetadataRetirementDirectlyValidatesQueueInsertionOrderingAndDurableDuplicates) {
	const Superblock initial = initializeAndRecoverRetirementStore(io);
	MetadataRetirementComponents components(io, initial);
	MetadataWriteBatch firstBatch(components.pools, initial, LogSequenceNumber(2U));
	const RetireQueueEntry retired{PhysicalSlotId(40U), SegmentId(3U), firstBatch.generation(),
									RetireQueueDisposition::RQD_RETIRED_SEGMENT};
	EXPECT_NO_THROW(components.retirement.prepareQueueInsertions(firstBatch, std::span(&retired, 1U)));
	firstBatch.publish(components.bootstrap);
	const Superblock published = firstBatch.candidate();

	MetadataWriteBatch duplicateBatch(components.pools, published, LogSequenceNumber(3U));
	RetireQueueEntry duplicate = retired;
	duplicate.retired_generation = duplicateBatch.generation();
	try {
		components.retirement.prepareQueueInsertions(duplicateBatch, std::span(&duplicate, 1U));
		FAIL() << "Expected duplicate retirement slot rejection";
	} catch (const FormatException &error) {
		EXPECT_EQ(error.error(), FormatError::FERR_INVALID_STATE) << error.what();
	}

	MetadataWriteBatch malformedBatch(components.pools, published, LogSequenceNumber(3U));
	const RetireQueueEntry invalidSlot{{}, SegmentId(4U), malformedBatch.generation(),
									 RetireQueueDisposition::RQD_RETIRED_SEGMENT};
	expectFormatError([&] { components.retirement.prepareQueueInsertions(malformedBatch, std::span(&invalidSlot, 1U)); },
					  FormatError::FERR_INVALID_STATE);
	const RetireQueueEntry invalidDisposition{PhysicalSlotId(41U), SegmentId(4U), malformedBatch.generation(),
										 static_cast<RetireQueueDisposition>(0U)};
	expectFormatError(
			[&] { components.retirement.prepareQueueInsertions(malformedBatch, std::span(&invalidDisposition, 1U)); },
			FormatError::FERR_INVALID_STATE);
	EXPECT_NO_THROW(components.retirement.prepareQueueInsertions(malformedBatch, {}));
}

TEST_F(RelocatableMetadataStoreTest, MetadataRetirementUsesSnapshotGenerationAndCatalogLocationForVisibility) {
	const Superblock current = publishCatalogMappingForRetirement(io);
	MetadataRetirementComponents components(io, current);
	MetadataSnapshotPin snapshot = components.snapshots.openSnapshot();
	MetadataWriteGuard writer = components.snapshots.beginWrite();
	const RetireQueueEntry beforeSnapshot{PhysicalSlotId(40U), SegmentId(3U), current.generation,
											 RetireQueueDisposition::RQD_RELOCATED_SOURCE};
	EXPECT_TRUE(components.retirement.isVisibleToActiveSnapshot(writer, beforeSnapshot));

	const RetireQueueEntry mappedSource{PhysicalSlotId(40U), SegmentId(3U), Generation(1U),
											  RetireQueueDisposition::RQD_RELOCATED_SOURCE};
	EXPECT_TRUE(components.retirement.isVisibleToActiveSnapshot(writer, mappedSource));
	const RetireQueueEntry relocatedSource{PhysicalSlotId(41U), SegmentId(3U), Generation(1U),
												RetireQueueDisposition::RQD_RELOCATED_SOURCE};
	EXPECT_FALSE(components.retirement.isVisibleToActiveSnapshot(writer, relocatedSource));
	const RetireQueueEntry absentSource{PhysicalSlotId(40U), SegmentId(99U), Generation(1U),
											 RetireQueueDisposition::RQD_RELOCATED_SOURCE};
	EXPECT_FALSE(components.retirement.isVisibleToActiveSnapshot(writer, absentSource));
	snapshot.release();
	EXPECT_FALSE(components.retirement.isVisibleToActiveSnapshot(writer, mappedSource));
}

TEST_F(RelocatableMetadataStoreTest, CatalogSegmentRetirementCoordinatorRejectsInvalidLifecycleTransitions) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const PageLocation page{PageId(81U), request.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, request.segment_generation};
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(
							std::span(&request, 1U), std::span(&page, 1U), {}, PhysicalSlotId(40U), PhysicalSlotId(40U),
							LogSequenceNumber(2U),
							[](std::span<const CatalogEntry>) {})
							.has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();
	store.reset();

	MetadataRetirementComponents components(io, current);
	MetadataWriteBatch invalidBatch(components.pools, current, LogSequenceNumber(3U));
	expectFormatError(
			[&] {
				components.segmentRetirement.prepareRetirement(
						invalidBatch, request.segment_id, Generation(request.segment_generation.value() + 1U));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { components.segmentRetirement.prepareRetirement(invalidBatch, request.segment_id, request.segment_generation); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(components.segmentRetirement.validateRetiredRelease(
						current, request.segment_id, request.segment_generation));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogSegmentRetirementCoordinatorRejectsAQueuedSlotThatNoLongerMatchesCatalog) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	request.require_absent = true;
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
							.has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();
	store.reset();

	MetadataRetirementComponents components(io, current);
	MetadataWriteBatch retirementBatch(components.pools, current, LogSequenceNumber(3U));
	ASSERT_NO_THROW(components.segmentRetirement.prepareRetirement(
				retirementBatch, request.segment_id, request.segment_generation));
	retirementBatch.publish(components.bootstrap);
	const Superblock retired = retirementBatch.candidate();
	const RetireQueueEntry queued = components.segmentRetirement.validateRetiredRelease(
			retired, request.segment_id, request.segment_generation);
	expectFormatError(
			[&] {
				static_cast<void>(components.segmentRetirement.validateRetiredRelease(
						retired, request.segment_id, Generation(request.segment_generation.value() + 1U)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataWriteBatch releaseBatch(components.pools, retired, LogSequenceNumber(4U));
	RetireQueueEntry mismatched = queued;
	mismatched.physical_slot = PhysicalSlotId(41U);
	expectFormatError([&] { components.segmentRetirement.prepareRetiredRelease(releaseBatch, mismatched); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogSegmentRetirementCoordinatorRejectsStaleRetirementGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	request.require_absent = true;
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U))
							.has_value());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();
	store.reset();

	MetadataRetirementComponents components(io, current);
	MetadataWriteBatch retirementBatch(components.pools, current, LogSequenceNumber(3U));
	components.segmentRetirement.prepareRetirement(retirementBatch, request.segment_id, request.segment_generation);
	retirementBatch.publish(components.bootstrap);
	const Superblock retired = retirementBatch.candidate();
	const RetireQueueEntry queued = components.segmentRetirement.validateRetiredRelease(
			retired, request.segment_id, request.segment_generation);

	// The queue generation is the publication fence that made the source
	// unreachable. A smaller value must never permit a direct slot release.
	CatalogEntry newerRetired = components.catalog.find(retired, request.segment_id);
	newerRetired.generation = Generation(2U);
	newerRetired.committed_lsn = LogSequenceNumber(4U);
	RetireQueueEntry stale = queued;
	stale.retired_generation = Generation(1U);
	MetadataWriteBatch corruptBatch(components.pools, retired, LogSequenceNumber(4U));
	components.catalog.prepareUpsert(corruptBatch, std::span(&newerRetired, 1U));
	components.locations.prepareReplaceRetireQueue(corruptBatch, std::span(&stale, 1U), {});
	corruptBatch.publish(components.bootstrap);
	const Superblock corrupted = corruptBatch.candidate();

	expectFormatError(
			[&] {
				static_cast<void>(components.segmentRetirement.validateRetiredRelease(
						corrupted, request.segment_id, newerRetired.generation));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataWriteBatch releaseBatch(components.pools, corrupted, LogSequenceNumber(5U));
	expectFormatError([&] { components.segmentRetirement.prepareRetiredRelease(releaseBatch, stale); },
					  FormatError::FERR_INVALID_STATE);
}
