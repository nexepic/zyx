/**
 * @file test_RelocatableMetadataStoreValidation.cpp
 * @brief Validation coverage for relocatable Metadata Store public operations.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>
#include <gtest/gtest.h>

#include "graph/storage/StorageIO.hpp"
#include "graph/storage/relocatable/metadata/MetadataPageFile.hpp"
#include "graph/storage/relocatable/metadata/RelocatableMetadataStore.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	Superblock initialSuperblock() {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(1);
		result.checkpoint_lsn = LogSequenceNumber(1);
		return result;
	}

	CatalogSlotReservationRequest reservationRequest(SegmentId segmentId, SegmentKind kind) {
		CatalogSlotReservationRequest request;
		request.segment_id = segmentId;
		request.kind = kind;
		request.owner_id = 17;
		request.segment_generation = Generation(1);
		request.state = SegmentState::SGS_SEALED;
		request.require_absent = true;
		return request;
	}

	class RelocatableMetadataStoreValidationTest : public ::testing::Test {
	protected:
		std::filesystem::path dataPath;
		std::shared_ptr<std::fstream> dataFile;
		std::shared_ptr<StorageIO> io;

		void SetUp() override {
			const auto uuid = boost::uuids::random_generator()();
			dataPath = std::filesystem::temp_directory_path() /
					   ("relocatable_metadata_validation_" + to_string(uuid) + ".dat");
			dataFile = std::make_shared<std::fstream>(dataPath, std::ios::in | std::ios::out | std::ios::binary |
															std::ios::trunc);
			ASSERT_TRUE(dataFile->is_open());
			io = std::make_shared<StorageIO>(dataFile, INVALID_FILE_HANDLE, INVALID_FILE_HANDLE);
		}

		void TearDown() override {
			io.reset();
			if (dataFile && dataFile->is_open()) {
				dataFile->close();
			}
			dataFile.reset();
			std::error_code error;
			std::filesystem::remove(dataPath, error);
		}
	};

} // namespace

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

	auto readOnly = RelocatableMetadataStore::openReadOnly(io);
	expectFormatError([&] { static_cast<void>(readOnly->reserveNativeGraphLogicalIds(1U, 0U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(readOnly->acquireNativeGraphLogicalIds(1U, 0U, 1U)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreValidationTest, ValidatesLogicalIdLeaseCapabilitiesAcrossNodeAndEdgeNamespaces) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	EXPECT_TRUE(store->claimNativeGraphLogicalIdReservation({}));
	const NativeGraphLogicalIdReservation reservation = store->acquireNativeGraphLogicalIds(2U, 0U, 2U);
	ASSERT_EQ(reservation.node_ranges.size(), 1U);
	EXPECT_TRUE(reservation.edge_ranges.empty());

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
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						malformedLargeValueWork, {}, LogSequenceNumber(3U), graphPreparer, {}, {}, {}, {}, {}, {}, {},
						{}, Generation(beforeGeneration.value())));
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

	MetadataSnapshotPin afterRejected = store->openSnapshot();
	EXPECT_EQ(afterRejected.superblock().generation, Generation(1U));
	afterRejected.release();
}

TEST_F(RelocatableMetadataStoreValidationTest, ValidatesEntityPropertyPoolMaterializationBeforePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const EntityPropertyKey key{EntityPropertyOwnerKind::EPOK_NODE, 17U};
	const EntityPropertyPagePoolPreparer validPreparer =
			[](const CatalogEntry &pool, PageId pageId, PageLocation &page, EntityPropertyLocation &property) {
				page = {pageId, pool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, pool.generation, 0U};
				property = {property.key, pageId, 0U, 1U};
			};
	expectFormatError(
			[&] {
				static_cast<void>(store->materializeEntityPropertyPagePool(
						0U, key, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U), validPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->materializeEntityPropertyPagePool(
						17U, {}, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U), validPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->materializeEntityPropertyPagePool(
						17U, key, PhysicalSlotId(41U), PhysicalSlotId(40U), LogSequenceNumber(2U), validPreparer));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->materializeEntityPropertyPagePool(
						17U, key, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber{}, {}));
			},
			FormatError::FERR_INVALID_HEADER);

	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40U), true, LogSequenceNumber(2U)));
	const std::optional<EntityPropertyPagePoolMaterialization> unavailable = store->materializeEntityPropertyPagePool(
			17U, key, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(3U), validPreparer);
	EXPECT_FALSE(unavailable.has_value());
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40U), false, LogSequenceNumber(4U)));
	expectFormatError(
			[&] {
				static_cast<void>(store->materializeEntityPropertyPagePool(
						17U, key, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(5U),
						[](const CatalogEntry &, PageId, PageLocation &page, EntityPropertyLocation &) {
							page.page_index = 0U;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin afterRejectedPreparer = store->openSnapshot();
	EXPECT_FALSE(store->findEntityPropertyLocationOptional(afterRejectedPreparer, key).has_value());
	afterRejectedPreparer.release();

	const std::optional<EntityPropertyPagePoolMaterialization> published = store->materializeEntityPropertyPagePool(
			17U, key, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(6U), validPreparer);
	ASSERT_TRUE(published.has_value());
	EXPECT_EQ(published->property.key, key);
	expectFormatError(
			[&] {
				static_cast<void>(store->materializeEntityPropertyPagePool(
						17U, key, PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(7U), validPreparer));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsInvalidExtents) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const ExtentLocation malformedExtent{ExtentId(101), SegmentId(31), RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U,
										 RSS_PAGE_POOL_DATA_FRAME_COUNT, Generation(1)};
	expectFormatError(
			[&] {
				static_cast<void>(store->relocateCatalogExtentSegment(
						malformedExtent, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(4),
						[](const CatalogEntry &, const CatalogEntry &) {}));
			},
			FormatError::FERR_INVALID_HEADER);

	const CatalogSlotReservationRequest entityRequest =
			reservationRequest(SegmentId(32), SegmentKind::SGK_ENTITY_PAGE_POOL);
	const ExtentLocation entityExtent{ExtentId(102), entityRequest.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
									  RSS_PAGE_POOL_DATA_FRAME_COUNT, entityRequest.segment_generation};
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(
							 std::span(&entityRequest, 1), {}, std::span(&entityExtent, 1), PhysicalSlotId(41),
							 PhysicalSlotId(41), LogSequenceNumber(4), [](std::span<const CatalogEntry>) {})
						.has_value());
	expectFormatError(
			[&] {
				static_cast<void>(store->relocateCatalogExtentSegment(
						entityExtent, PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(5),
						[](const CatalogEntry &, const CatalogEntry &) {}));
			},
			FormatError::FERR_INVALID_STATE);
	ExtentLocation staleExtent = entityExtent;
	staleExtent.generation = Generation(2);
	expectFormatError(
			[&] {
				static_cast<void>(store->relocateCatalogExtentSegment(
						staleExtent, PhysicalSlotId(42), PhysicalSlotId(42), LogSequenceNumber(5),
						[](const CatalogEntry &, const CatalogEntry &) {}));
			},
			FormatError::FERR_INVALID_STATE);

}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsEmptyTypedReservationBatchesBeforeWritingMetadata) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const CatalogSlotReservationRequest manifestRequest =
			reservationRequest(SegmentId(51), SegmentKind::SGK_VECTOR_MANIFEST_POOL);
	const PageLocation manifestPage{PageId(101), manifestRequest.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
									manifestRequest.segment_generation};
	const VectorManifestLocation manifest{manifestRequest.owner_id, manifestPage.page_id, manifestPage.generation};
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndVectorManifests(
						std::span(&manifestRequest, 1), {}, std::span(&manifest, 1), PhysicalSlotId(40),
						PhysicalSlotId(40), LogSequenceNumber(2), [](std::span<const CatalogEntry>) {}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndVectorManifests(
						std::span(&manifestRequest, 1), std::span(&manifestPage, 1), {}, PhysicalSlotId(40),
						PhysicalSlotId(40), LogSequenceNumber(2), [](std::span<const CatalogEntry>) {}));
			},
			FormatError::FERR_INVALID_HEADER);
	const CatalogSlotReservationPreparer noPreparer;
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndVectorManifests(
						std::span(&manifestRequest, 1), std::span(&manifestPage, 1), std::span(&manifest, 1),
						PhysicalSlotId(40), PhysicalSlotId(40), LogSequenceNumber(2), noPreparer));
			},
			FormatError::FERR_INVALID_HEADER);

}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsAnExtentRelocationWhoseDirectoryEntryIsMissing) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const CatalogSlotReservationRequest request = reservationRequest(SegmentId(81), SegmentKind::SGK_ENTITY_PAGE_POOL);
	const ExtentLocation extent{ExtentId(111), request.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
								RSS_PAGE_POOL_DATA_FRAME_COUNT, request.segment_generation};
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalogAndLocations(
							 std::span(&request, 1), {}, std::span(&extent, 1), PhysicalSlotId(40), PhysicalSlotId(40),
							 LogSequenceNumber(2), [](std::span<const CatalogEntry>) {})
						.has_value());
	ExtentLocation missing = extent;
	missing.extent_id = ExtentId(112);
	expectFormatError(
			[&] {
				static_cast<void>(store->relocateCatalogExtentSegment(
						missing, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3),
						[](const CatalogEntry &, const CatalogEntry &) {}));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsAnInitialPageLocationWithoutALogicalPageId) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const CatalogSlotReservationRequest request = reservationRequest(SegmentId(91), SegmentKind::SGK_INDEX_PAGE_POOL);
	const PageLocation invalidPage{PageId{}, request.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME,
								   request.segment_generation};
	expectFormatError(
			[&] {
				static_cast<void>(store->reserveFirstFreeAndUpsertCatalogAndLocations(
						std::span(&request, 1), std::span(&invalidPage, 1), {}, PhysicalSlotId(40), PhysicalSlotId(40),
						LogSequenceNumber(2), [](std::span<const CatalogEntry>) {}));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableMetadataStoreValidationTest, RejectsTailTruncationForAFileShorterThanTheBootstrap) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	dataFile->close();
	std::filesystem::resize_file(dataPath, 0U);
	dataFile->open(dataPath, std::ios::in | std::ios::out | std::ios::binary);
	ASSERT_TRUE(dataFile->is_open());
	expectFormatError([&] { static_cast<void>(store->truncateReleasedTail()); }, FormatError::FERR_BUFFER_TOO_SMALL);
}

TEST_F(RelocatableMetadataStoreValidationTest, FiltersIneligiblePoolsAndValidatesCatalogTailOwnership) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40), true, LogSequenceNumber(2)));
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(41), true, LogSequenceNumber(3)));
	CatalogEntry indexPool;
	indexPool.segment_id = SegmentId(61);
	indexPool.physical_slot = PhysicalSlotId(40);
	indexPool.kind = SegmentKind::SGK_INDEX_PAGE_POOL;
	indexPool.owner_id = 17;
	indexPool.generation = Generation(1);
	indexPool.state = SegmentState::SGS_SEALED;
	indexPool.committed_lsn = LogSequenceNumber(4);
	CatalogEntry entityPool = indexPool;
	entityPool.segment_id = SegmentId(62);
	entityPool.physical_slot = PhysicalSlotId(41);
	entityPool.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	static_cast<void>(store->upsertCatalog(std::array{indexPool, entityPool}, LogSequenceNumber(4)));
	MetadataSnapshotPin snapshot = store->openSnapshot();
	const std::vector<CatalogEntry> pools = store->findCurrentIndexPagePools(snapshot);
	ASSERT_EQ(pools.size(), 1U);
	EXPECT_EQ(pools.front().segment_id, indexPool.segment_id);
	snapshot.release();

	const CatalogSlotReservationRequest retiredRequest =
			reservationRequest(SegmentId(71), SegmentKind::SGK_ENTITY_PAGE_POOL);
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(retiredRequest, PhysicalSlotId(3), PhysicalSlotId(3),
															LogSequenceNumber(5))
						.has_value());
	expectFormatError([&] { static_cast<void>(store->truncateReleasedTail()); }, FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin dataRegionSnapshot = store->openSnapshot();
	const uint64_t dataRegionOffset = dataRegionSnapshot.superblock().data_region_offset;
	dataRegionSnapshot.release();
	const std::byte zero{};
	io->writeAt(dataRegionOffset + 41U * RSS_SEGMENT_SIZE - 1U, &zero, sizeof(zero));
	io->sync();

	const CatalogSlotReservationRequest relocatedRequest =
			reservationRequest(SegmentId(72), SegmentKind::SGK_ENTITY_PAGE_POOL);
	ASSERT_TRUE(store->reserveFirstFreeAndUpsertCatalog(relocatedRequest, PhysicalSlotId(4), PhysicalSlotId(4),
															LogSequenceNumber(6))
						.has_value());
	static_cast<void>(store->retireCatalogSegment(retiredRequest.segment_id, Generation(1), LogSequenceNumber(7)));
	ASSERT_TRUE(store->relocateCatalogSegment(relocatedRequest.segment_id, Generation(1), PhysicalSlotId(5),
															  PhysicalSlotId(5), LogSequenceNumber(8),
											  [](const CatalogEntry &, const CatalogEntry &) {})
						.has_value());
	const RelocatableTailTruncationResult tail = store->truncateReleasedTail();
	EXPECT_EQ(tail.status, RelocatableTailTruncationStatus::VTTS_NOTHING_TO_TRUNCATE);
}
