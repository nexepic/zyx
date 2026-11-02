/**
 * @file test_RelocatableMetadataStoreReservations.cpp
 * @brief Reservation and ID lease lifecycle tests for the metadata facade.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <limits>
#include <map>
#include <system_error>
#include <vector>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/relocatable/allocation/AllocatorDirectory.hpp"
#include "graph/storage/relocatable/maintenance/RelocatableIntegrityScanner.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/metadata/CatalogTree.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageRouteDirectory.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

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

TEST_F(RelocatableMetadataStoreTest, LogicalGraphIdLeasesRejectInvalidRequestsWithoutPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock expected = before.superblock();
	before.release();

	expectFormatError(
			[&] { static_cast<void>(store->reserveNativeGraphLogicalIds(0U, 0U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->acquireNativeGraphLogicalIds(0U, 0U, 1U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->acquireNativeGraphLogicalIds(1U, 0U, 0U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->acquireNativeGraphLogicalIds(
						1U, 0U, std::numeric_limits<uint64_t>::max()));
			},
			FormatError::FERR_INVALID_HEADER);

	MetadataSnapshotPin after = store->openSnapshot();
	EXPECT_EQ(after.superblock().generation, expected.generation);
	EXPECT_EQ(after.superblock().checkpoint_lsn, expected.checkpoint_lsn);
	EXPECT_EQ(after.superblock().next_logical_node_id, expected.next_logical_node_id);
	EXPECT_EQ(after.superblock().next_logical_edge_id, expected.next_logical_edge_id);
}

TEST_F(RelocatableMetadataStoreTest, LogicalGraphIdLeasesServeFullyCachedReservationsWithoutRepublishing) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const NativeGraphLogicalIdReservation first = store->acquireNativeGraphLogicalIds(1U, 1U, 4U);
	EXPECT_EQ(first.node_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{1U, 1U}}));
	EXPECT_EQ(first.edge_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{1U, 1U}}));

	MetadataSnapshotPin afterFirstLease = store->openSnapshot();
	const Superblock published = afterFirstLease.superblock();
	EXPECT_EQ(published.next_logical_node_id, 5U);
	EXPECT_EQ(published.next_logical_edge_id, 5U);
	afterFirstLease.release();

	const NativeGraphLogicalIdReservation cached = store->acquireNativeGraphLogicalIds(2U, 3U, 4U);
	EXPECT_EQ(cached.node_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{2U, 2U}}));
	EXPECT_EQ(cached.edge_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{2U, 3U}}));
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation(first));
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation(cached));

	MetadataSnapshotPin afterCachedLease = store->openSnapshot();
	EXPECT_EQ(afterCachedLease.superblock().generation, published.generation);
	EXPECT_EQ(afterCachedLease.superblock().checkpoint_lsn, published.checkpoint_lsn);
	EXPECT_EQ(afterCachedLease.superblock().next_logical_node_id, published.next_logical_node_id);
	EXPECT_EQ(afterCachedLease.superblock().next_logical_edge_id, published.next_logical_edge_id);
}

TEST_F(RelocatableMetadataStoreTest, ReadOnlyMetadataStoreRejectsLeasesAndRefreshWithoutPublication) {
	auto writable = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = writable->openSnapshot();
	const Superblock expected = before.superblock();
	before.release();
	auto readOnly = RelocatableMetadataStore::openReadOnly(io);

	expectFormatError(
			[&] { static_cast<void>(readOnly->reserveNativeGraphLogicalIds(1U, 0U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(readOnly->acquireNativeGraphLogicalIds(1U, 0U, 1U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { readOnly->refreshAfterExternalPublication(); }, FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin after = writable->openSnapshot();
	EXPECT_EQ(after.superblock().generation, expected.generation);
	EXPECT_EQ(after.superblock().checkpoint_lsn, expected.checkpoint_lsn);
	EXPECT_EQ(after.superblock().next_logical_node_id, expected.next_logical_node_id);
	EXPECT_EQ(after.superblock().next_logical_edge_id, expected.next_logical_edge_id);
}

TEST_F(RelocatableMetadataStoreTest, SingleOwnerKeepsPinnedCatalogAcrossLeasesAndReopen) {
    auto store = RelocatableMetadataStore::initializeNewComplete(io, superblock(1U, 1U));
    auto first = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID, 1U);
    first.require_absent = true;
    const auto prepare = [&](std::span<const CatalogEntry> pools) {
        for (const auto &pool : pools) initializeTestSharedPagePool(*io, RSS_FILE_BOOTSTRAP_BYTES, pool);
        const auto last = pools.back().physical_slot;
        const auto end = PhysicalSegmentAllocator::checkedSlotOffset(RSS_FILE_BOOTSTRAP_BYTES, last) + RSS_SEGMENT_SIZE;
        const std::byte zero{}; io->writeAt(end - 1U, &zero, 1U);
    };
    const auto firstReservation = store->reserveFirstFreeAndUpsertCatalog(std::span(&first, 1U),
        PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(3U), prepare);
    ASSERT_TRUE(firstReservation);
    auto old = store->openSnapshot();
    const auto graphRevision = old.superblock().graph_revision;
    const NativeGraphLogicalIdLease lease = store->reserveNativeGraphLogicalIds(2U, 1U);
    EXPECT_EQ(lease.nodes, (NativeGraphLogicalIdLeaseRange{1U, 2U}));
    EXPECT_EQ(lease.edges, (NativeGraphLogicalIdLeaseRange{1U, 1U}));
    auto second = reservationRequest(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U, 1U);
    second.require_absent = true;
    const auto secondReservation = store->reserveFirstFreeAndUpsertCatalog(std::span(&second, 1U),
        PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(5U), prepare);
    ASSERT_TRUE(secondReservation);
    EXPECT_EQ(store->findCatalog(old, first.segment_id).physical_slot, PhysicalSlotId(40U));
    EXPECT_THROW(static_cast<void>(store->findCatalog(old, second.segment_id)), FormatException);
    auto current = store->openSnapshot();
    EXPECT_EQ(current.superblock().graph_revision, graphRevision);
    EXPECT_EQ(store->findCatalog(current, second.segment_id).physical_slot, PhysicalSlotId(41U));
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *store).scan()));
    old.release(); current.release(); store.reset();
    auto reopened = RelocatableMetadataStore::open(io);
    auto recovered = reopened->openSnapshot();
    EXPECT_EQ(reopened->findCatalog(recovered, first.segment_id).physical_slot, PhysicalSlotId(40U));
    EXPECT_EQ(reopened->findCatalog(recovered, second.segment_id).physical_slot, PhysicalSlotId(41U));
    EXPECT_EQ(recovered.superblock().next_logical_node_id, 3U);
    EXPECT_EQ(recovered.superblock().next_logical_edge_id, 2U);
    EXPECT_EQ(recovered.superblock().graph_revision, graphRevision);
    EXPECT_NO_THROW(static_cast<void>(RelocatableIntegrityScanner(io, *reopened).scan()));
}
