/**
 * @file test_RelocatableMetadataStoreGraphMutation_EntityPoolValidation.cpp
 * @brief Entity page-pool input contracts for graph metadata publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include "graph/storage/relocatable/entity/EntityPageStore.hpp"

#include <array>
#include <functional>

namespace {

const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

void noOpGraphMaterializer(std::span<EntityPageReplacementMaterialization>,
						   std::span<EntityPageAppendBatchMaterialization>,
						   std::span<EntityPageNewPoolBatchMaterialization>,
						   std::span<NativeGraphPropertyPageReplacementMaterialization>,
						   std::span<NativeGraphPropertyPageBatchMaterialization>,
						   std::span<LargeValueMaterializationResult>) {}

} // namespace

TEST_F(RelocatableMetadataStoreTest, RejectsConflictingOrEntityMappedAuxiliaryNewPoolsBeforeReservingAnyPhysicalSlot) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));

	CatalogSlotReservationRequest reservation;
	reservation.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
	reservation.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	reservation.owner_id = 17U;
	reservation.segment_generation = Generation(1U);
	reservation.state = SegmentState::SGS_SEALED;
	reservation.require_absent = true;
	const EntityPageNewPoolBatchRequest first{reservation,
											  PageId(101U),
											  PhysicalSlotId(40U),
											  PhysicalSlotId(40U),
											  {},
											  {},
											  PageOwnerKind::POK_ENTITY_LABEL};
	EntityPageNewPoolBatchRequest conflicting = first;
	conflicting.page_id = PageId(102U);
	conflicting.first_slot = PhysicalSlotId(41U);
	conflicting.last_slot = PhysicalSlotId(41U);
	const std::array<EntityPageNewPoolBatchRequest, 2U> conflictingPools{{first, conflicting}};

	bool materializerCalled = false;
	const auto expectRejected = [&](std::span<const EntityPageNewPoolBatchRequest> requests) {
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, requests, noPropertyReplacements, noPropertyBatches,
							noLargeValues, {}, LogSequenceNumber(2U),
							[&materializerCalled](std::span<EntityPageReplacementMaterialization>,
													  std::span<EntityPageAppendBatchMaterialization>,
													  std::span<EntityPageNewPoolBatchMaterialization>,
													  std::span<NativeGraphPropertyPageReplacementMaterialization>,
													  std::span<NativeGraphPropertyPageBatchMaterialization>,
													  std::span<LargeValueMaterializationResult>) {
								materializerCalled = true;
							},
							{}));
				},
				FormatError::FERR_INVALID_STATE);
		EXPECT_FALSE(materializerCalled);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
		EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
		EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(41U)));
		EXPECT_FALSE(store->findPageLocationOptional(unchanged, PageId(101U)).has_value());
		EXPECT_FALSE(store->findPageLocationOptional(unchanged, PageId(102U)).has_value());
		unchanged.release();
	};

	expectRejected(conflictingPools);

	// Exercise each identity component independently. These are all malformed
	// duplicate claims for the same physical pool and must fail before any slot
	// reservation, regardless of which field differs first.
	const auto expectIdentityMismatch = [&](auto mutate) {
		EntityPageNewPoolBatchRequest mismatch = first;
		mutate(mismatch);
		const std::array<EntityPageNewPoolBatchRequest, 2U> requests{{first, mismatch}};
		expectRejected(requests);
	};
	expectIdentityMismatch(
			[](auto &request) { request.reservation.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID + 1U); });
	expectIdentityMismatch([](auto &request) { request.reservation.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL; });
	expectIdentityMismatch([](auto &request) { request.reservation.owner_id = 18U; });
	expectIdentityMismatch([](auto &request) { request.reservation.segment_generation = Generation(2U); });
	expectIdentityMismatch([](auto &request) { request.reservation.require_absent = false; });
	expectIdentityMismatch([](auto &request) { request.reservation.state = SegmentState::SGS_RETIRED; });
	expectIdentityMismatch([](auto &request) { request.last_slot = PhysicalSlotId(41U); });

	const auto expectMalformed = [&](auto mutate) {
		EntityPageNewPoolBatchRequest malformed = first;
		mutate(malformed);
		const std::array<EntityPageNewPoolBatchRequest, 1U> requests{{malformed}};
		expectRejected(requests);
	};
	// A new-pool request is an untrusted public planner boundary. Validate every
	// independently supplied identity and ownership field before reserving its slot.
	expectMalformed([](auto &request) { request.reservation.segment_id = {}; });
	expectMalformed([](auto &request) {
		request.reservation.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID - 1U);
	});
	expectMalformed([](auto &request) { request.reservation.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL; });
	expectMalformed([](auto &request) { request.reservation.owner_id = 0U; });
	expectMalformed([](auto &request) { request.reservation.segment_generation = {}; });
	expectMalformed([](auto &request) { request.reservation.state = SegmentState::SGS_RETIRED; });
	expectMalformed([](auto &request) { request.reservation.require_absent = false; });
	expectMalformed([](auto &request) { request.page_id = {}; });
	expectMalformed([](auto &request) { request.first_slot = {}; });
	expectMalformed([](auto &request) { request.last_slot = {}; });
	expectMalformed([](auto &request) {
		request.first_slot = PhysicalSlotId(41U);
		request.last_slot = PhysicalSlotId(40U);
	});
	expectMalformed([](auto &request) { request.owner_kind = static_cast<PageOwnerKind>(255U); });
	expectMalformed([](auto &request) {
		request.owner_kind = PageOwnerKind::POK_ENTITY;
		request.inserted_nodes.clear();
		request.inserted_edges.clear();
	});

	EntityPageNewPoolBatchRequest entityMappedAuxiliary = first;
	entityMappedAuxiliary.inserted_nodes = {{1U, PageId(101U), 0U, 1U}};
	const std::array<EntityPageNewPoolBatchRequest, 1U> malformedAuxiliary{{entityMappedAuxiliary}};
	expectRejected(malformedAuxiliary);
}

TEST_F(RelocatableMetadataStoreTest, RejectsForgedEntityReplacementPoolAndLocationFencesBeforeMaterialization) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	EntityPageStore entities(io, *store);
	NodeEntityRecord node;
	const EntityPageNewPoolRequest poolRequest{
			SegmentId(31U), 901U, Generation(1U), PageId(101U), PhysicalSlotId(40U), PhysicalSlotId(40U)};
	const auto inserted = entities.allocateNode(poolRequest, 1001U, node, LogSequenceNumber(2U));
	ASSERT_TRUE(inserted.has_value());

	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	const CatalogEntry currentPool = inserted->pool;
	const PageLocation currentLocation = inserted->page;
	before.release();
	bool materialized = false;
	const auto materializer = [&](std::span<EntityPageReplacementMaterialization>,
									std::span<EntityPageAppendBatchMaterialization>,
									std::span<EntityPageNewPoolBatchMaterialization>,
									std::span<NativeGraphPropertyPageReplacementMaterialization>,
									std::span<NativeGraphPropertyPageBatchMaterialization>,
									std::span<LargeValueMaterializationResult>) { materialized = true; };
	const auto reject = [&](const std::function<void(EntityPageReplacementRequest &)> &forge) {
		EntityPageReplacementRequest request;
		request.expected_pool = currentPool;
		request.expected_location = currentLocation;
		forge(request);
		const std::array<EntityPageReplacementRequest, 1U> requests{{request}};
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							requests, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
							noLargeValues, {}, LogSequenceNumber(3U), materializer, {}));
				},
				FormatError::FERR_INVALID_STATE);
		EXPECT_FALSE(materialized);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
		EXPECT_EQ(store->findPageLocation(unchanged, PageId(101U)), currentLocation);
		unchanged.release();
	};

	reject([](auto &request) { request.expected_pool.physical_slot = PhysicalSlotId(41U); });
	reject([](auto &request) { request.expected_pool.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL; });
	reject([](auto &request) { request.expected_pool.owner_id = 902U; });
	reject([](auto &request) { request.expected_pool.generation = Generation(2U); });
	reject([](auto &request) { request.expected_pool.committed_lsn = LogSequenceNumber(3U); });
	reject([](auto &request) { request.expected_location.segment_id = SegmentId(32U); });
	reject([](auto &request) { request.expected_location.generation = Generation(2U); });
	reject([](auto &request) { request.expected_location.page_index = 0U; });
	reject([](auto &request) { request.expected_location.page_index = RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U; });

	// Existing cross-pool destinations use the same identity fence.
	reject([&](auto &request) { request.replacement_pool = currentPool; request.replacement_pool.physical_slot = PhysicalSlotId(41U); });
	reject([&](auto &request) { request.replacement_pool = currentPool; request.replacement_pool.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL; });
	reject([&](auto &request) { request.replacement_pool = currentPool; request.replacement_pool.owner_id = 902U; });
	reject([&](auto &request) { request.replacement_pool = currentPool; request.replacement_pool.generation = Generation(2U); });
	reject([&](auto &request) { request.replacement_pool = currentPool; request.replacement_pool.state = SegmentState::SGS_FREE; });
	reject([&](auto &request) { request.replacement_pool = currentPool; request.replacement_pool.committed_lsn = LogSequenceNumber(3U); });

	// A missing replacement Segment cannot carry any residual identity field.
	reject([](auto &request) { request.replacement_pool.physical_slot = PhysicalSlotId(41U); });
	reject([](auto &request) { request.replacement_pool.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL; });
	reject([](auto &request) { request.replacement_pool.owner_id = 902U; });
	reject([](auto &request) { request.replacement_pool.generation = Generation(1U); });
	reject([](auto &request) { request.replacement_pool.state = SegmentState::SGS_SEALED; });
	reject([](auto &request) { request.replacement_pool.committed_lsn = LogSequenceNumber(3U); });

	// A fresh destination is a complete, unpublished identity fence.
	reject([](auto &request) { request.replacement_new_pool = EntityPageFreshPoolDestination{SegmentId(32U), 0U, Generation(1U), PhysicalSlotId(41U), PhysicalSlotId(41U)}; });
	reject([](auto &request) { request.replacement_new_pool = EntityPageFreshPoolDestination{SegmentId{}, 901U, Generation(1U), PhysicalSlotId(41U), PhysicalSlotId(41U)}; });
	reject([](auto &request) { request.replacement_new_pool = EntityPageFreshPoolDestination{SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID - 1U), 901U, Generation(1U), PhysicalSlotId(41U), PhysicalSlotId(41U)}; });
	reject([](auto &request) { request.replacement_new_pool = EntityPageFreshPoolDestination{SegmentId(32U), 901U, Generation{}, PhysicalSlotId(41U), PhysicalSlotId(41U)}; });
	reject([](auto &request) { request.replacement_new_pool = EntityPageFreshPoolDestination{SegmentId(32U), 901U, Generation(1U), PhysicalSlotId{}, PhysicalSlotId(41U)}; });
	reject([](auto &request) { request.replacement_new_pool = EntityPageFreshPoolDestination{SegmentId(32U), 901U, Generation(1U), PhysicalSlotId(41U), PhysicalSlotId{}}; });
	reject([](auto &request) { request.replacement_new_pool = EntityPageFreshPoolDestination{SegmentId(32U), 901U, Generation(1U), PhysicalSlotId(42U), PhysicalSlotId(41U)}; });
	reject([&](auto &request) { request.replacement_new_pool = EntityPageFreshPoolDestination{currentPool.segment_id, currentPool.owner_id, currentPool.generation, currentPool.physical_slot, currentPool.physical_slot}; });
	reject([](auto &request) { request.replacement_new_pool = EntityPageFreshPoolDestination{SegmentId(32U), 902U, Generation(1U), PhysicalSlotId(41U), PhysicalSlotId(41U)}; });
	{
		EntityPageReplacementRequest first;
		first.expected_pool = currentPool;
		first.expected_location = currentLocation;
		first.replacement_new_pool = EntityPageFreshPoolDestination{SegmentId(32U), currentPool.owner_id, Generation(1U), PhysicalSlotId(41U), PhysicalSlotId(41U)};
		const auto rejectConflictingFreshIdentity = [&](const std::function<void(EntityPageFreshPoolDestination &)> &forge) {
			EntityPageReplacementRequest conflicting = first;
			forge(*conflicting.replacement_new_pool);
			const std::array<EntityPageReplacementRequest, 2U> requests{{first, conflicting}};
			expectFormatError([&] { static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
					requests, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
					noLargeValues, {}, LogSequenceNumber(3U), materializer, {})); }, FormatError::FERR_INVALID_STATE);
			EXPECT_FALSE(materialized);
		};
		// Both requests name the same fresh Segment. Each remaining identity field
		// must match before the coordinator can reserve or materialize that pool.
		rejectConflictingFreshIdentity([](auto &pool) { pool.pool_owner_id = 902U; });
		rejectConflictingFreshIdentity([](auto &pool) { pool.generation = Generation(2U); });
		rejectConflictingFreshIdentity([](auto &pool) { pool.first_slot = PhysicalSlotId(42U); });
		rejectConflictingFreshIdentity([](auto &pool) { pool.last_slot = PhysicalSlotId(42U); });
	}
	reject([&](auto &request) {
		request.replacement_pool = currentPool;
		request.replacement_new_pool = EntityPageFreshPoolDestination{SegmentId(32U), currentPool.owner_id, Generation(1U), PhysicalSlotId(41U), PhysicalSlotId(41U)};
	});
	reject([](auto &request) { request.owner_kind = static_cast<PageOwnerKind>(255U); });
	reject([](auto &request) { request.owner_kind = PageOwnerKind::POK_ENTITY_LABEL; request.inserted_nodes = {{1002U, PageId(101U), 0U, 1U}}; });
	reject([](auto &request) { request.inserted_nodes = {{1002U, PageId(101U), 0U, 1U}}; request.inserted_edges = {{2001U, PageId(101U), 0U, 1U}}; });
	reject([](auto &request) { request.retire_page = true; request.inserted_nodes = {{1002U, PageId(101U), 0U, 1U}}; });
}

TEST_F(RelocatableMetadataStoreTest, RejectsForgedEntityAppendPoolAndMapFencesBeforeMaterialization) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	EntityPageStore entities(io, *store);
	NodeEntityRecord node;
	const EntityPageNewPoolRequest poolRequest{
			SegmentId(31U), 901U, Generation(1U), PageId(101U), PhysicalSlotId(40U), PhysicalSlotId(40U)};
	const auto inserted = entities.allocateNode(poolRequest, 1001U, node, LogSequenceNumber(2U));
	ASSERT_TRUE(inserted.has_value());

	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	const CatalogEntry currentPool = inserted->pool;
	before.release();
	const auto reject = [&](const std::function<void(EntityPageAppendBatchRequest &)> &forge) {
		EntityPageAppendBatchRequest request;
		request.expected_pool = currentPool;
		request.page_id = PageId(102U);
		request.inserted_nodes = {{1002U, request.page_id, 0U, 1U}};
		forge(request);
		const std::array<EntityPageAppendBatchRequest, 1U> requests{{request}};
		expectFormatError([&] { static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
				noEntityReplacements, requests, noEntityPools, noPropertyReplacements, noPropertyBatches,
				noLargeValues, {}, LogSequenceNumber(3U), noOpGraphMaterializer, {})); }, FormatError::FERR_INVALID_STATE);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
		EXPECT_FALSE(store->findNodePageLocationOptional(unchanged, 1002U).has_value());
		unchanged.release();
	};

	reject([](auto &request) { request.page_id = {}; });
	reject([](auto &request) { request.expected_pool.physical_slot = PhysicalSlotId(41U); });
	reject([](auto &request) { request.expected_pool.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL; });
	reject([](auto &request) { request.expected_pool.owner_id = 902U; });
	reject([](auto &request) { request.expected_pool.generation = Generation(2U); });
	reject([](auto &request) { request.expected_pool.committed_lsn = LogSequenceNumber(3U); });
	reject([](auto &request) { request.owner_kind = static_cast<PageOwnerKind>(255U); });
	reject([](auto &request) { request.owner_kind = PageOwnerKind::POK_ENTITY_LABEL; request.inserted_nodes = {{1002U, request.page_id, 0U, 1U}}; });
	reject([](auto &request) { request.inserted_nodes.clear(); });
	reject([](auto &request) { request.inserted_edges = {{2001U, request.page_id, 0U, 1U}}; });
	reject([](auto &request) { request.page_id = PageId(101U); });
}

TEST_F(RelocatableMetadataStoreTest,
       RejectsFreshReplacementAndNewPoolBatchesWithMismatchedIdentityBeforeMaterialization) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	EntityPageStore entities(io, *store);
	NodeEntityRecord node;
	const EntityPageNewPoolRequest poolRequest{
			SegmentId(31U), 901U, Generation(1U), PageId(101U), PhysicalSlotId(40U), PhysicalSlotId(40U)};
	const auto inserted = entities.allocateNode(poolRequest, 1001U, node, LogSequenceNumber(2U));
	ASSERT_TRUE(inserted.has_value());

	EntityPageReplacementRequest replacement;
	replacement.expected_pool = inserted->pool;
	replacement.expected_location = inserted->page;
	replacement.replacement_new_pool =
			EntityPageFreshPoolDestination{SegmentId(32U), 901U, Generation(1U), PhysicalSlotId(41U), PhysicalSlotId(41U)};
	EntityPageNewPoolBatchRequest newPool;
	newPool.reservation = {SegmentId(32U), SegmentKind::SGK_ENTITY_PAGE_POOL, 902U, Generation(1U),
							 SegmentState::SGS_SEALED, true};
	newPool.page_id = PageId(102U);
	newPool.first_slot = PhysicalSlotId(41U);
	newPool.last_slot = PhysicalSlotId(41U);
	newPool.owner_kind = PageOwnerKind::POK_ENTITY;
	newPool.inserted_nodes = {{1002U, newPool.page_id, 0U, 1U}};

	const std::array<EntityPageReplacementRequest, 1U> replacements{{replacement}};
	const std::array<EntityPageAppendBatchRequest, 0U> noAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 1U> newPools{{newPool}};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						replacements, noAppends, newPools, noPropertyReplacements, noPropertyBatches, noLargeValues, {},
						LogSequenceNumber(3U), noOpGraphMaterializer, {}));
			},
			FormatError::FERR_INVALID_STATE);

	// Matching identities pass the cross-request fence and continue into the
	// materialization contract, which is intentionally left unfilled here.
	newPool.reservation.owner_id = 901U;
	const std::array<EntityPageNewPoolBatchRequest, 1U> matchingNewPools{{newPool}};
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						replacements, noAppends, matchingNewPools, noPropertyReplacements, noPropertyBatches, noLargeValues,
						{}, LogSequenceNumber(3U), noOpGraphMaterializer, {}));
			},
			FormatError::FERR_INVALID_STATE);
}
