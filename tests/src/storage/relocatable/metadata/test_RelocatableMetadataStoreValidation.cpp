/**
 * @file test_RelocatableMetadataStoreValidation.cpp
 * @brief Validation coverage for relocatable Metadata Store public operations.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreValidationTestFixture.hpp"

#include <atomic>
#include <cerrno>
#include <limits>
#include <system_error>

#include "graph/storage/PreadHelper.hpp"
#include "graph/storage/PwriteHelper.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

TEST_F(RelocatableMetadataStoreValidationTest, RejectsAllocatorRadixSummaryThatDisagreesWithItsBitmapLeaf) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const Superblock current = snapshot.superblock();
	snapshot.release();
	MetadataPageFile pages(io, current);
	MetadataRadixPage allocatorRoot = pages.readMetadataRadix(current.allocator_root);
	ASSERT_EQ(allocatorRoot.directory_type, MetadataDirectoryType::MDT_ALLOCATOR);
	ASSERT_TRUE(allocatorRoot.children[0].isValid());
	allocatorRoot.allocator_free_children[0] &= ~std::byte{0x01};
	pages.writeMetadataRadix(current.allocator_root, allocatorRoot);
	store.reset();

	expectFormatError([&] { static_cast<void>(RelocatableMetadataStore::open(io)); }, FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsInvalidAndReadOnlyLogicalIdLeaseRequestsBeforePublication) {
	auto writable = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	expectFormatError([&] { static_cast<void>(writable->reserveNativeGraphLogicalIds(0U, 0U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(writable->acquireNativeGraphLogicalIds(0U, 0U, 1U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(writable->acquireNativeGraphLogicalIds(1U, 0U, 0U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(writable->acquireNativeGraphLogicalIds(
						1U, 0U, std::numeric_limits<uint64_t>::max()));
			},
			FormatError::FERR_INVALID_HEADER);

	auto readOnly = RelocatableMetadataStore::openReadOnly(io);
	expectFormatError([&] { readOnly->refreshAfterExternalPublication(); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(readOnly->reserveNativeGraphLogicalIds(1U, 0U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(readOnly->acquireNativeGraphLogicalIds(1U, 0U, 1U)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreValidationTest,
		ServesFullyCachedLogicalIdLeasesWithoutPublishingAnotherMetadataGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const NativeGraphLogicalIdReservation initial = store->acquireNativeGraphLogicalIds(1U, 1U, 4U);
	ASSERT_EQ(initial.node_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{1U, 1U}}));
	ASSERT_EQ(initial.edge_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{1U, 1U}}));
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation(initial));

	MetadataSnapshotPin afterInitial = store->openSnapshot();
	const Superblock durableAfterInitial = afterInitial.superblock();
	afterInitial.release();
	EXPECT_EQ(durableAfterInitial.generation, Generation(2U));
	EXPECT_EQ(durableAfterInitial.next_logical_node_id, 5U);
	EXPECT_EQ(durableAfterInitial.next_logical_edge_id, 5U);

	const NativeGraphLogicalIdReservation cached = store->acquireNativeGraphLogicalIds(2U, 3U, 4U);
	EXPECT_EQ(cached.node_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{2U, 2U}}));
	EXPECT_EQ(cached.edge_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{2U, 3U}}));
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation(cached));
	EXPECT_FALSE(store->claimNativeGraphLogicalIdReservation(cached));

	MetadataSnapshotPin afterCached = store->openSnapshot();
	EXPECT_EQ(afterCached.superblock(), durableAfterInitial);
	afterCached.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
		CombinesCachedAndFreshLogicalIdRangesWithoutReorderingNamespaces) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const NativeGraphLogicalIdReservation initial = store->acquireNativeGraphLogicalIds(1U, 1U, 4U);
	ASSERT_TRUE(store->claimNativeGraphLogicalIdReservation(initial));
	const NativeGraphLogicalIdReservation cached = store->acquireNativeGraphLogicalIds(2U, 3U, 4U);
	ASSERT_TRUE(store->claimNativeGraphLogicalIdReservation(cached));

	// Only Node ID 4 remains cached. The request must return it before the
	// freshly leased Node range while independently leasing the exhausted Edge
	// namespace.
	const NativeGraphLogicalIdReservation mixed = store->acquireNativeGraphLogicalIds(2U, 1U, 4U);
	EXPECT_EQ(mixed.node_ranges,
			  (std::vector<NativeGraphLogicalIdLeaseRange>{{4U, 1U}, {5U, 1U}}));
	EXPECT_EQ(mixed.edge_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{5U, 1U}}));
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation(mixed));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(3U));
	EXPECT_EQ(snapshot.superblock().next_logical_node_id, 9U);
	EXPECT_EQ(snapshot.superblock().next_logical_edge_id, 9U);
	snapshot.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   DistinguishesIdentityOnlyBootstrapAdvancesFromDirectoryMutations) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	MetadataSnapshotPin legacySnapshot = store->openSnapshot();
	const Superblock legacy = legacySnapshot.superblock();
	legacySnapshot.release();

	const Superblock routed = store->migratePageDirectoryToBootstrap(LogSequenceNumber(2U));
	ASSERT_TRUE(routed.usesPageDirectoryBootstrap());
	EXPECT_FALSE(store->isIdentityOnlyBootstrapAdvance(legacy, routed));

	const NativeGraphLogicalIdLease lease = store->reserveNativeGraphLogicalIds(1U, 0U);
	EXPECT_TRUE(store->isIdentityOnlyBootstrapAdvance(routed, lease.superblock));

	const Superblock directoryMutation =
			store->setPhysicalSlotAllocated(PhysicalSlotId(40U), true, LogSequenceNumber(4U));
	EXPECT_FALSE(store->isIdentityOnlyBootstrapAdvance(lease.superblock, directoryMutation));
}

TEST_F(RelocatableMetadataStoreValidationTest, ReadOnlyStoreRejectsMetadataMutationAndMaintenanceEntrypoints) {
	static_cast<void>(RelocatableMetadataStore::initializeNew(io, initialSuperblock()));
	auto readOnly = RelocatableMetadataStore::openReadOnly(io);

	// These entry points either publish a new metadata generation or alter the
	// physical tail. A read-only handle must reject them before inspecting state.
	expectFormatError(
			[&] { static_cast<void>(readOnly->migratePageDirectoryToBootstrap(LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { static_cast<void>(readOnly->reclaimEmptyMetadataPagePools(LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { static_cast<void>(readOnly->evacuateSparseMetadataPagePool(LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { static_cast<void>(readOnly->rotateMetadataEmergencyReserve(LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(readOnly->truncateReleasedTail()); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsInactiveSnapshotPinsAcrossDirectoryReadSurfaces) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const MetadataSnapshotPin inactive;
	const EntityPropertyKey propertyKey{EntityPropertyOwnerKind::EPOK_NODE, 1U};
	const auto stopNode = [](const EntityPageLocation &) { return false; };
	const auto stopPage = [](const PageLocation &) { return false; };

	expectFormatError([&] { static_cast<void>(store->findCatalog(inactive, SegmentId(1U))); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { store->visitCatalogEntries(inactive, [](const CatalogEntry &) {}); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findPageLocation(inactive, PageId(1U))); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findPageLocationOptional(inactive, PageId(1U))); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findExtentLocation(inactive, ExtentId(1U))); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findExtentLocationOptional(inactive, ExtentId(1U))); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findNodePageLocation(inactive, 1U)); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findEdgePageLocation(inactive, 1U)); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findEntityPropertyLocation(inactive, propertyKey)); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findStateLocation(inactive, 1U)); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findVectorLocation(inactive, 1U)); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findVectorManifest(inactive, 1U)); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findLargeValueManifest(inactive, LargeValueId(1U))); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findIndexRoot(inactive, 1U)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { store->visitNodePageLocations(inactive, [](const EntityPageLocation &) {}); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->visitNodePageLocationsUntil(inactive, stopNode)); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->visitNodePageLocationsAfter(inactive, 0U, 1U, stopNode)); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->visitPageLocationsUntil(inactive, stopPage)); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->findPageLocationsForSegment(inactive, SegmentId(1U))); },
				  FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(store->isPhysicalSlotAllocated(inactive, PhysicalSlotId(1U))); },
				  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsEmptyDirectoryMutationsAndInvalidStateReplacementInput) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());

	expectFormatError(
			[&] {
				static_cast<void>(store->upsertEntityPageLocations({}, {}, {}, {}, LogSequenceNumber(2U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->upsertEntityPropertyLocations({}, {}, LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->upsertStateLocations({}, {}, LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->upsertVectorLocations({}, {}, LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->upsertVectorManifests({}, {}, LogSequenceNumber(2U))); },
			FormatError::FERR_INVALID_HEADER);

	const PageLocation invalidPage{};
	const StateLocation invalidState{};
	expectFormatError(
			[&] {
				static_cast<void>(store->replaceStatePageAndUpsertLocation(
						invalidPage, invalidState, LogSequenceNumber(2U), {}));
			},
			FormatError::FERR_INVALID_HEADER);

	const auto statePreparer = [](const CatalogEntry &pool, PageId pageId, PageLocation &page, StateLocation &state) {
		page = {pageId, pool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, pool.generation, 0U};
		state = {state.state_id, pageId, 0U, 1U};
	};
	const auto materialized = store->materializeStatePagePool(
			1U, 77U, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U), statePreparer);
	ASSERT_TRUE(materialized.has_value());
	StateLocation invalidLocation = materialized->state;
	invalidLocation.state_id = 0U;
	expectFormatError(
			[&] {
				static_cast<void>(store->upsertStateLocations(std::span(&invalidLocation, 1U), {}, LogSequenceNumber(3U)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsInvalidEntityPageMutationPreconditions) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const LogSequenceNumber checkpointLsn(2U);
	const ExistingPoolPagePreparer pagePreparer = [](const CatalogEntry &, PageLocation &) {};
	const ExistingPoolPageReplacementPreparer replacementPreparer =
			[](const CatalogEntry &, const PageLocation &, PageLocation &) {};
	const ExistingPoolPageRetirementPreparer retirementPreparer = [](const CatalogEntry &, const PageLocation &) {};
	const EntityPageLocation node{1U, PageId(1U), 0U, 1U};
	const EntityPageLocation edge{1U, PageId(1U), 0U, 1U};

	expectFormatError(
			[&] { static_cast<void>(store->appendPageToExistingPool({}, {}, checkpointLsn, {})); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->appendEntityPageToExistingPool({}, {}, {}, {}, checkpointLsn, {})); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->appendEntityPageToExistingPool({}, {}, {}, {}, {}, pagePreparer)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->appendEntityPageToExistingPool({}, {}, {}, {}, checkpointLsn, pagePreparer)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						store->replaceEntityPageInExistingPool({}, {}, {}, {}, {}, {}, {}, checkpointLsn, {}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(
						store->replaceEntityPageInExistingPool({}, {}, {}, {}, {}, {}, {}, {}, replacementPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->replaceEntityPageInExistingPool(
						{}, {}, {}, std::span(&node, 1U), {}, std::span(&edge, 1U), {}, checkpointLsn,
						replacementPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->mutateEntityPages({}, {}, {}, checkpointLsn, {})); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->mutateEntityPagesInExistingPools({}, {}, checkpointLsn, {})); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->replaceEntityPagesInExistingPools({}, checkpointLsn, {})); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->retireEntityPageInExistingPool({}, {}, {}, {}, checkpointLsn, {})); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store->retireEntityPageInExistingPool({}, {}, {}, {}, {}, retirementPreparer)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->retireEntityPageInExistingPool({}, {}, {}, {}, checkpointLsn, retirementPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsInvalidExistingEntityPoolMutationsBeforePublishingMetadata) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	CatalogSlotReservationRequest request =
			reservationRequest(SegmentId(31U), SegmentKind::SGK_ENTITY_PAGE_POOL);
	const std::optional<CatalogSlotReservation> reservation =
			store->reserveFirstFreeAndUpsertCatalog(request, PhysicalSlotId(40U), PhysicalSlotId(40U),
															LogSequenceNumber(2U));
	ASSERT_TRUE(reservation.has_value());

	const EntityPageLocation firstNode{101U, PageId(91U), 0U, 1U};
	PageLocation firstPage{firstNode.page_id, reservation->entry.segment_id, 0U, reservation->entry.generation};
	static_cast<void>(store->appendEntityPageToExistingPool(
			reservation->entry, firstPage, std::span(&firstNode, 1U), {}, LogSequenceNumber(3U),
			[](const CatalogEntry &, PageLocation &page) { page.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME; }));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	const PageLocation publishedFirstPage = store->findPageLocation(before, firstNode.page_id);
	before.release();

	bool callbackCalled = false;
	const EntityPageLocation duplicateNode{102U, firstNode.page_id, 0U, 1U};
	expectFormatError(
			[&] {
				static_cast<void>(store->appendEntityPageToExistingPool(
						reservation->entry, firstPage, std::span(&duplicateNode, 1U), {}, LogSequenceNumber(4U),
						[&callbackCalled](const CatalogEntry &, PageLocation &) { callbackCalled = true; }));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(callbackCalled);

	PageLocation secondPage{PageId(92U), reservation->entry.segment_id, 0U, reservation->entry.generation};
	const EntityPageLocation wrongPage{102U, PageId(93U), 0U, 1U};
	expectFormatError(
			[&] {
				static_cast<void>(store->appendEntityPageToExistingPool(
						reservation->entry, secondPage, std::span(&wrongPage, 1U), {}, LogSequenceNumber(4U),
						[](const CatalogEntry &, PageLocation &) {}));
			},
			FormatError::FERR_INVALID_STATE);

	const EntityPageLocation invalidNode{0U, firstNode.page_id, 1U, 1U};
	PageLocation replacement{firstNode.page_id, reservation->entry.segment_id, 0U, reservation->entry.generation};
	expectFormatError(
			[&] {
				static_cast<void>(store->replaceEntityPageInExistingPool(
						reservation->entry, publishedFirstPage, replacement, std::span(&invalidNode, 1U), {}, {}, {},
						LogSequenceNumber(4U), [](const CatalogEntry &, const PageLocation &, PageLocation &) {}));
			},
			FormatError::FERR_INVALID_STATE);

	const std::array<uint64_t, 1U> missingNodes{102U};
	expectFormatError(
			[&] {
				static_cast<void>(store->replaceEntityPageInExistingPool(
						reservation->entry, publishedFirstPage, replacement, {}, missingNodes, {}, {}, LogSequenceNumber(4U),
						[](const CatalogEntry &, const PageLocation &, PageLocation &) {}));
			},
			FormatError::FERR_INVALID_STATE);

	callbackCalled = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->replaceEntityPageInExistingPool(
						reservation->entry, publishedFirstPage, replacement, {}, {}, {}, {}, LogSequenceNumber(4U),
						[&callbackCalled](const CatalogEntry &, const PageLocation &, PageLocation &) {
							callbackCalled = true;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(callbackCalled);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
	EXPECT_EQ(store->findPageLocation(unchanged, firstNode.page_id), publishedFirstPage);
	EXPECT_EQ(store->findNodePageLocation(unchanged, firstNode.entity_id), firstNode);
	unchanged.release();
}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsInvalidCompleteInitializationCheckpoints) {
	Superblock invalid = initialSuperblock();
	invalid.checkpoint_lsn = {};
	expectFormatError([&] { static_cast<void>(RelocatableMetadataStore::initializeNewComplete(io, invalid)); },
					  FormatError::FERR_INVALID_HEADER);

	invalid = initialSuperblock();
	invalid.checkpoint_lsn = LogSequenceNumber(std::numeric_limits<uint64_t>::max());
	expectFormatError([&] { static_cast<void>(RelocatableMetadataStore::initializeNewComplete(io, invalid)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsEmptyEntityPoolReservationPlan) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const LogSequenceNumber checkpointLsn(2U);

	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndEntityPageLocations(
						{}, {}, {}, {}, PhysicalSlotId(40U), PhysicalSlotId(40U), checkpointLsn, {}));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreValidationTest, ValidatesLogicalIdLeaseCapabilitiesAcrossNodeAndEdgeNamespaces) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation({}));
	const NativeGraphLogicalIdReservation reservation = store->acquireNativeGraphLogicalIds(2U, 0U, 2U);
	ASSERT_EQ(reservation.node_ranges.size(), 1U);
	EXPECT_TRUE(reservation.edge_ranges.empty());

	// A caller may only claim the exact, nonempty capability it received. This
	// rejects malformed and fabricated ranges without consuming the real lease.
	NativeGraphLogicalIdReservation zeroFirst = reservation;
	zeroFirst.node_ranges.front().first_id = 0U;
	EXPECT_FALSE(store->claimNativeGraphLogicalIdReservation(zeroFirst));
	NativeGraphLogicalIdReservation zeroCount = reservation;
	zeroCount.node_ranges.front().count = 0U;
	EXPECT_FALSE(store->claimNativeGraphLogicalIdReservation(zeroCount));
	NativeGraphLogicalIdReservation unknownRange = reservation;
	unknownRange.node_ranges.front().first_id += reservation.node_ranges.front().count;
	EXPECT_FALSE(store->claimNativeGraphLogicalIdReservation(unknownRange));
	NativeGraphLogicalIdReservation wrongCount = reservation;
	wrongCount.node_ranges.front().count = 1U;
	EXPECT_FALSE(store->claimNativeGraphLogicalIdReservation(wrongCount));
	NativeGraphLogicalIdReservation duplicateRange = reservation;
	duplicateRange.node_ranges.push_back(reservation.node_ranges.front());
	EXPECT_FALSE(store->claimNativeGraphLogicalIdReservation(duplicateRange));
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation(reservation));

	const NativeGraphLogicalIdLease nodeLease = store->reserveNativeGraphLogicalIds(1U, 0U);
	NativeGraphLogicalIdReservation nodeCapability;
	nodeCapability.node_ranges.push_back(nodeLease.nodes);
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation(nodeCapability));
	const NativeGraphLogicalIdLease edgeLease = store->reserveNativeGraphLogicalIds(0U, 1U);
	NativeGraphLogicalIdReservation edgeCapability;
	edgeCapability.edge_ranges.push_back(edgeLease.edges);
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation(edgeCapability));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	EXPECT_EQ(snapshot.superblock().next_logical_node_id, nodeLease.nodes.first_id + nodeLease.nodes.count);
	EXPECT_EQ(snapshot.superblock().next_logical_edge_id, edgeLease.edges.first_id + edgeLease.edges.count);
	snapshot.release();
}

TEST_F(RelocatableMetadataStoreValidationTest, AbandoningLogicalIdCapabilitiesRequiresTheirExactIssuedRanges) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const NativeGraphLogicalIdReservation nodeReservation = store->acquireNativeGraphLogicalIds(1U, 0U, 1U);
	ASSERT_EQ(nodeReservation.node_ranges.size(), 1U);
	ASSERT_TRUE(nodeReservation.edge_ranges.empty());
	NativeGraphLogicalIdReservation wrongCount = nodeReservation;
	++wrongCount.node_ranges.front().count;
	store->abandonNativeGraphLogicalIdReservation(wrongCount);
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation(nodeReservation));

	const NativeGraphLogicalIdReservation edgeReservation = store->acquireNativeGraphLogicalIds(0U, 1U, 1U);
	ASSERT_TRUE(edgeReservation.node_ranges.empty());
	ASSERT_EQ(edgeReservation.edge_ranges.size(), 1U);
	NativeGraphLogicalIdReservation unknownRange = edgeReservation;
	++unknownRange.edge_ranges.front().first_id;
	store->abandonNativeGraphLogicalIdReservation(unknownRange);

	// An abandon request is best-effort, but it must never release a different
	// outstanding capability that shares only part of the caller-supplied shape.
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation(edgeReservation));
}

TEST_F(RelocatableMetadataStoreValidationTest, PreservesLogicalIdHighWaterMarksWhenDirectLeasePublicationFails) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store->reserveNativeGraphLogicalIds(1U, 1U, options)), BootstrapPublishInterrupted);
	MetadataSnapshotPin afterFailure = store->openSnapshot();
	EXPECT_EQ(afterFailure.superblock().next_logical_node_id, 1U);
	EXPECT_EQ(afterFailure.superblock().next_logical_edge_id, 1U);
	afterFailure.release();

	const NativeGraphLogicalIdLease lease = store->reserveNativeGraphLogicalIds(1U, 1U);
	EXPECT_EQ(lease.nodes, (NativeGraphLogicalIdLeaseRange{1U, 1U}));
	EXPECT_EQ(lease.edges, (NativeGraphLogicalIdLeaseRange{1U, 1U}));
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   DirectLogicalIdLeaseFinalSyncFailureReopensOnlyACompleteGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	io->sync();
	store.reset();

	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
	store = RelocatableMetadataStore::open(io);

	std::atomic<uint32_t> syncCalls{0U};
	{
		StorageIO::ScopedNativeSyncHookForTesting failFinalFence([&syncCalls](file_handle_t fd) {
			if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 1U) {
#ifdef _WIN32
				SetLastError(ERROR_WRITE_FAULT);
#else
				errno = EIO;
#endif
				return -1;
			}
			return portable_fsync(fd);
		});
		EXPECT_THROW(static_cast<void>(store->reserveNativeGraphLogicalIds(1U, 1U)), std::system_error);
	}
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);

	store.reset();
	io.reset();
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	const file_handle_t reopenedHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(reopenedHandle, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(dataFile, reopenedHandle, reopenedHandle);
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(2U));
	EXPECT_EQ(snapshot.superblock().next_logical_node_id, 2U);
	EXPECT_EQ(snapshot.superblock().next_logical_edge_id, 2U);
	snapshot.release();
	reopened.reset();
	io.reset();
	EXPECT_EQ(portable_close_rw(reopenedHandle), 0);
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   CachedLogicalIdLeaseFinalSyncFailureDoesNotExposeAnUndurableCachedRange) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	io->sync();
	store.reset();

	const file_handle_t nativeHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(dataFile, nativeHandle, nativeHandle);
	store = RelocatableMetadataStore::open(io);

	std::atomic<uint32_t> syncCalls{0U};
	{
		StorageIO::ScopedNativeSyncHookForTesting failFinalFence([&syncCalls](file_handle_t fd) {
			if (syncCalls.fetch_add(1U, std::memory_order_relaxed) == 1U) {
#ifdef _WIN32
				SetLastError(ERROR_WRITE_FAULT);
#else
				errno = EIO;
#endif
				return -1;
			}
			return portable_fsync(fd);
		});
		EXPECT_THROW(static_cast<void>(store->acquireNativeGraphLogicalIds(1U, 1U, 4U)), std::system_error);
	}
	EXPECT_GE(syncCalls.load(std::memory_order_relaxed), 2U);

	store.reset();
	io.reset();
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);

	const file_handle_t reopenedHandle = portable_open_rw(dataPath.string().c_str());
	ASSERT_NE(reopenedHandle, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(dataFile, reopenedHandle, reopenedHandle);
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin snapshot = reopened->openSnapshot();
	EXPECT_EQ(snapshot.superblock().generation, Generation(2U));
	EXPECT_EQ(snapshot.superblock().next_logical_node_id, 5U);
	EXPECT_EQ(snapshot.superblock().next_logical_edge_id, 5U);
	snapshot.release();

	const NativeGraphLogicalIdReservation retried = reopened->acquireNativeGraphLogicalIds(1U, 1U, 4U);
	ASSERT_EQ(retried.node_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{5U, 1U}}));
	ASSERT_EQ(retried.edge_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{5U, 1U}}));
	reopened->abandonNativeGraphLogicalIdReservation(retried);
	reopened.reset();
	io.reset();
	EXPECT_EQ(portable_close_rw(reopenedHandle), 0);
}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsExhaustedLogicalIdNamespacesBeforeStartingPublication) {
	Superblock exhausted = initialSuperblock();
	exhausted.next_logical_node_id = 0U;
	exhausted.next_logical_edge_id = 0U;
	auto store = RelocatableMetadataStore::initializeNew(io, exhausted);
	expectFormatError([&] { static_cast<void>(store->reserveNativeGraphLogicalIds(1U, 0U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(store->reserveNativeGraphLogicalIds(0U, 1U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(store->acquireNativeGraphLogicalIds(1U, 0U, 1U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(store->acquireNativeGraphLogicalIds(0U, 1U, 1U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableMetadataStoreValidationTest, BoundsLogicalIdLeasesAtTheFinalRepresentableIdentity) {
	Superblock finalIds = initialSuperblock();
	finalIds.next_logical_node_id = std::numeric_limits<uint64_t>::max();
	finalIds.next_logical_edge_id = std::numeric_limits<uint64_t>::max();
	auto store = RelocatableMetadataStore::initializeNew(io, finalIds);

	MetadataSnapshotPin before = store->openSnapshot();
	expectFormatError([&] { static_cast<void>(store->reserveNativeGraphLogicalIds(2U, 0U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(store->reserveNativeGraphLogicalIds(0U, 2U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), before.superblock());
	unchanged.release();

	const NativeGraphLogicalIdLease node = store->reserveNativeGraphLogicalIds(1U, 0U);
	EXPECT_EQ(node.nodes, (NativeGraphLogicalIdLeaseRange{std::numeric_limits<uint64_t>::max(), 1U}));
	const NativeGraphLogicalIdLease edge = store->reserveNativeGraphLogicalIds(0U, 1U);
	EXPECT_EQ(edge.edges, (NativeGraphLogicalIdLeaseRange{std::numeric_limits<uint64_t>::max(), 1U}));
	expectFormatError([&] { static_cast<void>(store->reserveNativeGraphLogicalIds(1U, 0U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(store->reserveNativeGraphLogicalIds(0U, 1U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	before.release();
}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsLogicalIdLeasesWhenTheCheckpointNamespaceIsExhausted) {
	Superblock exhaustedCheckpoint = initialSuperblock();
	exhaustedCheckpoint.checkpoint_lsn = LogSequenceNumber(std::numeric_limits<uint64_t>::max());
	auto store = RelocatableMetadataStore::initializeNew(io, exhaustedCheckpoint);
	expectFormatError([&] { static_cast<void>(store->reserveNativeGraphLogicalIds(1U, 0U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
	expectFormatError([&] { static_cast<void>(store->acquireNativeGraphLogicalIds(1U, 0U, 1U)); },
					  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   PublishesAnAlreadyLeasedLogicalIdCapabilityWithoutManufacturingDirectoryMutations) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const NativeGraphLogicalIdLease lease = store->reserveNativeGraphLogicalIds(1U, 0U);
	const NativeGraphLogicalIdReservation reservation{{lease.nodes}, {}};

	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	EXPECT_EQ(beforeSuperblock.next_logical_node_id, 2U);
	EXPECT_EQ(beforeSuperblock.next_logical_page_id, 1U);
	before.release();

	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
			noLargeValues, {}, LogSequenceNumber(3U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			{}, {}, {}, {}, {}, {}, {}, reservation));

	MetadataSnapshotPin after = store->openSnapshot();
	EXPECT_EQ(after.superblock().generation, Generation(beforeSuperblock.generation.value() + 1U));
	EXPECT_EQ(after.superblock().next_logical_node_id, beforeSuperblock.next_logical_node_id);
	EXPECT_EQ(after.superblock().next_logical_page_id, beforeSuperblock.next_logical_page_id);
	EXPECT_FALSE(store->findPageLocationOptional(after, PageId(1U)).has_value());
	after.release();

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(beforeSuperblock.generation.value() + 1U));
	EXPECT_EQ(recovered.superblock().next_logical_node_id, beforeSuperblock.next_logical_node_id);
	EXPECT_EQ(recovered.superblock().next_logical_page_id, beforeSuperblock.next_logical_page_id);
	EXPECT_FALSE(reopened->findPageLocationOptional(recovered, PageId(1U)).has_value());
	recovered.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsMalformedCompoundGraphPublicationPlansWithoutCallingMaterializersOrPublishing) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool graphPreparerCalled = false;
	const NativeGraphPageTransactionWithLargeValuesPreparer graphPreparer =
			[&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
								  std::span<EntityPageAppendBatchMaterialization>,
								  std::span<EntityPageNewPoolBatchMaterialization>,
								  std::span<NativeGraphPropertyPageReplacementMaterialization>,
								  std::span<NativeGraphPropertyPageBatchMaterialization>,
								  std::span<LargeValueMaterializationResult>) { graphPreparerCalled = true; };
	const NativeGraphDerivedStatePreparer derivedPreparer =
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {};

	MetadataSnapshotPin before = store->openSnapshot();
	const Generation beforeGeneration = before.superblock().generation;
	before.release();

	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, {}, LogSequenceNumber(2U), graphPreparer, {}));
			},
			FormatError::FERR_INVALID_HEADER);

	NativeGraphDerivedStateMutation missingDerivedPreparer;
	missingDerivedPreparer.reused_large_values.push_back({});
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, missingDerivedPreparer, LogSequenceNumber(2U), graphPreparer, {}));
			},
			FormatError::FERR_INVALID_HEADER);

	NativeGraphDerivedStateMutation missingVectorBootstrapPreparer;
	missingVectorBootstrapPreparer.vector_bootstraps.push_back({});
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, missingVectorBootstrapPreparer, LogSequenceNumber(2U), graphPreparer,
						derivedPreparer));
			},
			FormatError::FERR_INVALID_HEADER);

	const std::array<NativeGraphLargeValueMutation, 1U> malformedLargeValueWork{{}};
	const NativeGraphVectorDeltaPreparer unexpectedVectorDeltaPreparer =
			[](std::vector<NativeGraphVectorDeltaMaterialization> &) {};
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						malformedLargeValueWork, {}, LogSequenceNumber(2U), graphPreparer, {}, {},
						unexpectedVectorDeltaPreparer));
			},
			FormatError::FERR_INVALID_HEADER);

	// A nonempty graph batch must not make an otherwise mismatched Vector
	// callback pair acceptable. These reject before validating or materializing
	// the deliberately incomplete Entity request below.
	const std::array<EntityPageNewPoolBatchRequest, 1U> nonemptyGraphWork{{}};
	const NativeGraphVectorBootstrapPreparer unexpectedVectorBootstrapPreparer =
			[](std::vector<NativeGraphVectorBootstrapMaterialization> &) {};
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, nonemptyGraphWork, noPropertyReplacements,
						noPropertyPools, noLargeValues, {}, LogSequenceNumber(2U), graphPreparer, {},
						unexpectedVectorBootstrapPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, nonemptyGraphWork, noPropertyReplacements,
						noPropertyPools, noLargeValues, {}, LogSequenceNumber(2U), graphPreparer, {}, {},
						unexpectedVectorDeltaPreparer));
			},
			FormatError::FERR_INVALID_HEADER);

	// These plans carry their real typed preparers. They must be rejected for
	// their malformed content before any graph-page materializer can run.
	NativeGraphDerivedStateMutation malformedIndexRoot;
	malformedIndexRoot.index_tree_mutations.push_back({});
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, malformedIndexRoot, LogSequenceNumber(2U), graphPreparer, derivedPreparer));
			},
			FormatError::FERR_INVALID_HEADER);

	const NativeGraphVectorBootstrapPreparer vectorBootstrapPreparer =
			[](std::vector<NativeGraphVectorBootstrapMaterialization> &) {};
	NativeGraphDerivedStateMutation malformedVectorBootstrap;
	malformedVectorBootstrap.vector_bootstraps.push_back({});
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, malformedVectorBootstrap, LogSequenceNumber(2U), graphPreparer, derivedPreparer,
						vectorBootstrapPreparer));
			},
			FormatError::FERR_INVALID_HEADER);

	const NativeGraphVectorDeltaPreparer vectorDeltaPreparer =
			[](std::vector<NativeGraphVectorDeltaMaterialization> &) {};
	NativeGraphDerivedStateMutation malformedVectorDelta;
	malformedVectorDelta.vector_deltas.push_back({});
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, malformedVectorDelta, LogSequenceNumber(2U), graphPreparer, derivedPreparer, {},
						vectorDeltaPreparer));
			},
			FormatError::FERR_INVALID_HEADER);

	const PageRetirement validShapeRetirement{{PageId(17U), SegmentId(7U), RSS_PAGE_POOL_FIRST_DATA_FRAME,
													 Generation(1U), 0U},
										 1U, PageOwnerKind::POK_LARGE_VALUE_DATA, LogSequenceNumber(1U)};
	const std::array<PageRetirement, 1U> retirementWithoutManifestIds{{validShapeRetirement}};
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, {}, LogSequenceNumber(2U), graphPreparer, {}, {}, {},
						retirementWithoutManifestIds));
			},
			FormatError::FERR_INVALID_HEADER);

	const std::array<LargeValueId, 2U> unorderedErasedIds{{LargeValueId(2U), LargeValueId(1U)}};
	const ExistingPoolPagesRetirementPreparer retirementPreparer =
			[](std::span<const ExistingPoolPageRetirementBatch>) {};
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, {}, LogSequenceNumber(2U), graphPreparer, {}, {}, {},
						retirementWithoutManifestIds, unorderedErasedIds, retirementPreparer));
			},
			FormatError::FERR_INVALID_HEADER);

	const std::array<LargeValueId, 1U> erasedManifestId{{LargeValueId(2U)}};
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, {}, LogSequenceNumber(2U), graphPreparer, {}, {}, {},
						retirementWithoutManifestIds, erasedManifestId, retirementPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
	EXPECT_FALSE(graphPreparerCalled);

	static_cast<void>(store->reserveNativeGraphLogicalIds(1U, 0U));
	MetadataSnapshotPin beforeStalePublication = store->openSnapshot();
	const Generation generationBeforeStalePublication = beforeStalePublication.superblock().generation;
	beforeStalePublication.release();
	NativeGraphDerivedStateMutation staleDerivedState;
	staleDerivedState.index_tree_mutations.push_back({
			.index_id = 77U,
			.new_pool_requests = {{PhysicalSlotId(40U), PhysicalSlotId(40U), 77U, 1U}},
			.replacement_root = {{0U, 0U, true}},
	});
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, staleDerivedState, LogSequenceNumber(3U), graphPreparer, derivedPreparer, {}, {}, {},
						{}, {},
						{}, {}, Generation(beforeGeneration.value())));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(graphPreparerCalled);

	MetadataSnapshotPin afterRejectedPlans = store->openSnapshot();
	EXPECT_EQ(afterRejectedPlans.superblock().generation, generationBeforeStalePublication);
	afterRejectedPlans.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin afterReopen = reopened->openSnapshot();
	EXPECT_EQ(afterReopen.superblock().generation, generationBeforeStalePublication);
	afterReopen.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsMalformedDerivedTreeShapesBeforeAcquiringOrPublishingGraphPages) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool graphPreparerCalled = false;
	const NativeGraphPageTransactionWithLargeValuesPreparer graphPreparer =
			[&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
								  std::span<EntityPageAppendBatchMaterialization>,
								  std::span<EntityPageNewPoolBatchMaterialization>,
								  std::span<NativeGraphPropertyPageReplacementMaterialization>,
								  std::span<NativeGraphPropertyPageBatchMaterialization>,
								  std::span<LargeValueMaterializationResult>) { graphPreparerCalled = true; };
	const NativeGraphDerivedStatePreparer derivedPreparer =
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {};
	const auto expectRejected = [&](const NativeGraphDerivedStateMutation &derived) {
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyPools, noLargeValues, derived, LogSequenceNumber(2U), graphPreparer,
							derivedPreparer));
				},
				FormatError::FERR_INVALID_HEADER);
		EXPECT_FALSE(graphPreparerCalled);
	};

	NativeGraphDerivedStateMutation malformedPool;
	NativeGraphIndexTreeMutation invalidExistingPool;
	invalidExistingPool.index_id = 1U;
	invalidExistingPool.expected_root = IndexRootLocation{1U, PageId(1U), Generation(1U)};
	invalidExistingPool.pool_requests.push_back({});
	malformedPool.index_tree_mutations.push_back(std::move(invalidExistingPool));
	expectRejected(malformedPool);

	NativeGraphDerivedStateMutation malformedNewPool;
	NativeGraphIndexTreeMutation invalidNewPool;
	invalidNewPool.index_id = 2U;
	invalidNewPool.new_pool_requests.push_back({});
	invalidNewPool.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
	malformedNewPool.index_tree_mutations.push_back(std::move(invalidNewPool));
	expectRejected(malformedNewPool);

	NativeGraphDerivedStateMutation malformedFreshRootReference;
	NativeGraphIndexTreeMutation invalidFreshRootReference;
	invalidFreshRootReference.index_id = 3U;
	invalidFreshRootReference.new_pool_requests.push_back({PhysicalSlotId(3U), PhysicalSlotId(3U), 3U, 1U});
	invalidFreshRootReference.replacement_root = NativeGraphIndexTreeAppendedPageReference{1U, 0U, true};
	malformedFreshRootReference.index_tree_mutations.push_back(std::move(invalidFreshRootReference));
	expectRejected(malformedFreshRootReference);

	NativeGraphDerivedStateMutation malformedStableRoot;
	NativeGraphIndexTreeMutation invalidStableRoot;
	invalidStableRoot.index_id = 4U;
	invalidStableRoot.expected_root = IndexRootLocation{4U, PageId(4U), Generation(1U)};
	invalidStableRoot.pool_requests.push_back({SegmentId(4U), {}, 1U, {}});
	invalidStableRoot.stable_replacement_root = PageId{};
	malformedStableRoot.index_tree_mutations.push_back(std::move(invalidStableRoot));
	expectRejected(malformedStableRoot);

	NativeGraphDerivedStateMutation malformedFreshRootPoolMix;
	NativeGraphIndexTreeMutation invalidFreshRootPoolMix;
	invalidFreshRootPoolMix.index_id = 5U;
	invalidFreshRootPoolMix.pool_requests.push_back({SegmentId(5U), {}, 1U, {}});
	malformedFreshRootPoolMix.index_tree_mutations.push_back(std::move(invalidFreshRootPoolMix));
	expectRejected(malformedFreshRootPoolMix);

	NativeGraphDerivedStateMutation duplicateReplacement;
	NativeGraphIndexTreeMutation duplicateReplacementMutation;
	duplicateReplacementMutation.index_id = 6U;
	duplicateReplacementMutation.expected_root = IndexRootLocation{6U, PageId(6U), Generation(1U)};
	const PageLocation repeatedReplacement{PageId(61U), SegmentId(6U), RSS_PAGE_POOL_FIRST_DATA_FRAME,
									  Generation(1U), 0U};
	duplicateReplacementMutation.pool_requests.push_back(
			{SegmentId(6U), {repeatedReplacement, repeatedReplacement}, 0U, {}});
	duplicateReplacement.index_tree_mutations.push_back(std::move(duplicateReplacementMutation));
	expectRejected(duplicateReplacement);

	NativeGraphDerivedStateMutation duplicateRetirement;
	NativeGraphIndexTreeMutation duplicateRetirementMutation;
	duplicateRetirementMutation.index_id = 7U;
	duplicateRetirementMutation.expected_root = IndexRootLocation{7U, PageId(7U), Generation(1U)};
	const IndexTreePageRetirement repeatedRetirement{
			{PageId(71U), SegmentId(7U), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U},
			LogSequenceNumber(1U)};
	duplicateRetirementMutation.pool_requests.push_back(
			{SegmentId(7U), {}, 0U, {repeatedRetirement, repeatedRetirement}});
	duplicateRetirement.index_tree_mutations.push_back(std::move(duplicateRetirementMutation));
	expectRejected(duplicateRetirement);

	NativeGraphDerivedStateMutation retirementWithReplacement;
	NativeGraphIndexTreeMutation invalidRootRetirement;
	invalidRootRetirement.index_id = 8U;
	invalidRootRetirement.expected_root = IndexRootLocation{8U, PageId(8U), Generation(1U)};
	invalidRootRetirement.retire_root = true;
	invalidRootRetirement.pool_requests.push_back({SegmentId(8U), {repeatedReplacement}, 0U, {}});
	retirementWithReplacement.index_tree_mutations.push_back(std::move(invalidRootRetirement));
	expectRejected(retirementWithReplacement);

	NativeGraphDerivedStateMutation retirementWithoutPinnedRoot;
	NativeGraphIndexTreeMutation invalidPinnedRootRetirement;
	invalidPinnedRootRetirement.index_id = 9U;
	invalidPinnedRootRetirement.expected_root = IndexRootLocation{9U, PageId(9U), Generation(1U)};
	invalidPinnedRootRetirement.retire_root = true;
	invalidPinnedRootRetirement.pool_requests.push_back({SegmentId(9U), {}, 0U, {repeatedRetirement}});
	retirementWithoutPinnedRoot.index_tree_mutations.push_back(std::move(invalidPinnedRootRetirement));
	expectRejected(retirementWithoutPinnedRoot);

	MetadataSnapshotPin afterRejected = store->openSnapshot();
	EXPECT_EQ(afterRejected.superblock().generation, Generation(1U));
	afterRejected.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsMalformedReusedLargeValueAndVectorBootstrapPlansBeforeStartingPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	const NativeGraphPageTransactionWithLargeValuesPreparer graphPreparer =
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>,
			   std::span<LargeValueMaterializationResult>) {};
	const NativeGraphDerivedStatePreparer derivedPreparer =
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {};
	const NativeGraphVectorBootstrapPreparer vectorPreparer =
			[](std::vector<NativeGraphVectorBootstrapMaterialization> &) {};

	const std::vector<std::byte> emptyBytes;
	const LargeValueRef reusable{LargeValueId(1U), Generation(1U), 0U, graph::utils::Sha256::digest(emptyBytes),
									 LargeValueKind::LVK_BYTES, LargeValueCodec::LVC_RAW,
									 "application/octet-stream"};
	NativeGraphDerivedStateMutation duplicateReuse;
	duplicateReuse.reused_large_values = {reusable, reusable};
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, duplicateReuse, LogSequenceNumber(2U), graphPreparer, derivedPreparer));
			},
			FormatError::FERR_INVALID_HEADER);

	const auto bootstrapPlan = [](uint32_t dimensions, uint32_t rawPageCount) {
		NativeGraphDerivedStateMutation result;
		result.vector_bootstraps.push_back(
				{91U, 92U, 1U, 1U, dimensions, VectorDistanceMetric::VDM_L2, rawPageCount,
				 PhysicalSlotId(40U), PhysicalSlotId(43U)});
		return result;
	};
	expectFormatError(
			[&] {
				const NativeGraphDerivedStateMutation oversizedRecord = bootstrapPlan(2048U, 1U);
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, oversizedRecord, LogSequenceNumber(2U), graphPreparer, derivedPreparer,
						vectorPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				const NativeGraphDerivedStateMutation noncanonicalPageCount = bootstrapPlan(1U, 2U);
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, noncanonicalPageCount, LogSequenceNumber(2U), graphPreparer, derivedPreparer,
						vectorPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				const NativeGraphDerivedStateMutation missingMappingRoots = bootstrapPlan(1U, 1U);
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, missingMappingRoots, LogSequenceNumber(2U), graphPreparer, derivedPreparer,
						vectorPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
	// Validate every persisted bootstrap identity component before attempting
	// first-use mapping-root planning or invoking the typed Vector materializer.
	const auto expectMalformedBootstrap = [&](const NativeGraphDerivedStateMutation &candidate) {
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyPools, noLargeValues, candidate, LogSequenceNumber(2U), graphPreparer,
							derivedPreparer, vectorPreparer));
				},
				FormatError::FERR_INVALID_HEADER);
	};
	NativeGraphDerivedStateMutation zeroIndex = bootstrapPlan(1U, 1U);
	zeroIndex.vector_bootstraps.front().index_id = 0U;
	expectMalformedBootstrap(zeroIndex);
	NativeGraphDerivedStateMutation duplicateMappingId = bootstrapPlan(1U, 1U);
	duplicateMappingId.vector_bootstraps.front().vector_node_mapping_index_id = 91U;
	expectMalformedBootstrap(duplicateMappingId);
	NativeGraphDerivedStateMutation zeroFirstVectorId = bootstrapPlan(1U, 1U);
	zeroFirstVectorId.vector_bootstraps.front().first_vector_id = 0U;
	expectMalformedBootstrap(zeroFirstVectorId);
	NativeGraphDerivedStateMutation zeroVectorCount = bootstrapPlan(1U, 1U);
	zeroVectorCount.vector_bootstraps.front().vector_count = 0U;
	expectMalformedBootstrap(zeroVectorCount);
	NativeGraphDerivedStateMutation zeroDimensions = bootstrapPlan(1U, 1U);
	zeroDimensions.vector_bootstraps.front().dimensions = 0U;
	expectMalformedBootstrap(zeroDimensions);
	NativeGraphDerivedStateMutation unknownMetric = bootstrapPlan(1U, 1U);
	unknownMetric.vector_bootstraps.front().metric = static_cast<VectorDistanceMetric>(255U);
	expectMalformedBootstrap(unknownMetric);
	NativeGraphDerivedStateMutation zeroRawPages = bootstrapPlan(1U, 1U);
	zeroRawPages.vector_bootstraps.front().raw_page_count = 0U;
	expectMalformedBootstrap(zeroRawPages);
	NativeGraphDerivedStateMutation invalidSlots = bootstrapPlan(1U, 1U);
	invalidSlots.vector_bootstraps.front().first_slot = PhysicalSlotId{};
	expectMalformedBootstrap(invalidSlots);
	NativeGraphDerivedStateMutation reversedSlots = bootstrapPlan(1U, 1U);
	reversedSlots.vector_bootstraps.front().first_slot = PhysicalSlotId(44U);
	expectMalformedBootstrap(reversedSlots);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
	unchanged.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsAReusedLargeValueWithoutAReachableManifestBeforePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool graphPreparerCalled = false;
	const NativeGraphPageTransactionWithLargeValuesPreparer graphPreparer =
			[&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
								  std::span<EntityPageAppendBatchMaterialization>,
								  std::span<EntityPageNewPoolBatchMaterialization>,
								  std::span<NativeGraphPropertyPageReplacementMaterialization>,
								  std::span<NativeGraphPropertyPageBatchMaterialization>,
								  std::span<LargeValueMaterializationResult>) { graphPreparerCalled = true; };
	const NativeGraphDerivedStatePreparer derivedPreparer =
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {};

	const std::vector<std::byte> emptyBytes;
	NativeGraphDerivedStateMutation derivedState;
	derivedState.reused_large_values.push_back(
			{LargeValueId(1U), Generation(1U), 0U, graph::utils::Sha256::digest(emptyBytes),
			 LargeValueKind::LVK_BYTES, LargeValueCodec::LVC_RAW, "application/octet-stream"});

	MetadataSnapshotPin before = store->openSnapshot();
	const Generation beforeGeneration = before.superblock().generation;
	before.release();
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, derivedState, LogSequenceNumber(2U), graphPreparer, derivedPreparer));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(graphPreparerCalled);

	MetadataSnapshotPin after = store->openSnapshot();
	EXPECT_EQ(after.superblock().generation, beforeGeneration);
	after.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, beforeGeneration);
	recovered.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsAReusedLargeValueWhoseImmutableReferenceDoesNotMatchTheCurrentManifest) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	LargeValueStore values(io, *store);
	LargeValueWriteOptions writeOptions;
	writeOptions.first_data_slot = PhysicalSlotId(40U);
	writeOptions.last_data_slot = PhysicalSlotId(60U);
	writeOptions.first_manifest_slot = PhysicalSlotId(61U);
	writeOptions.last_manifest_slot = PhysicalSlotId(80U);
	writeOptions.kind = LargeValueKind::LVK_BYTES;
	writeOptions.media_type = "application/octet-stream";
	const std::vector<std::byte> payload{std::byte{0x4DU}};
	const LargeValueWriteResult written = values.write(payload, writeOptions, LogSequenceNumber(2U));

	LargeValueRef mismatched = written.reference;
	++mismatched.logical_bytes;
	NativeGraphDerivedStateMutation derivedState;
	derivedState.reused_large_values.push_back(mismatched);
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool graphPreparerCalled = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, derivedState, LogSequenceNumber(3U),
						[&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
													   std::span<EntityPageAppendBatchMaterialization>,
													   std::span<EntityPageNewPoolBatchMaterialization>,
													   std::span<NativeGraphPropertyPageReplacementMaterialization>,
													   std::span<NativeGraphPropertyPageBatchMaterialization>,
													   std::span<LargeValueMaterializationResult>) { graphPreparerCalled = true; },
						[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(graphPreparerCalled);

	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(current.superblock().generation, written.superblock.generation);
	EXPECT_EQ(values.read(current, written.reference), payload);
	current.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedValues(io, *reopened);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, written.superblock.generation);
	EXPECT_EQ(reopenedValues.read(recovered, written.reference), payload);
	recovered.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RevalidatesAReachableReusedLargeValueBeforePublishingTheGraphGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	LargeValueStore values(io, *store);
	LargeValueWriteOptions writeOptions;
	writeOptions.first_data_slot = PhysicalSlotId(40U);
	writeOptions.last_data_slot = PhysicalSlotId(60U);
	writeOptions.first_manifest_slot = PhysicalSlotId(61U);
	writeOptions.last_manifest_slot = PhysicalSlotId(80U);
	writeOptions.kind = LargeValueKind::LVK_BYTES;
	writeOptions.media_type = "application/octet-stream";
	const std::vector<std::byte> payload{std::byte{0x4DU}};
	const LargeValueWriteResult written = values.write(payload, writeOptions, LogSequenceNumber(2U));

	NativeGraphDerivedStateMutation derivedState;
	derivedState.reused_large_values.push_back(written.reference);
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool graphPreparerCalled = false;
	bool derivedPreparerCalled = false;
	const Superblock published = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
			noLargeValues, derivedState, LogSequenceNumber(3U),
			[&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
								   std::span<EntityPageAppendBatchMaterialization>,
								   std::span<EntityPageNewPoolBatchMaterialization>,
								   std::span<NativeGraphPropertyPageReplacementMaterialization>,
								   std::span<NativeGraphPropertyPageBatchMaterialization>,
								   std::span<LargeValueMaterializationResult>) { graphPreparerCalled = true; },
			[&derivedPreparerCalled](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {
				derivedPreparerCalled = true;
			});
	EXPECT_TRUE(graphPreparerCalled);
	EXPECT_TRUE(derivedPreparerCalled);
	EXPECT_EQ(published.generation, Generation(written.superblock.generation.value() + 1U));

	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(current.superblock().generation, published.generation);
	EXPECT_EQ(values.read(current, written.reference), payload);
	current.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	LargeValueStore reopenedValues(io, *reopened);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, published.generation);
	EXPECT_EQ(reopenedValues.read(recovered, written.reference), payload);
	recovered.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsTypedEntityMaterializerFrameCollisionsWithoutPublishingTheGraphGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

	CatalogSlotReservationRequest reservation;
	reservation.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
	reservation.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	reservation.owner_id = 17U;
	reservation.segment_generation = Generation(1U);
	reservation.state = SegmentState::SGS_SEALED;
	reservation.require_absent = true;
	const std::array<EntityPageNewPoolBatchRequest, 2U> newPools{{
			{reservation, PageId(101U), PhysicalSlotId(40U), PhysicalSlotId(40U), {}, {},
			 PageOwnerKind::POK_ENTITY_LABEL},
			{reservation, PageId(102U), PhysicalSlotId(40U), PhysicalSlotId(40U), {}, {},
			 PageOwnerKind::POK_ENTITY_LABEL},
	}};

	bool graphPreparerCalled = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, newPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, {}, LogSequenceNumber(2U),
						[&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
											  std::span<EntityPageAppendBatchMaterialization>,
											  std::span<EntityPageNewPoolBatchMaterialization> materializations,
											  std::span<NativeGraphPropertyPageReplacementMaterialization>,
											  std::span<NativeGraphPropertyPageBatchMaterialization>,
											  std::span<LargeValueMaterializationResult>) {
							graphPreparerCalled = true;
							ASSERT_EQ(materializations.size(), 2U);
							// Typed pages must not share a frame even when they share one pool.
							for (EntityPageNewPoolBatchMaterialization &materialization: materializations) {
								materialization.initial_location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							}
						},
						{}));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(graphPreparerCalled);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
	expectFormatError([&] { static_cast<void>(store->findCatalog(unchanged, reservation.segment_id)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(store->findPageLocationOptional(unchanged, PageId(101U)).has_value());
	EXPECT_FALSE(store->findPageLocationOptional(unchanged, PageId(102U)).has_value());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
	unchanged.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(1U));
	expectFormatError([&] { static_cast<void>(reopened->findCatalog(recovered, reservation.segment_id)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	recovered.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsUnmaterializedEntityFramesWithoutPublishingTheGraphGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

	CatalogSlotReservationRequest reservation;
	reservation.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
	reservation.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	reservation.owner_id = 17U;
	reservation.segment_generation = Generation(1U);
	reservation.state = SegmentState::SGS_SEALED;
	reservation.require_absent = true;
	const std::array<EntityPageNewPoolBatchRequest, 1U> newPools{{
			{reservation, PageId(101U), PhysicalSlotId(40U), PhysicalSlotId(40U), {}, {},
			 PageOwnerKind::POK_ENTITY_LABEL},
	}};

	bool graphPreparerCalled = false;
	EXPECT_THROW(
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, newPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, {}, LogSequenceNumber(2U),
						[&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
										  std::span<EntityPageAppendBatchMaterialization>,
										  std::span<EntityPageNewPoolBatchMaterialization> materializations,
										  std::span<NativeGraphPropertyPageReplacementMaterialization>,
										  std::span<NativeGraphPropertyPageBatchMaterialization>,
										  std::span<LargeValueMaterializationResult>) {
							graphPreparerCalled = true;
							ASSERT_EQ(materializations.size(), 1U);
							// Deliberately leave the coordinator-assigned pool and frame unwritten.
						},
						{})),
				FormatException);
	EXPECT_TRUE(graphPreparerCalled);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
	expectFormatError([&] { static_cast<void>(store->findCatalog(unchanged, reservation.segment_id)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(store->findPageLocationOptional(unchanged, PageId(101U)).has_value());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
	unchanged.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(1U));
	expectFormatError([&] { static_cast<void>(reopened->findCatalog(recovered, reservation.segment_id)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(reopened->findPageLocationOptional(recovered, PageId(101U)).has_value());
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	recovered.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsUnmaterializedIndexFramesWithoutPublishingTheGraphGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

	NativeGraphDerivedStateMutation derived;
	NativeGraphIndexTreeMutation tree;
	tree.index_id = 17U;
	tree.new_pool_requests.push_back({PhysicalSlotId(40U), PhysicalSlotId(40U), tree.index_id, 1U});
	tree.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
	derived.index_tree_mutations.push_back(std::move(tree));

	bool derivedPreparerCalled = false;
	EXPECT_THROW(
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, derived, LogSequenceNumber(2U),
						[](std::span<EntityPageReplacementMaterialization>,
						   std::span<EntityPageAppendBatchMaterialization>,
						   std::span<EntityPageNewPoolBatchMaterialization>,
						   std::span<NativeGraphPropertyPageReplacementMaterialization>,
						   std::span<NativeGraphPropertyPageBatchMaterialization>,
						   std::span<LargeValueMaterializationResult>) {},
						[&derivedPreparerCalled](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
							derivedPreparerCalled = true;
							ASSERT_EQ(batches.size(), 1U);
							ASSERT_EQ(batches.front().size(), 1U);
							batches.front().front().appended_locations.front().page_index =
									RSS_PAGE_POOL_FIRST_DATA_FRAME;
							// Deliberately leave the assigned pool and B+Tree frame unwritten.
						})),
				FormatException);
	EXPECT_TRUE(derivedPreparerCalled);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
	EXPECT_FALSE(store->findIndexRootOptional(unchanged, 17U).has_value());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
	unchanged.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(1U));
	EXPECT_FALSE(reopened->findIndexRootOptional(recovered, 17U).has_value());
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	recovered.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsVectorBootstrapPreparerIdentityChangesWithoutPublishingTheGraphGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

	NativeGraphDerivedStateMutation derived;
	for (const uint64_t indexId: {17U, 18U}) {
		NativeGraphIndexTreeMutation mapping;
		mapping.index_id = indexId;
		mapping.new_pool_requests.push_back(
				{PhysicalSlotId(40U + indexId - 17U), PhysicalSlotId(40U + indexId - 17U), indexId, 1U});
		mapping.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
		derived.index_tree_mutations.push_back(std::move(mapping));
	}
	derived.vector_bootstraps.push_back(
			{17U, 18U, 1U, 1U, 2U, VectorDistanceMetric::VDM_L2, 1U, PhysicalSlotId(42U), PhysicalSlotId(43U)});

	bool vectorPreparerCalled = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, derived, LogSequenceNumber(2U),
						[](std::span<EntityPageReplacementMaterialization>,
						   std::span<EntityPageAppendBatchMaterialization>,
						   std::span<EntityPageNewPoolBatchMaterialization>,
						   std::span<NativeGraphPropertyPageReplacementMaterialization>,
						   std::span<NativeGraphPropertyPageBatchMaterialization>,
						   std::span<LargeValueMaterializationResult>) {},
						[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
							ASSERT_EQ(batches.size(), 2U);
							for (std::vector<IndexTreeMutationPoolBatch> &mapping: batches) {
								ASSERT_EQ(mapping.size(), 1U);
								ASSERT_EQ(mapping.front().appended_locations.size(), 1U);
								mapping.front().appended_locations.front().page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							}
						},
						[&vectorPreparerCalled](std::vector<NativeGraphVectorBootstrapMaterialization> &materializations) {
							vectorPreparerCalled = true;
							ASSERT_EQ(materializations.size(), 1U);
							// Mapping roots and all logical Vector metadata belong to the coordinator.
							++materializations.front().mutation.dimensions;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(vectorPreparerCalled);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
	EXPECT_FALSE(store->findIndexRootOptional(unchanged, 17U).has_value());
	EXPECT_FALSE(store->findIndexRootOptional(unchanged, 18U).has_value());
	EXPECT_FALSE(store->findVectorManifestOptional(unchanged, 17U).has_value());
	for (const uint64_t slot: {40U, 41U, 42U, 43U}) {
		EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(slot)));
	}
	unchanged.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(1U));
	EXPECT_FALSE(reopened->findVectorManifestOptional(recovered, 17U).has_value());
	recovered.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsUnmaterializedVectorFramesWithoutPublishingTheGraphGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

	NativeGraphDerivedStateMutation derived;
	for (const uint64_t indexId: {47U, 48U}) {
		NativeGraphIndexTreeMutation mapping;
		mapping.index_id = indexId;
		mapping.new_pool_requests.push_back(
				{PhysicalSlotId(70U + indexId - 47U), PhysicalSlotId(70U + indexId - 47U), indexId, 1U});
		mapping.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
		derived.index_tree_mutations.push_back(std::move(mapping));
	}
	derived.vector_bootstraps.push_back(
			{47U, 48U, 1U, 1U, 2U, VectorDistanceMetric::VDM_L2, 1U, PhysicalSlotId(72U), PhysicalSlotId(73U)});

	SlottedPagePool indexPages(io, *store);
	BTreePageStore indexes(indexPages, *store);
	const std::array<uint64_t, 2U> mappingIds{{47U, 48U}};
	bool vectorPreparerCalled = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, derived, LogSequenceNumber(2U),
						[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
						   std::span<EntityPageNewPoolBatchMaterialization>,
						   std::span<NativeGraphPropertyPageReplacementMaterialization>,
						   std::span<NativeGraphPropertyPageBatchMaterialization>,
						   std::span<LargeValueMaterializationResult>) {},
						[&indexes, &mappingIds](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
							materializeInitialIndexBatches(indexes, batches, mappingIds, LogSequenceNumber(2U));
						},
						[&vectorPreparerCalled](std::vector<NativeGraphVectorBootstrapMaterialization> &plans) {
							vectorPreparerCalled = true;
							ASSERT_EQ(plans.size(), 1U);
							NativeGraphVectorBootstrapMaterialization &plan = plans.front();
							plan.raw_pages.front().page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							plan.manifest_page.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							plan.vector_locations = {{plan.mutation.first_vector_id, plan.raw_pages.front().page_id, 0U, 1U}};
							// Deliberately leave the assigned Raw and Manifest frames unwritten.
						}));
			},
			FormatError::FERR_BUFFER_TOO_SMALL);
	EXPECT_TRUE(vectorPreparerCalled);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
	EXPECT_FALSE(store->findIndexRootOptional(unchanged, 47U).has_value());
	EXPECT_FALSE(store->findIndexRootOptional(unchanged, 48U).has_value());
	EXPECT_FALSE(store->findVectorManifestOptional(unchanged, 47U).has_value());
	for (const uint64_t slot: {70U, 71U, 72U, 73U}) {
		EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(slot)));
	}
	unchanged.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(1U));
	EXPECT_FALSE(reopened->findVectorManifestOptional(recovered, 47U).has_value());
	recovered.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsUnmaterializedVectorDeltaFramesWithoutPublishingTheGraphGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	constexpr uint64_t INDEX_ID = 57U;
	constexpr uint64_t REVERSE_INDEX_ID = 58U;
	constexpr uint64_t DELTA_INDEX_ID = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | INDEX_ID;

	NativeGraphDerivedStateMutation bootstrap;
	for (const uint64_t indexId: {INDEX_ID, REVERSE_INDEX_ID}) {
		NativeGraphIndexTreeMutation mapping;
		mapping.index_id = indexId;
		mapping.new_pool_requests.push_back(
				{PhysicalSlotId(80U + indexId - INDEX_ID), PhysicalSlotId(80U + indexId - INDEX_ID), indexId, 1U});
		mapping.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
		bootstrap.index_tree_mutations.push_back(std::move(mapping));
	}
	bootstrap.vector_bootstraps.push_back(
			{INDEX_ID, REVERSE_INDEX_ID, 1U, 1U, 2U, VectorDistanceMetric::VDM_L2, 1U, PhysicalSlotId(82U),
			 PhysicalSlotId(83U)});
	SlottedPagePool indexPages(io, *store);
	BTreePageStore indexes(indexPages, *store);
	VectorPageStore rawPages(io, *store);
	VectorManifestStore manifests(io, *store);
	const std::array<uint64_t, 2U> bootstrapIndexIds{{INDEX_ID, REVERSE_INDEX_ID}};
	static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools, noLargeValues,
			bootstrap, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[&indexes, &bootstrapIndexIds](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				materializeInitialIndexBatches(indexes, batches, bootstrapIndexIds, LogSequenceNumber(2U));
			},
			[&rawPages, &manifests](std::vector<NativeGraphVectorBootstrapMaterialization> &plans) {
				materializeInitialVectorBootstrap(rawPages, manifests, plans, LogSequenceNumber(2U));
			}));

	MetadataSnapshotPin beforeDelta = store->openSnapshot();
	const IndexRootLocation nodeRoot = store->findIndexRoot(beforeDelta, INDEX_ID);
	const IndexRootLocation reverseRoot = store->findIndexRoot(beforeDelta, REVERSE_INDEX_ID);
	const VectorManifestLocation manifest = store->findVectorManifest(beforeDelta, INDEX_ID);
	const PageLocation manifestPage = store->findPageLocation(beforeDelta, manifest.manifest_page_id);
	const CatalogEntry manifestPool = store->findCatalog(beforeDelta, manifestPage.segment_id);
	const std::array<std::byte, 1U> key{{std::byte{0x01}}};
	const BTreeRootLeafMutationPlan nodePlan = indexes.planRootLeafUpsert(beforeDelta, INDEX_ID, key, 1U);
	const BTreeRootLeafMutationPlan reversePlan = indexes.planRootLeafUpsert(beforeDelta, REVERSE_INDEX_ID, key, 1U);
	const Generation bootstrapGeneration = beforeDelta.superblock().generation;
	beforeDelta.release();

	const auto replacementMutation = [](const BTreeRootLeafMutationPlan &plan) {
		NativeGraphIndexTreeMutation mutation;
		mutation.index_id = plan.expected_root.index_id;
		mutation.expected_root = plan.expected_root;
		mutation.pool_requests.push_back({plan.expected_pool.segment_id, {plan.expected_location}, 0U, {}});
		mutation.stable_replacement_root = plan.expected_location.page_id;
		return mutation;
	};
	NativeGraphDerivedStateMutation deltaDerived;
	deltaDerived.index_tree_mutations.push_back(replacementMutation(nodePlan));
	deltaDerived.index_tree_mutations.push_back(replacementMutation(reversePlan));
	NativeGraphIndexTreeMutation deltaMapping;
	deltaMapping.index_id = DELTA_INDEX_ID;
	deltaMapping.new_pool_requests.push_back({PhysicalSlotId(84U), PhysicalSlotId(84U), DELTA_INDEX_ID, 1U});
	deltaMapping.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
	deltaDerived.index_tree_mutations.push_back(std::move(deltaMapping));
	NativeGraphVectorDeltaMutation delta;
	delta.index_id = INDEX_ID;
	delta.vector_node_mapping_index_id = REVERSE_INDEX_ID;
	delta.delta_mapping_index_id = DELTA_INDEX_ID;
	delta.expected_node_mapping_root = nodeRoot;
	delta.expected_vector_node_mapping_root = reverseRoot;
	delta.expected_manifest_pool = manifestPool;
	delta.expected_manifest_location = manifestPage;
	delta.expected_manifest = manifest;
	delta.expected_manifest_owner_lsn = LogSequenceNumber(2U);
	delta.dimensions = 2U;
	delta.metric = VectorDistanceMetric::VDM_L2;
	delta.first_vector_id = 2U;
	delta.inserted_vector_count = 1U;
	delta.delta_entry_count = 1U;
	delta.raw_pages.push_back({NativeGraphVectorDeltaRawPlacement::NGVDRP_NEW_POOL, {}, {}, {}, 0U, 1U, 1U});
	delta.manifest_in_new_pool = true;
	delta.first_slot = PhysicalSlotId(85U);
	delta.last_slot = PhysicalSlotId(86U);
	deltaDerived.vector_deltas.push_back(std::move(delta));

	bool vectorPreparerCalled = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, deltaDerived, LogSequenceNumber(3U),
						[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
						   std::span<EntityPageNewPoolBatchMaterialization>,
						   std::span<NativeGraphPropertyPageReplacementMaterialization>,
						   std::span<NativeGraphPropertyPageBatchMaterialization>,
						   std::span<LargeValueMaterializationResult>) {},
						[&indexes, &nodePlan, &reversePlan](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
							ASSERT_EQ(batches.size(), 3U);
							ASSERT_EQ(batches[0].size(), 1U);
							ASSERT_EQ(batches[1].size(), 1U);
							ASSERT_EQ(batches[2].size(), 1U);
							indexes.prepareRootLeafMutation(nodePlan, batches[0].front().pool, nodePlan.expected_location,
													batches[0].front().replacement_locations.front(), LogSequenceNumber(3U));
							indexes.prepareRootLeafMutation(reversePlan, batches[1].front().pool, reversePlan.expected_location,
													batches[1].front().replacement_locations.front(), LogSequenceNumber(3U));
							IndexTreeMutationPoolBatch &newBatch = batches[2].front();
							const PageOwnerHeader owner{newBatch.appended_page_ids.front(), DELTA_INDEX_ID,
													PageOwnerKind::POK_INDEX, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
													newBatch.pool.generation, LogSequenceNumber(3U)};
							BTreePageHeader header;
							header.index_id = DELTA_INDEX_ID;
							header.page_id = owner.page_id;
							header.generation = newBatch.pool.generation;
							std::vector<BTreePage> pages;
							pages.push_back(BTreePage::create(owner, header));
							indexes.prepareNewPoolInitialPages(newBatch.pool, pages, newBatch.appended_locations,
													LogSequenceNumber(3U));
						},
						{}, [&vectorPreparerCalled](std::vector<NativeGraphVectorDeltaMaterialization> &plans) {
							vectorPreparerCalled = true;
							ASSERT_EQ(plans.size(), 1U);
							NativeGraphVectorDeltaMaterialization &plan = plans.front();
							plan.raw_pages.front().location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							plan.manifest_replacement.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							plan.vector_locations = {{plan.mutation.first_vector_id, plan.raw_pages.front().location.page_id, 0U, 1U}};
							// Deliberately leave both metadata-assigned delta frames unwritten.
						}));
			},
			FormatError::FERR_BUFFER_TOO_SMALL);
	EXPECT_TRUE(vectorPreparerCalled);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock().generation, bootstrapGeneration);
	EXPECT_EQ(unchanged.superblock().next_logical_vector_id, 2U);
	EXPECT_EQ(store->findIndexRoot(unchanged, INDEX_ID), nodeRoot);
	EXPECT_EQ(store->findIndexRoot(unchanged, REVERSE_INDEX_ID), reverseRoot);
	EXPECT_FALSE(store->findIndexRootOptional(unchanged, DELTA_INDEX_ID).has_value());
	EXPECT_EQ(store->findVectorManifest(unchanged, INDEX_ID), manifest);
	for (const uint64_t slot: {84U, 85U, 86U}) {
		EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(slot)));
	}
	unchanged.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, bootstrapGeneration);
	EXPECT_EQ(reopened->findVectorManifest(recovered, INDEX_ID), manifest);
	EXPECT_FALSE(reopened->findIndexRootOptional(recovered, DELTA_INDEX_ID).has_value());
	recovered.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsInvalidVectorBootstrapFramesAndLocationsWithoutPublishingTheGraphGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

	NativeGraphDerivedStateMutation derived;
	for (const uint64_t indexId: {27U, 28U}) {
		NativeGraphIndexTreeMutation mapping;
		mapping.index_id = indexId;
		mapping.new_pool_requests.push_back(
				{PhysicalSlotId(50U + indexId - 27U), PhysicalSlotId(50U + indexId - 27U), indexId, 1U});
		mapping.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
		derived.index_tree_mutations.push_back(std::move(mapping));
	}
	derived.vector_bootstraps.push_back(
			{27U, 28U, 1U, 1U, 2U, VectorDistanceMetric::VDM_L2, 1U, PhysicalSlotId(52U), PhysicalSlotId(53U)});

	const auto expectRejectedBootstrap = [&](const NativeGraphVectorBootstrapPreparer &vectorPreparer) {
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyPools, noLargeValues, derived, LogSequenceNumber(2U),
							[](std::span<EntityPageReplacementMaterialization>,
							   std::span<EntityPageAppendBatchMaterialization>,
							   std::span<EntityPageNewPoolBatchMaterialization>,
							   std::span<NativeGraphPropertyPageReplacementMaterialization>,
							   std::span<NativeGraphPropertyPageBatchMaterialization>,
							   std::span<LargeValueMaterializationResult>) {},
							[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
								for (std::vector<IndexTreeMutationPoolBatch> &mapping: batches) {
									mapping.front().appended_locations.front().page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
								}
							},
							vectorPreparer));
				},
				FormatError::FERR_INVALID_STATE);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
		EXPECT_FALSE(store->findVectorManifestOptional(unchanged, 27U).has_value());
		unchanged.release();
	};
	const auto setValidVectorLocation = [](NativeGraphVectorBootstrapMaterialization &materialization) {
		materialization.vector_locations = {
				{materialization.mutation.first_vector_id, materialization.raw_pages.front().page_id, 0U, 1U}};
	};

	expectRejectedBootstrap([&setValidVectorLocation](std::vector<NativeGraphVectorBootstrapMaterialization> &plans) {
		ASSERT_EQ(plans.size(), 1U);
		setValidVectorLocation(plans.front());
		// The raw page remains at the metadata sentinel frame instead of a data frame.
	});
	expectRejectedBootstrap([&setValidVectorLocation](std::vector<NativeGraphVectorBootstrapMaterialization> &plans) {
		ASSERT_EQ(plans.size(), 1U);
		NativeGraphVectorBootstrapMaterialization &plan = plans.front();
		plan.raw_pages.front().page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
		setValidVectorLocation(plan);
		// The manifest also needs a concrete data frame; leaving the sentinel is invalid.
	});
	expectRejectedBootstrap([&setValidVectorLocation](std::vector<NativeGraphVectorBootstrapMaterialization> &plans) {
		ASSERT_EQ(plans.size(), 1U);
		NativeGraphVectorBootstrapMaterialization &plan = plans.front();
		plan.raw_pages.front().page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
		plan.manifest_page.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
		setValidVectorLocation(plan);
		plan.vector_locations.front().slot_generation = 0U;
	});

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(1U));
	EXPECT_FALSE(reopened->findIndexRootOptional(recovered, 27U).has_value());
	EXPECT_FALSE(reopened->findIndexRootOptional(recovered, 28U).has_value());
	EXPECT_FALSE(reopened->findVectorManifestOptional(recovered, 27U).has_value());
	for (const uint64_t slot: {50U, 51U, 52U, 53U}) {
		EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(slot)));
	}
	recovered.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsVectorDeltaPreparerIdentityChangesWithoutPublishingOverTheCompleteBootstrapGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	constexpr uint64_t INDEX_ID = 37U;
	constexpr uint64_t VECTOR_NODE_MAPPING_INDEX_ID = 38U;
	constexpr uint64_t DELTA_MAPPING_INDEX_ID = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | INDEX_ID;
	const std::array<uint64_t, 2U> bootstrapIndexIds{{INDEX_ID, VECTOR_NODE_MAPPING_INDEX_ID}};

	NativeGraphDerivedStateMutation bootstrap;
	for (const uint64_t indexId: bootstrapIndexIds) {
		NativeGraphIndexTreeMutation mapping;
		mapping.index_id = indexId;
		mapping.new_pool_requests.push_back(
				{PhysicalSlotId(60U + indexId - INDEX_ID), PhysicalSlotId(60U + indexId - INDEX_ID), indexId, 1U});
		mapping.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
		bootstrap.index_tree_mutations.push_back(std::move(mapping));
	}
	bootstrap.vector_bootstraps.push_back({INDEX_ID,
									  VECTOR_NODE_MAPPING_INDEX_ID,
									  1U,
									  1U,
									  2U,
									  VectorDistanceMetric::VDM_L2,
									  1U,
								  PhysicalSlotId(62U),
								  PhysicalSlotId(63U)});
	SlottedPagePool indexPages(io, *store);
	BTreePageStore indexes(indexPages, *store);
	VectorPageStore rawPages(io, *store);
	VectorManifestStore manifests(io, *store);
	static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools, noLargeValues,
			bootstrap, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[&indexes, &bootstrapIndexIds](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				materializeInitialIndexBatches(indexes, batches, bootstrapIndexIds, LogSequenceNumber(2U));
			},
			[&rawPages, &manifests](std::vector<NativeGraphVectorBootstrapMaterialization> &plans) {
				materializeInitialVectorBootstrap(rawPages, manifests, plans, LogSequenceNumber(2U));
			}));

	MetadataSnapshotPin beforeDelta = store->openSnapshot();
	const IndexRootLocation nodeRoot = store->findIndexRoot(beforeDelta, INDEX_ID);
	const IndexRootLocation reverseRoot = store->findIndexRoot(beforeDelta, VECTOR_NODE_MAPPING_INDEX_ID);
	const PageLocation nodeRootPage = store->findPageLocation(beforeDelta, nodeRoot.root_page_id);
	const PageLocation reverseRootPage = store->findPageLocation(beforeDelta, reverseRoot.root_page_id);
	const VectorManifestLocation manifest = store->findVectorManifest(beforeDelta, INDEX_ID);
	const PageLocation manifestPage = store->findPageLocation(beforeDelta, manifest.manifest_page_id);
	const CatalogEntry manifestPool = store->findCatalog(beforeDelta, manifestPage.segment_id);
	const Generation bootstrapGeneration = beforeDelta.superblock().generation;
	EXPECT_EQ(bootstrapGeneration, Generation(2U));
	beforeDelta.release();

	const auto existingRootMutation = [](const IndexRootLocation &root, const PageLocation &page) {
		NativeGraphIndexTreeMutation mutation;
		mutation.index_id = root.index_id;
		mutation.expected_root = root;
		mutation.pool_requests.push_back({page.segment_id, {}, 1U, {}});
		return mutation;
	};
	NativeGraphDerivedStateMutation deltaDerived;
	deltaDerived.index_tree_mutations.push_back(existingRootMutation(nodeRoot, nodeRootPage));
	deltaDerived.index_tree_mutations.push_back(existingRootMutation(reverseRoot, reverseRootPage));
	NativeGraphIndexTreeMutation deltaMapping;
	deltaMapping.index_id = DELTA_MAPPING_INDEX_ID;
	deltaMapping.new_pool_requests.push_back(
			{PhysicalSlotId(64U), PhysicalSlotId(64U), DELTA_MAPPING_INDEX_ID, 1U});
	deltaMapping.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
	deltaDerived.index_tree_mutations.push_back(std::move(deltaMapping));
	NativeGraphVectorDeltaMutation delta;
	delta.index_id = INDEX_ID;
	delta.vector_node_mapping_index_id = VECTOR_NODE_MAPPING_INDEX_ID;
	delta.delta_mapping_index_id = DELTA_MAPPING_INDEX_ID;
	delta.expected_node_mapping_root = nodeRoot;
	delta.expected_vector_node_mapping_root = reverseRoot;
	delta.expected_manifest_pool = manifestPool;
	delta.expected_manifest_location = manifestPage;
	delta.expected_manifest = manifest;
	delta.expected_manifest_owner_lsn = LogSequenceNumber(2U);
	delta.dimensions = 2U;
	delta.metric = VectorDistanceMetric::VDM_L2;
	delta.first_vector_id = 2U;
	delta.inserted_vector_count = 1U;
	delta.delta_entry_count = 1U;
	delta.raw_pages.push_back({NativeGraphVectorDeltaRawPlacement::NGVDRP_NEW_POOL, {}, {}, {}, 0U, 1U, 1U});
	delta.first_slot = PhysicalSlotId(65U);
	delta.last_slot = PhysicalSlotId(66U);
	deltaDerived.vector_deltas.push_back(std::move(delta));

	const auto expectPreflightRejectedDelta = [&](const NativeGraphDerivedStateMutation &derived) {
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyPools, noLargeValues, derived, LogSequenceNumber(3U),
							[](std::span<EntityPageReplacementMaterialization>,
							   std::span<EntityPageAppendBatchMaterialization>,
							   std::span<EntityPageNewPoolBatchMaterialization>,
							   std::span<NativeGraphPropertyPageReplacementMaterialization>,
							   std::span<NativeGraphPropertyPageBatchMaterialization>,
							   std::span<LargeValueMaterializationResult>) {},
							[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {},
							[](std::vector<NativeGraphVectorDeltaMaterialization> &) {}));
				},
				FormatError::FERR_INVALID_HEADER);
	};
	NativeGraphDerivedStateMutation missingDeltaMappingTransition = deltaDerived;
	missingDeltaMappingTransition.index_tree_mutations.pop_back();
	expectPreflightRejectedDelta(missingDeltaMappingTransition);
	NativeGraphDerivedStateMutation unorderedErasedVectors = deltaDerived;
	unorderedErasedVectors.vector_deltas.front().erased_vector_ids = {0U};
	expectPreflightRejectedDelta(unorderedErasedVectors);
	NativeGraphDerivedStateMutation invalidRawTransition = deltaDerived;
	invalidRawTransition.vector_deltas.front().raw_pages.front().page_ordinal = 1U;
	expectPreflightRejectedDelta(invalidRawTransition);
	NativeGraphDerivedStateMutation incompleteRawTransitions = deltaDerived;
	incompleteRawTransitions.vector_deltas.front().raw_pages.front().inserted_record_count = 2U;
	expectPreflightRejectedDelta(incompleteRawTransitions);

	size_t deltaPreparerCalls = 0U;
	const auto expectRejectedDelta = [&](const NativeGraphDerivedStateMutation &derived,
									 const NativeGraphVectorDeltaPreparer &deltaPreparer) {
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
							noLargeValues, derived, LogSequenceNumber(3U),
							[](std::span<EntityPageReplacementMaterialization>,
							   std::span<EntityPageAppendBatchMaterialization>,
							   std::span<EntityPageNewPoolBatchMaterialization>,
							   std::span<NativeGraphPropertyPageReplacementMaterialization>,
							   std::span<NativeGraphPropertyPageBatchMaterialization>,
							   std::span<LargeValueMaterializationResult>) {},
							[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
								ASSERT_EQ(batches.size(), 3U);
								batches[0].front().appended_locations.front().page_index =
										RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
								batches[1].front().appended_locations.front().page_index =
										RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
								batches[2].front().appended_locations.front().page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
							},
							{}, deltaPreparer));
				},
				FormatError::FERR_INVALID_STATE);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock().generation, bootstrapGeneration);
		EXPECT_EQ(unchanged.superblock().next_logical_vector_id, 2U);
		EXPECT_FALSE(store->findIndexRootOptional(unchanged, DELTA_MAPPING_INDEX_ID).has_value());
		for (const uint64_t slot: {64U, 65U, 66U}) {
			EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(slot)));
		}
		unchanged.release();
	};
	const auto setValidVectorLocation = [](NativeGraphVectorDeltaMaterialization &plan) {
		plan.vector_locations = {{plan.mutation.first_vector_id, plan.raw_pages.front().location.page_id, 0U, 1U}};
	};

	expectRejectedDelta(deltaDerived, [&deltaPreparerCalls, &setValidVectorLocation](
						std::vector<NativeGraphVectorDeltaMaterialization> &plans) {
		++deltaPreparerCalls;
		ASSERT_EQ(plans.size(), 1U);
		NativeGraphVectorDeltaMaterialization &plan = plans.front();
		setValidVectorLocation(plan);
		// Manifest identity is owned by metadata, never by the typed writer.
		++plan.manifest_location.index_id;
	});
	expectRejectedDelta(deltaDerived, [&deltaPreparerCalls, &setValidVectorLocation](
						std::vector<NativeGraphVectorDeltaMaterialization> &plans) {
		++deltaPreparerCalls;
		ASSERT_EQ(plans.size(), 1U);
		setValidVectorLocation(plans.front());
		// Raw data must be written to a concrete pool frame, not the metadata sentinel.
	});
	expectRejectedDelta(deltaDerived, [&deltaPreparerCalls, &setValidVectorLocation](
						std::vector<NativeGraphVectorDeltaMaterialization> &plans) {
		++deltaPreparerCalls;
		ASSERT_EQ(plans.size(), 1U);
		NativeGraphVectorDeltaMaterialization &plan = plans.front();
		plan.raw_pages.front().location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
		setValidVectorLocation(plan);
		// The COW Manifest requires an independent, concrete destination frame.
	});
	expectRejectedDelta(deltaDerived, [&deltaPreparerCalls, &setValidVectorLocation](
						std::vector<NativeGraphVectorDeltaMaterialization> &plans) {
		++deltaPreparerCalls;
		ASSERT_EQ(plans.size(), 1U);
		NativeGraphVectorDeltaMaterialization &plan = plans.front();
		plan.raw_pages.front().location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
		plan.manifest_replacement.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
		setValidVectorLocation(plan);
		// COW must not overwrite the source Manifest frame still visible to old snapshots.
	});
	expectRejectedDelta(deltaDerived, [&deltaPreparerCalls, &setValidVectorLocation](
						std::vector<NativeGraphVectorDeltaMaterialization> &plans) {
		++deltaPreparerCalls;
		ASSERT_EQ(plans.size(), 1U);
		NativeGraphVectorDeltaMaterialization &plan = plans.front();
		plan.raw_pages.front().location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
		plan.manifest_replacement.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
		setValidVectorLocation(plan);
		plan.vector_locations.front().slot_generation = 0U;
	});
	expectRejectedDelta(deltaDerived, [&deltaPreparerCalls, &setValidVectorLocation](
						std::vector<NativeGraphVectorDeltaMaterialization> &plans) {
		++deltaPreparerCalls;
		ASSERT_EQ(plans.size(), 1U);
		NativeGraphVectorDeltaMaterialization &plan = plans.front();
		plan.raw_pages.front().location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
		plan.manifest_replacement.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
		setValidVectorLocation(plan);
		// Only the metadata coordinator may assign the fresh pool identity.
		++plan.raw_new_pools.front().owner_id;
	});
	NativeGraphDerivedStateMutation freshManifestDelta = deltaDerived;
	freshManifestDelta.vector_deltas.front().manifest_in_new_pool = true;
	expectRejectedDelta(freshManifestDelta, [&deltaPreparerCalls, &setValidVectorLocation](
										std::vector<NativeGraphVectorDeltaMaterialization> &plans) {
		++deltaPreparerCalls;
		ASSERT_EQ(plans.size(), 1U);
		NativeGraphVectorDeltaMaterialization &plan = plans.front();
		plan.raw_pages.front().location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
		plan.manifest_replacement.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME;
		setValidVectorLocation(plan);
		// A new Manifest pool is assigned by metadata, never by its typed writer.
		++plan.manifest_new_pool.owner_id;
	});
	EXPECT_EQ(deltaPreparerCalls, 7U);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock().generation, bootstrapGeneration);
	EXPECT_EQ(unchanged.superblock().next_logical_vector_id, 2U);
	EXPECT_EQ(store->findIndexRoot(unchanged, INDEX_ID), nodeRoot);
	EXPECT_EQ(store->findIndexRoot(unchanged, VECTOR_NODE_MAPPING_INDEX_ID), reverseRoot);
	EXPECT_FALSE(store->findIndexRootOptional(unchanged, DELTA_MAPPING_INDEX_ID).has_value());
	EXPECT_EQ(store->findVectorManifest(unchanged, INDEX_ID), manifest);
	for (const uint64_t slot: {64U, 65U, 66U}) {
		EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(slot)));
	}
	unchanged.release();
	store.reset();

	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, bootstrapGeneration);
	EXPECT_EQ(reopened->findIndexRoot(recovered, INDEX_ID), nodeRoot);
	EXPECT_EQ(reopened->findVectorManifest(recovered, INDEX_ID), manifest);
	EXPECT_FALSE(reopened->findIndexRootOptional(recovered, DELTA_MAPPING_INDEX_ID).has_value());
	recovered.release();
}
