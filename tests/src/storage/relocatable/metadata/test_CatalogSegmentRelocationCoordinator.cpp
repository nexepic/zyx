/**
 * @file test_CatalogSegmentRelocationCoordinator.cpp
 * @brief Direct validation for Catalog relocation pre-publication coordination.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/CatalogSegmentRelocationCoordinator.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataRetirement.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

namespace {

	class CatalogRelocationComponents final {
	public:
		CatalogRelocationComponents(std::shared_ptr<StorageIO> storage, const Superblock &current) :
			storage_(std::move(storage)), bootstrap(storage_), pages(storage_, current),
			pools(storage_, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER), catalog(pages, pools),
			locations(pages, pools), allocator(pages, pools), snapshots(bootstrap),
			retirement(catalog, locations, snapshots), coordinator(catalog, allocator, locations, retirement) {}

		std::shared_ptr<StorageIO> storage_;
		SuperblockFile bootstrap;
		MetadataPageFile pages;
		MetadataPagePoolAllocator pools;
		CatalogTree catalog;
		LocationDirectory locations;
		AllocatorDirectory allocator;
		MetadataSnapshotRegistry snapshots;
		MetadataRetirement retirement;
		CatalogSegmentRelocationCoordinator coordinator;
	};

	[[nodiscard]] Superblock publishReservedSegment(const std::shared_ptr<StorageIO> &io,
													CatalogSlotReservationRequest request,
													std::span<const ExtentLocation> extents = {}) {
		auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		bool reserved = false;
		if (extents.empty()) {
			reserved = store->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40U), PhysicalSlotId(40U),
															   LogSequenceNumber(2U))
							   .has_value();
		} else {
			reserved = store->reserveFirstFreeAndUpsertCatalogAndLocations(
									std::span(&request, 1U), {}, extents, PhysicalSlotId(40U), PhysicalSlotId(40U),
									LogSequenceNumber(2U), [](std::span<const CatalogEntry>) {})
							   .has_value();
		}
		EXPECT_TRUE(reserved);
		if (!reserved) {
			throw std::runtime_error("could not create Catalog relocation test fixture");
		}
		MetadataSnapshotPin snapshot = store->openSnapshot();
		const Superblock current = snapshot.superblock();
		snapshot.release();
		return current;
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest, CatalogSegmentRelocationCoordinatorStagesMoveAndSourceRelease) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegment(io, request);
	CatalogRelocationComponents components(io, current);

	MetadataWriteBatch relocationBatch(components.pools, current, LogSequenceNumber(3U));
	bool copied = false;
	const std::optional<CatalogSlotRelocationPlan> plan = components.coordinator.prepareSegmentRelocation(
			relocationBatch, request.segment_id, request.segment_generation, PhysicalSlotId(41U), PhysicalSlotId(41U),
			[&copied](const CatalogEntry &source, const CatalogEntry &destination) {
				EXPECT_EQ(source.physical_slot, PhysicalSlotId(40U));
				EXPECT_EQ(destination.physical_slot, PhysicalSlotId(41U));
				EXPECT_EQ(destination.generation, Generation(2U));
				copied = true;
			});
	ASSERT_TRUE(plan.has_value());
	EXPECT_TRUE(copied);
	EXPECT_EQ(plan->source.physical_slot, PhysicalSlotId(40U));
	EXPECT_EQ(plan->destination.physical_slot, PhysicalSlotId(41U));
	EXPECT_TRUE(components.allocator.isAllocated(relocationBatch.candidate(), PhysicalSlotId(40U)));
	EXPECT_TRUE(components.allocator.isAllocated(relocationBatch.candidate(), PhysicalSlotId(41U)));
	EXPECT_EQ(components.catalog.find(relocationBatch.candidate(), request.segment_id).generation, Generation(2U));
	const std::optional<RetireQueueEntry> pending =
			components.locations.findRetireQueueEntryOptional(relocationBatch.candidate(), PhysicalSlotId(40U));
	ASSERT_TRUE(pending.has_value());
	EXPECT_EQ(pending->disposition, RetireQueueDisposition::RQD_RELOCATED_SOURCE);
	relocationBatch.publish(components.bootstrap);
	const Superblock relocated = relocationBatch.candidate();

	const RetireQueueEntry source =
			components.coordinator.validateRelocatedSourceRelease(relocated, request.segment_id, Generation(2U));
	MetadataWriteBatch releaseBatch(components.pools, relocated, LogSequenceNumber(4U));
	EXPECT_NO_THROW(components.coordinator.prepareRelocatedSourceRelease(releaseBatch, source));
	EXPECT_FALSE(components.allocator.isAllocated(releaseBatch.candidate(), PhysicalSlotId(40U)));
	EXPECT_TRUE(components.allocator.isAllocated(releaseBatch.candidate(), PhysicalSlotId(41U)));
	EXPECT_FALSE(components.locations.findRetireQueueEntryOptional(releaseBatch.candidate(), PhysicalSlotId(40U))
						 .has_value());
	EXPECT_EQ(components.catalog.find(releaseBatch.candidate(), request.segment_id).physical_slot, PhysicalSlotId(41U));
}

TEST_F(RelocatableMetadataStoreTest, CatalogSegmentRelocationCoordinatorRejectsInvalidOrPendingMoves) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegment(io, request);
	CatalogRelocationComponents components(io, current);
	MetadataWriteBatch invalidBatch(components.pools, current, LogSequenceNumber(3U));
	CatalogSlotRelocationPreparer empty;
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.prepareSegmentRelocation(
						invalidBatch, request.segment_id, request.segment_generation, PhysicalSlotId(41U),
						PhysicalSlotId(41U), empty));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.prepareSegmentRelocation(
						invalidBatch, request.segment_id, Generation(2U), PhysicalSlotId(41U), PhysicalSlotId(41U),
						[](const CatalogEntry &, const CatalogEntry &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(components.coordinator
						 .prepareSegmentRelocation(invalidBatch, request.segment_id, request.segment_generation,
												   PhysicalSlotId(40U), PhysicalSlotId(40U),
												   [](const CatalogEntry &, const CatalogEntry &) {})
						 .has_value());

	MetadataWriteBatch relocationBatch(components.pools, current, LogSequenceNumber(3U));
	ASSERT_TRUE(components.coordinator
						.prepareSegmentRelocation(relocationBatch, request.segment_id, request.segment_generation,
												  PhysicalSlotId(41U), PhysicalSlotId(41U),
												  [](const CatalogEntry &, const CatalogEntry &) {})
						.has_value());
	relocationBatch.publish(components.bootstrap);
	const Superblock relocated = relocationBatch.candidate();
	MetadataWriteBatch pendingBatch(components.pools, relocated, LogSequenceNumber(4U));
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.prepareSegmentRelocation(
						pendingBatch, request.segment_id, Generation(2U), PhysicalSlotId(42U), PhysicalSlotId(42U),
						[](const CatalogEntry &, const CatalogEntry &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.validateRelocatedSourceRelease(relocated, request.segment_id,
																						Generation(1U)));
			},
			FormatError::FERR_INVALID_STATE);
	const RetireQueueEntry source =
			components.coordinator.validateRelocatedSourceRelease(relocated, request.segment_id, Generation(2U));
	MetadataWriteBatch releaseBatch(components.pools, relocated, LogSequenceNumber(4U));
	RetireQueueEntry badSource = source;
	badSource.physical_slot = PhysicalSlotId(41U);
	expectFormatError([&] { components.coordinator.prepareRelocatedSourceRelease(releaseBatch, badSource); },
					  FormatError::FERR_INVALID_STATE);
	RetireQueueEntry badIdentity = source;
	badIdentity.retired_generation = Generation(source.retired_generation.value() + 1U);
	expectFormatError([&] { components.coordinator.prepareRelocatedSourceRelease(releaseBatch, badIdentity); },
					  FormatError::FERR_INVALID_STATE);
	MetadataWriteBatch corruptAllocationBatch(components.pools, relocated, LogSequenceNumber(4U));
	components.allocator.prepareSetAllocated(corruptAllocationBatch, source.physical_slot, false);
	corruptAllocationBatch.publish(components.bootstrap);
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.validateRelocatedSourceRelease(
						corruptAllocationBatch.candidate(), request.segment_id, Generation(2U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogSegmentRelocationCoordinatorRejectsStaleRetirementGeneration) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegment(io, request);
	CatalogRelocationComponents components(io, current);

	MetadataWriteBatch relocationBatch(components.pools, current, LogSequenceNumber(3U));
	ASSERT_TRUE(components.coordinator
						.prepareSegmentRelocation(relocationBatch, request.segment_id, request.segment_generation,
												  PhysicalSlotId(41U), PhysicalSlotId(41U),
												  [](const CatalogEntry &, const CatalogEntry &) {})
						.has_value());
	relocationBatch.publish(components.bootstrap);
	const Superblock relocated = relocationBatch.candidate();
	const RetireQueueEntry source =
			components.coordinator.validateRelocatedSourceRelease(relocated, request.segment_id, Generation(2U));

	// A queue generation below the current Catalog generation cannot describe
	// the publication that made this source unreachable.
	RetireQueueEntry stale = source;
	stale.retired_generation = Generation(1U);
	MetadataWriteBatch corruptBatch(components.pools, relocated, LogSequenceNumber(4U));
	components.locations.prepareReplaceRetireQueue(corruptBatch, std::span(&stale, 1U), {});
	corruptBatch.publish(components.bootstrap);
	const Superblock corrupted = corruptBatch.candidate();

	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.validateRelocatedSourceRelease(corrupted, request.segment_id,
																						Generation(2U)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataWriteBatch releaseBatch(components.pools, corrupted, LogSequenceNumber(5U));
	expectFormatError([&] { components.coordinator.prepareRelocatedSourceRelease(releaseBatch, stale); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogSegmentRelocationCoordinatorRejectsFutureRetirementGeneration) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegment(io, request);
	CatalogRelocationComponents components(io, current);

	MetadataWriteBatch relocationBatch(components.pools, current, LogSequenceNumber(3U));
	ASSERT_TRUE(components.coordinator
						.prepareSegmentRelocation(relocationBatch, request.segment_id, request.segment_generation,
											  PhysicalSlotId(41U), PhysicalSlotId(41U),
											  [](const CatalogEntry &, const CatalogEntry &) {})
						.has_value());
	relocationBatch.publish(components.bootstrap);
	const Superblock relocated = relocationBatch.candidate();
	const RetireQueueEntry source =
			components.coordinator.validateRelocatedSourceRelease(relocated, request.segment_id, Generation(2U));

	MetadataWriteBatch corruptBatch(components.pools, relocated, LogSequenceNumber(4U));
	RetireQueueEntry future = source;
	future.retired_generation = Generation(corruptBatch.generation().value() + 1U);
	components.locations.prepareReplaceRetireQueue(corruptBatch, std::span(&future, 1U), {});
	corruptBatch.publish(components.bootstrap);
	const Superblock corrupted = corruptBatch.candidate();

	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.validateRelocatedSourceRelease(corrupted, request.segment_id,
																					Generation(2U)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataWriteBatch releaseBatch(components.pools, corrupted, LogSequenceNumber(5U));
	expectFormatError([&] { components.coordinator.prepareRelocatedSourceRelease(releaseBatch, future); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogSegmentRelocationCoordinatorRebindsOnlyCurrentFullExtent) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	request.require_absent = true;
	const ExtentLocation extent{ExtentId(91U), request.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
								RSS_PAGE_POOL_DATA_FRAME_COUNT, request.segment_generation};
	const Superblock current = publishReservedSegment(io, request, std::span(&extent, 1U));
	CatalogRelocationComponents components(io, current);

	MetadataWriteBatch invalidBatch(components.pools, current, LogSequenceNumber(3U));
	CatalogSlotRelocationPreparer empty;
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.prepareExtentRelocation(
						invalidBatch, extent, PhysicalSlotId(41U), PhysicalSlotId(41U), empty));
			},
			FormatError::FERR_INVALID_HEADER);
	ExtentLocation partial = extent;
	partial.page_count = RSS_PAGE_POOL_DATA_FRAME_COUNT - 1U;
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.prepareExtentRelocation(
						invalidBatch, partial, PhysicalSlotId(41U), PhysicalSlotId(41U),
						[](const CatalogEntry &, const CatalogEntry &) {}));
			},
			FormatError::FERR_INVALID_HEADER);
	ExtentLocation stale = extent;
	stale.generation = Generation(2U);
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.prepareExtentRelocation(
						invalidBatch, stale, PhysicalSlotId(41U), PhysicalSlotId(41U),
						[](const CatalogEntry &, const CatalogEntry &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	ExtentLocation missing = extent;
	missing.extent_id = ExtentId(extent.extent_id.value() + 1U);
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.prepareExtentRelocation(
						invalidBatch, missing, PhysicalSlotId(41U), PhysicalSlotId(41U),
						[](const CatalogEntry &, const CatalogEntry &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataWriteBatch noSpaceBatch(components.pools, current, LogSequenceNumber(3U));
	EXPECT_FALSE(components.coordinator
						 .prepareExtentRelocation(noSpaceBatch, extent, PhysicalSlotId(40U), PhysicalSlotId(40U),
												  [](const CatalogEntry &, const CatalogEntry &) {})
						 .has_value());
	MetadataWriteBatch pendingQueueBatch(components.pools, current, LogSequenceNumber(3U));
	const RetireQueueEntry pending{PhysicalSlotId(42U), request.segment_id, pendingQueueBatch.generation(),
								   RetireQueueDisposition::RQD_RELOCATED_SOURCE};
	components.retirement.prepareQueueInsertions(pendingQueueBatch, std::span(&pending, 1U));
	pendingQueueBatch.publish(components.bootstrap);
	MetadataWriteBatch pendingBatch(components.pools, pendingQueueBatch.candidate(), LogSequenceNumber(4U));
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.prepareExtentRelocation(
						pendingBatch, extent, PhysicalSlotId(41U), PhysicalSlotId(41U),
						[](const CatalogEntry &, const CatalogEntry &) {}));
			},
			FormatError::FERR_INVALID_STATE);

	MetadataWriteBatch relocationBatch(components.pools, current, LogSequenceNumber(3U));
	bool copied = false;
	const std::optional<CatalogSlotRelocationPlan> plan = components.coordinator.prepareExtentRelocation(
			relocationBatch, extent, PhysicalSlotId(41U), PhysicalSlotId(41U),
			[&copied](const CatalogEntry &source, const CatalogEntry &destination) {
				EXPECT_EQ(source.kind, SegmentKind::SGK_CODEBOOK_EXTENT);
				EXPECT_EQ(destination.generation, Generation(2U));
				copied = true;
			});
	ASSERT_TRUE(plan.has_value());
	EXPECT_TRUE(copied);
	const ExtentLocation rebound = components.locations.findExtent(relocationBatch.candidate(), extent.extent_id);
	EXPECT_EQ(rebound.segment_id, extent.segment_id);
	EXPECT_EQ(rebound.generation, Generation(2U));
	EXPECT_EQ(rebound.first_page, RSS_PAGE_POOL_FIRST_DATA_FRAME);
	EXPECT_EQ(rebound.page_count, RSS_PAGE_POOL_DATA_FRAME_COUNT);
}

TEST_F(RelocatableMetadataStoreTest, CatalogSegmentRelocationCoordinatorRejectsCorruptPendingSourceQueues) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegment(io, request);
	CatalogRelocationComponents components(io, current);
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.validateRelocatedSourceRelease(current, request.segment_id,
																						request.segment_generation));
			},
			FormatError::FERR_INVALID_STATE);

	MetadataWriteBatch coincidentQueueBatch(components.pools, current, LogSequenceNumber(3U));
	const RetireQueueEntry coincident{PhysicalSlotId(40U), request.segment_id, coincidentQueueBatch.generation(),
									  RetireQueueDisposition::RQD_RELOCATED_SOURCE};
	components.retirement.prepareQueueInsertions(coincidentQueueBatch, std::span(&coincident, 1U));
	coincidentQueueBatch.publish(components.bootstrap);
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.validateRelocatedSourceRelease(
						coincidentQueueBatch.candidate(), request.segment_id, request.segment_generation));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogSegmentRelocationCoordinatorSkipsForeignQueuesAndRejectsDuplicates) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegment(io, request);
	CatalogRelocationComponents components(io, current);

	MetadataWriteBatch foreignQueueBatch(components.pools, current, LogSequenceNumber(3U));
	const std::array unrelated{RetireQueueEntry{PhysicalSlotId(41U), request.segment_id, foreignQueueBatch.generation(),
												RetireQueueDisposition::RQD_RETIRED_SEGMENT},
							   RetireQueueEntry{PhysicalSlotId(42U), SegmentId(request.segment_id.value() + 1U),
												foreignQueueBatch.generation(),
												RetireQueueDisposition::RQD_RELOCATED_SOURCE}};
	components.retirement.prepareQueueInsertions(foreignQueueBatch, unrelated);
	foreignQueueBatch.publish(components.bootstrap);
	const Superblock withForeignQueue = foreignQueueBatch.candidate();
	MetadataWriteBatch allowedBatch(components.pools, withForeignQueue, LogSequenceNumber(4U));
	EXPECT_TRUE(components.coordinator
						.prepareSegmentRelocation(allowedBatch, request.segment_id, request.segment_generation,
												  PhysicalSlotId(43U), PhysicalSlotId(43U),
												  [](const CatalogEntry &, const CatalogEntry &) {})
						.has_value());

	MetadataWriteBatch duplicateQueueBatch(components.pools, withForeignQueue, LogSequenceNumber(4U));
	const std::array duplicates{
			RetireQueueEntry{PhysicalSlotId(44U), request.segment_id, duplicateQueueBatch.generation(),
							 RetireQueueDisposition::RQD_RELOCATED_SOURCE},
			RetireQueueEntry{PhysicalSlotId(45U), request.segment_id, duplicateQueueBatch.generation(),
							 RetireQueueDisposition::RQD_RELOCATED_SOURCE}};
	components.retirement.prepareQueueInsertions(duplicateQueueBatch, duplicates);
	duplicateQueueBatch.publish(components.bootstrap);
	MetadataWriteBatch rejectedBatch(components.pools, duplicateQueueBatch.candidate(), LogSequenceNumber(5U));
	expectFormatError(
			[&] {
				static_cast<void>(components.coordinator.prepareSegmentRelocation(
						rejectedBatch, request.segment_id, request.segment_generation, PhysicalSlotId(46U),
						PhysicalSlotId(46U), [](const CatalogEntry &, const CatalogEntry &) {}));
			},
			FormatError::FERR_INVALID_STATE);
}
