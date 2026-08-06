/**
 * @file test_CatalogRetireQueueReclaimer.cpp
 * @brief Direct validation for snapshot-safe Catalog retire-queue reclamation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>
#include <stdexcept>

#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/format/SuperblockFile.hpp"
#include "graph/storage/relocatable/metadata/CatalogRetireQueueReclaimer.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPagePool.hpp"
#include "graph/storage/relocatable/metadata/MetadataRetirement.hpp"
#include "graph/storage/relocatable/metadata/MetadataWriteBatch.hpp"

namespace {

	class CatalogRetireQueueReclaimerComponents final {
	public:
		CatalogRetireQueueReclaimerComponents(std::shared_ptr<StorageIO> storage, const Superblock &current) :
			storage_(std::move(storage)), bootstrap(storage_), pages(storage_, current),
			pools(storage_, current, MetadataPagePoolOpenMode::CPOM_ROOT_RECOVER), catalog(pages, pools),
			locations(pages, pools), allocator(pages, pools), snapshots(bootstrap),
			retirement(catalog, locations, snapshots), reclaimer(catalog, allocator, locations, retirement) {}

		std::shared_ptr<StorageIO> storage_;
		SuperblockFile bootstrap;
		MetadataPageFile pages;
		MetadataPagePoolAllocator pools;
		CatalogTree catalog;
		LocationDirectory locations;
		AllocatorDirectory allocator;
		MetadataSnapshotRegistry snapshots;
		MetadataRetirement retirement;
		CatalogRetireQueueReclaimer reclaimer;
	};

	[[nodiscard]] Superblock publishReservedSegments(const std::shared_ptr<StorageIO> &io,
													 std::span<const CatalogSlotReservationRequest> requests) {
		auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		const std::optional<CatalogSlotReservations> reservations = store->reserveFirstFreeAndUpsertCatalog(
				requests, PhysicalSlotId(40U), PhysicalSlotId(48U), LogSequenceNumber(2U));
		EXPECT_TRUE(reservations.has_value());
		if (!reservations.has_value()) {
			throw std::runtime_error("could not create Catalog retire-queue test fixture");
		}
		MetadataSnapshotPin snapshot = store->openSnapshot();
		const Superblock current = snapshot.superblock();
		snapshot.release();
		return current;
	}

	[[nodiscard]] Superblock publishRelocatedAndRetiredEntries(const std::shared_ptr<StorageIO> &io,
															   CatalogSlotReservationRequest &relocated,
															   CatalogSlotReservationRequest &retired) {
		const std::array requests{relocated, retired};
		auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
		const bool reserved = store->reserveFirstFreeAndUpsertCatalog(requests, PhysicalSlotId(40U),
																	  PhysicalSlotId(41U), LogSequenceNumber(2U))
									  .has_value();
		const bool relocatedSegment =
				reserved &&
				store->relocateCatalogSegment(relocated.segment_id, relocated.segment_generation, PhysicalSlotId(42U),
											  PhysicalSlotId(42U), LogSequenceNumber(3U),
											  [](const CatalogEntry &, const CatalogEntry &) {})
						.has_value();
		EXPECT_TRUE(reserved);
		EXPECT_TRUE(relocatedSegment);
		if (!relocatedSegment) {
			throw std::runtime_error("could not create relocated Catalog retire-queue test fixture");
		}
		static_cast<void>(
				store->retireCatalogSegment(retired.segment_id, retired.segment_generation, LogSequenceNumber(4U)));
		MetadataSnapshotPin snapshot = store->openSnapshot();
		const Superblock current = snapshot.superblock();
		snapshot.release();
		return current;
	}

	void stageRelocatedSource(CatalogRetireQueueReclaimerComponents &components, const Superblock &current,
							  const CatalogSlotReservationRequest &request, PhysicalSlotId destinationSlot,
							  std::span<const RetireQueueEntry> queueEntries, LogSequenceNumber checkpointLsn) {
		MetadataWriteBatch batch(components.pools, current, checkpointLsn);
		const CatalogEntry source = components.catalog.find(current, request.segment_id);
		batch.reserveDataSlot(destinationSlot);
		components.allocator.prepareSetAllocated(batch, destinationSlot, true);
		CatalogEntry destination = source;
		destination.physical_slot = destinationSlot;
		destination.generation = Generation(source.generation.value() + 1U);
		destination.committed_lsn = checkpointLsn;
		components.catalog.prepareUpsert(batch, std::span(&destination, 1U));
		components.retirement.prepareQueueInsertions(batch, queueEntries);
		batch.publish(components.bootstrap);
	}

	[[nodiscard]] Superblock stageRawQueueEntries(CatalogRetireQueueReclaimerComponents &components,
												  const Superblock &current, std::span<const RetireQueueEntry> entries,
												  std::span<const PhysicalSlotId> additionalAllocatedSlots,
												  const CatalogEntry *catalogUpdate, LogSequenceNumber checkpointLsn) {
		MetadataWriteBatch batch(components.pools, current, checkpointLsn);
		for (const PhysicalSlotId slot: additionalAllocatedSlots) {
			batch.reserveDataSlot(slot);
			components.allocator.prepareSetAllocated(batch, slot, true);
		}
		if (catalogUpdate != nullptr) {
			components.catalog.prepareUpsert(batch, std::span(catalogUpdate, 1U));
		}
		components.locations.prepareReplaceRetireQueue(batch, entries, {});
		batch.publish(components.bootstrap);
		return batch.candidate();
	}

} // namespace

TEST_F(RelocatableMetadataStoreTest, CatalogRetireQueueReclaimerStagesRelocatedAndRetiredReleasesAtomically) {
	CatalogSlotReservationRequest relocated = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	relocated.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	relocated.require_absent = true;
	CatalogSlotReservationRequest retired = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U, 1U);
	retired.kind = SegmentKind::SGK_CODEBOOK_EXTENT;
	retired.require_absent = true;
	const Superblock current = publishRelocatedAndRetiredEntries(io, relocated, retired);
	CatalogRetireQueueReclaimerComponents components(io, current);
	MetadataWriteGuard writer = components.snapshots.beginWrite();
	const std::vector<RetireQueueEntry> reclaimable = components.reclaimer.collectReclaimable(current, writer);
	ASSERT_EQ(reclaimable.size(), 2U);
	EXPECT_EQ(reclaimable.front().physical_slot, PhysicalSlotId(40U));
	EXPECT_EQ(reclaimable.back().physical_slot, PhysicalSlotId(41U));
	MetadataWriteBatch emptyBatch(components.pools, current, LogSequenceNumber(5U));
	EXPECT_NO_THROW(components.reclaimer.prepareReclamation(emptyBatch, {}));
	const std::array reversed{reclaimable.back(), reclaimable.front()};
	expectFormatError([&] { components.reclaimer.prepareReclamation(emptyBatch, reversed); },
					  FormatError::FERR_INVALID_STATE);

	MetadataWriteBatch batch(components.pools, current, LogSequenceNumber(5U));
	EXPECT_NO_THROW(components.reclaimer.prepareReclamation(batch, reclaimable));
	EXPECT_FALSE(components.allocator.isAllocated(batch.candidate(), PhysicalSlotId(40U)));
	EXPECT_FALSE(components.allocator.isAllocated(batch.candidate(), PhysicalSlotId(41U)));
	EXPECT_TRUE(components.allocator.isAllocated(batch.candidate(), PhysicalSlotId(42U)));
	EXPECT_FALSE(components.locations.findRetireQueueEntryOptional(batch.candidate(), PhysicalSlotId(40U)).has_value());
	EXPECT_FALSE(components.locations.findRetireQueueEntryOptional(batch.candidate(), PhysicalSlotId(41U)).has_value());
	EXPECT_EQ(components.catalog.find(batch.candidate(), relocated.segment_id).physical_slot, PhysicalSlotId(42U));
	EXPECT_EQ(components.catalog.find(batch.candidate(), retired.segment_id).state, SegmentState::SGS_OBSOLETE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogRetireQueueReclaimerRetainsSlotsVisibleToAnActiveSnapshot) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegments(io, std::span(&request, 1U));
	CatalogRetireQueueReclaimerComponents components(io, current);
	MetadataSnapshotPin snapshot = components.snapshots.openSnapshot();
	const RetireQueueEntry source{PhysicalSlotId(40U), request.segment_id, Generation(3U),
								  RetireQueueDisposition::RQD_RELOCATED_SOURCE};
	stageRelocatedSource(components, current, request, PhysicalSlotId(41U), std::span(&source, 1U),
						 LogSequenceNumber(3U));
	const Superblock relocated = components.bootstrap.recover();
	MetadataWriteGuard writer = components.snapshots.beginWrite();
	EXPECT_TRUE(components.reclaimer.collectReclaimable(relocated, writer).empty());
	snapshot.release();
	const std::vector<RetireQueueEntry> reclaimable = components.reclaimer.collectReclaimable(relocated, writer);
	ASSERT_EQ(reclaimable.size(), 1U);
	EXPECT_EQ(reclaimable.front(), source);
}

TEST_F(RelocatableMetadataStoreTest, CatalogRetireQueueReclaimerRejectsCorruptAndDuplicateQueueEntries) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegments(io, std::span(&request, 1U));
	CatalogRetireQueueReclaimerComponents components(io, current);
	const std::array duplicateSources{RetireQueueEntry{PhysicalSlotId(40U), request.segment_id, Generation(3U),
													   RetireQueueDisposition::RQD_RELOCATED_SOURCE},
									  RetireQueueEntry{PhysicalSlotId(42U), request.segment_id, Generation(3U),
													   RetireQueueDisposition::RQD_RELOCATED_SOURCE}};
	MetadataWriteBatch duplicateBatch(components.pools, current, LogSequenceNumber(3U));
	components.allocator.prepareSetAllocated(duplicateBatch, PhysicalSlotId(41U), true);
	components.allocator.prepareSetAllocated(duplicateBatch, PhysicalSlotId(42U), true);
	CatalogEntry destination = components.catalog.find(current, request.segment_id);
	destination.physical_slot = PhysicalSlotId(41U);
	destination.generation = Generation(2U);
	destination.committed_lsn = LogSequenceNumber(3U);
	components.catalog.prepareUpsert(duplicateBatch, std::span(&destination, 1U));
	// Direct directory staging models a well-checksummed persistent queue that
	// bypassed the write-path validator and therefore must still fail closed.
	components.locations.prepareReplaceRetireQueue(duplicateBatch, duplicateSources, {});
	duplicateBatch.publish(components.bootstrap);
	MetadataWriteGuard writer = components.snapshots.beginWrite();
	expectFormatError(
			[&] { static_cast<void>(components.reclaimer.collectReclaimable(duplicateBatch.candidate(), writer)); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogRetireQueueReclaimerRejectsAQueuedCurrentCatalogSlot) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegments(io, std::span(&request, 1U));
	CatalogRetireQueueReclaimerComponents components(io, current);
	const RetireQueueEntry currentSlot{PhysicalSlotId(40U), request.segment_id, Generation(3U),
									   RetireQueueDisposition::RQD_RELOCATED_SOURCE};
	MetadataWriteBatch corruptBatch(components.pools, current, LogSequenceNumber(3U));
	components.retirement.prepareQueueInsertions(corruptBatch, std::span(&currentSlot, 1U));
	corruptBatch.publish(components.bootstrap);
	MetadataWriteGuard writer = components.snapshots.beginWrite();
	expectFormatError(
			[&] { static_cast<void>(components.reclaimer.collectReclaimable(corruptBatch.candidate(), writer)); },
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogRetireQueueReclaimerRejectsUnallocatedAndUnknownQueueEntries) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegments(io, std::span(&request, 1U));
	CatalogRetireQueueReclaimerComponents components(io, current);
	const RetireQueueEntry unallocated{PhysicalSlotId(42U), request.segment_id, Generation(3U),
									   RetireQueueDisposition::RQD_RELOCATED_SOURCE};
	const Superblock invalid =
			stageRawQueueEntries(components, current, std::span(&unallocated, 1U), {}, nullptr, LogSequenceNumber(3U));
	MetadataWriteGuard writer = components.snapshots.beginWrite();
	expectFormatError([&] { static_cast<void>(components.reclaimer.collectReclaimable(invalid, writer)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogRetireQueueReclaimerRejectsObsoleteAndInconsistentRetiredMappings) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegments(io, std::span(&request, 1U));
	CatalogRetireQueueReclaimerComponents components(io, current);
	const PhysicalSlotId extraSlot(42U);
	CatalogEntry obsolete = components.catalog.find(current, request.segment_id);
	obsolete.state = SegmentState::SGS_OBSOLETE;
	obsolete.committed_lsn = LogSequenceNumber(3U);
	const RetireQueueEntry relocated{extraSlot, request.segment_id, Generation(3U),
									 RetireQueueDisposition::RQD_RELOCATED_SOURCE};
	const Superblock obsoleteCurrent =
			stageRawQueueEntries(components, current, std::span(&relocated, 1U), std::span(&extraSlot, 1U), &obsolete,
								 LogSequenceNumber(3U));
	MetadataWriteGuard writer = components.snapshots.beginWrite();
	expectFormatError([&] { static_cast<void>(components.reclaimer.collectReclaimable(obsoleteCurrent, writer)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogRetireQueueReclaimerRejectsRetiredQueueMappingsThatDisagreeWithCatalog) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegments(io, std::span(&request, 1U));
	CatalogRetireQueueReclaimerComponents components(io, current);
	const RetireQueueEntry wrongState{PhysicalSlotId(40U), request.segment_id, Generation(3U),
									  RetireQueueDisposition::RQD_RETIRED_SEGMENT};
	const Superblock sealedCurrent =
			stageRawQueueEntries(components, current, std::span(&wrongState, 1U), {}, nullptr, LogSequenceNumber(3U));
	MetadataWriteGuard writer = components.snapshots.beginWrite();
	expectFormatError([&] { static_cast<void>(components.reclaimer.collectReclaimable(sealedCurrent, writer)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogRetireQueueReclaimerRejectsRetiredQueueMappingsWithAnotherPhysicalSlot) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegments(io, std::span(&request, 1U));
	CatalogRetireQueueReclaimerComponents components(io, current);
	const PhysicalSlotId extraSlot(42U);
	CatalogEntry retired = components.catalog.find(current, request.segment_id);
	retired.state = SegmentState::SGS_RETIRED;
	retired.committed_lsn = LogSequenceNumber(3U);
	const RetireQueueEntry wrongSlot{extraSlot, request.segment_id, Generation(3U),
									 RetireQueueDisposition::RQD_RETIRED_SEGMENT};
	const Superblock retiredCurrent = stageRawQueueEntries(components, current, std::span(&wrongSlot, 1U),
														   std::span(&extraSlot, 1U), &retired, LogSequenceNumber(3U));
	MetadataWriteGuard writer = components.snapshots.beginWrite();
	expectFormatError([&] { static_cast<void>(components.reclaimer.collectReclaimable(retiredCurrent, writer)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogRetireQueueReclaimerRejectsMismatchedDurableAndDuplicateReclamationInput) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegments(io, std::span(&request, 1U));
	CatalogRetireQueueReclaimerComponents components(io, current);
	const RetireQueueEntry source{PhysicalSlotId(40U), request.segment_id, Generation(3U),
								  RetireQueueDisposition::RQD_RELOCATED_SOURCE};
	stageRelocatedSource(components, current, request, PhysicalSlotId(41U), std::span(&source, 1U),
						 LogSequenceNumber(3U));
	const Superblock relocated = components.bootstrap.recover();
	MetadataWriteBatch mismatchBatch(components.pools, relocated, LogSequenceNumber(4U));
	RetireQueueEntry mismatched = source;
	mismatched.retired_generation = Generation(4U);
	expectFormatError([&] { components.reclaimer.prepareReclamation(mismatchBatch, std::span(&mismatched, 1U)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, CatalogRetireQueueReclaimerRejectsDuplicateLogicalReclamationInput) {
	CatalogSlotReservationRequest request = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
	request.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.require_absent = true;
	const Superblock current = publishReservedSegments(io, std::span(&request, 1U));
	const std::array duplicateSources{RetireQueueEntry{PhysicalSlotId(40U), request.segment_id, Generation(3U),
													   RetireQueueDisposition::RQD_RELOCATED_SOURCE},
									  RetireQueueEntry{PhysicalSlotId(42U), request.segment_id, Generation(3U),
													   RetireQueueDisposition::RQD_RELOCATED_SOURCE}};
	CatalogRetireQueueReclaimerComponents components(io, current);
	const PhysicalSlotId destinationSlot(41U);
	const PhysicalSlotId secondSourceSlot(42U);
	CatalogEntry destination = components.catalog.find(current, request.segment_id);
	destination.physical_slot = destinationSlot;
	destination.generation = Generation(2U);
	destination.committed_lsn = LogSequenceNumber(3U);
	const std::array allocationSlots{destinationSlot, secondSourceSlot};
	const Superblock duplicateCurrent = stageRawQueueEntries(components, current, duplicateSources, allocationSlots,
															 &destination, LogSequenceNumber(3U));
	MetadataWriteBatch duplicateBatch(components.pools, duplicateCurrent, LogSequenceNumber(4U));
	expectFormatError([&] { components.reclaimer.prepareReclamation(duplicateBatch, duplicateSources); },
					  FormatError::FERR_INVALID_STATE);
}
