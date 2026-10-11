/**
 * @file test_RelocatableMetadataStoreGraphMutation.cpp
 * @brief Durable graph-mutation generation-fence tests for metadata publication.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreTestFixture.hpp"

#include <array>
#include <limits>
#include <optional>
#include <string>
#include <vector>

#include "graph/storage/PwriteHelper.hpp"
#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePoolAllocator.hpp"
#include "graph/storage/relocatable/allocation/SlottedPagePool.hpp"
#include "graph/storage/relocatable/entity/EntityLabelSet.hpp"
#include "graph/storage/relocatable/entity/EntityPageStore.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgePrivateFamilyMaterializer.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphCandidateLayout.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphDeferredPublication.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphFinalPublication.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueContentRunBuilder.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueReferenceMapBuilder.hpp"
#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationPlan.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/NativeBulkRun.hpp"
#include "graph/storage/relocatable/property/PropertyCollectionRoot.hpp"
#include "graph/storage/relocatable/property/LargeValueStore.hpp"
#include "graph/storage/relocatable/schema/SystemCatalog.hpp"

TEST(NativeGraphPrivateSegmentSetTest, RejectsInvalidAndForeignOrdinals) {
	expectFormatError([] { static_cast<void>(NativeGraphPrivateSegmentSet(SegmentId(1U), Generation(1U), 2U, 0U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[] {
				static_cast<void>(NativeGraphPrivateSegmentSet(SegmentId(std::numeric_limits<uint64_t>::max()),
															   Generation(1U), 2U, 1U));
			},
			FormatError::FERR_INVALID_HEADER);

	const NativeGraphPrivateSegmentSet set(SegmentId(10U), Generation(3U), 2U, 42U);
	EXPECT_EQ(set.front(), NativeGraphPrivateSegmentMaterialization(SegmentId(10U), Generation(3U), 42U));
	EXPECT_EQ(set.back(), NativeGraphPrivateSegmentMaterialization(SegmentId(11U), Generation(3U), 42U));
	EXPECT_EQ(set.ordinalOf(set.back()), 1U);
	expectFormatError([&] { static_cast<void>(set.at(2U)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(set.ordinalOf({SegmentId(12U), Generation(3U), 42U})); },
					  FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest, PublishesStreamedEdgeGraphRootsInOneGenerationWithoutLargeValues) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const NativeGraphLogicalIdReservation reservation = store->acquireNativeGraphLogicalIds(0U, 2U, 2U);
	ASSERT_EQ(reservation.edge_ranges.size(), 1U);
	const uint64_t firstEdgeId = reservation.edge_ranges.front().first_id;
	const std::vector<NativeBulkEdgeRecord> records = {
			{firstEdgeId, 1U, 2U, "KNOWS", {{"weight", graph::PropertyValue(int64_t{7})}}},
			{firstEdgeId + 1U, 2U, 1U, "KNOWS", {{"weight", graph::PropertyValue(int64_t{9})}}},
	};
	PhysicalSegmentAllocator physical(io, *store);
	NativeBulkRunStore scratch(physical, *store,
								{PhysicalSlotId(221U), PhysicalSlotId(240U), RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U});
	NativeBulkEdgeRunStreamingWriter scratchWriter(
			scratch, 901U, {RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, records.size(), 4U}, LogSequenceNumber(3U));
	for (const NativeBulkEdgeRecord &record: records) {
		scratchWriter.add(record);
	}
	const NativeBulkEdgeRunManifest scratchManifest = scratchWriter.finish();
	const NativeBulkEdgeRecordSource source =
			nativeBulkEdgeRecordSource(scratch, scratchManifest.location, 4U);
	const NativeBulkEdgePublicationPreflightOptions preflight{16U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 128U};
	const NativeStreamedGraphPublicationPlan plan = NativeStreamedGraphPublicationPlan::derive(
			source, reservation.edge_ranges, preflight, [](uint64_t) { return true; });
	ASSERT_EQ(plan.large_value_count, 0U);
	const NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(plan);
	ASSERT_NE(layout.candidate_count, 0U);

	SlottedPagePool pagePools(io, *store);
	BTreePageStore indexes(pagePools, *store);
	SystemCatalog catalog(indexes, *store);
	MetadataSnapshotPin before = store->openSnapshot();
	NativeStreamedGraphFinalPublicationRequest request;
	request.expected_metadata_generation = before.superblock().generation;
	request.edge_id_reservation = reservation;
	request.durable_source = {&scratch, scratchManifest.location, 4U};
	request.publication_plan = plan;
	request.candidate_layout = layout;
	request.private_candidate_plan = {SegmentKind::SGK_BULK_SCRATCH,
										  801U,
										  PhysicalSlotId(40U),
										  PhysicalSlotId(120U),
										  RSS_ENVELOPE_BYTES,
										  {},
										  layout.candidate_count};
	request.entity_pool_owner_id = 802U;
	request.property_pool_owner_id = 803U;
	request.first_entity_output_slot = PhysicalSlotId(121U);
	request.last_entity_output_slot = PhysicalSlotId(140U);
	request.first_property_output_slot = PhysicalSlotId(141U);
	request.last_property_output_slot = PhysicalSlotId(160U);
	request.preparation.families = {
			16U,
			16U,
			RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
			{2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	request.preparation.maximum_catalog_entries = 64U;
	request.index_planning.catalog = {PhysicalSlotId(161U), PhysicalSlotId(190U), 64U, 16U};
	request.index_planning.adjacency = {PhysicalSlotId(191U), PhysicalSlotId(220U), 16U, 16U, 4U};
	const LogSequenceNumber publicationLsn(before.superblock().checkpoint_lsn.value() + 1U);
	const Superblock published =
			store->publishStreamedEdgeGraph(request, catalog, indexes, nullptr, publicationLsn);
	EXPECT_EQ(published.generation, Generation(before.superblock().generation.value() + 1U));
	EXPECT_FALSE(store->findEdgePageLocationOptional(before, firstEdgeId).has_value());
	before.release();

	MetadataSnapshotPin current = store->openSnapshot();
	const EntityPageLocation first = store->findEdgePageLocation(current, firstEdgeId);
	const EntityPageLocation second = store->findEdgePageLocation(current, firstEdgeId + 1U);
	EXPECT_EQ(first.page_id, second.page_id);
	EXPECT_TRUE(store->findPageLocationOptional(current, first.page_id).has_value());
	const std::optional<EntityPropertyLocation> property =
			store->findEntityPropertyLocationOptional(current, {EntityPropertyOwnerKind::EPOK_EDGE, firstEdgeId});
	ASSERT_TRUE(property.has_value());
	EXPECT_TRUE(store->findPageLocationOptional(current, property->page_id).has_value());
	const std::optional<IndexRootLocation> catalogRoot =
			store->findIndexRootOptional(current, RSS_SYSTEM_CATALOG_INDEX_ID);
	const std::optional<IndexRootLocation> adjacencyRoot =
			store->findIndexRootOptional(current, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	ASSERT_TRUE(catalogRoot.has_value());
	ASSERT_TRUE(adjacencyRoot.has_value());
	EXPECT_TRUE(store->findPageLocationOptional(current, catalogRoot->root_page_id).has_value());
	EXPECT_TRUE(store->findPageLocationOptional(current, adjacencyRoot->root_page_id).has_value());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(40U)));
	current.release();
}

TEST_F(RelocatableMetadataStoreTest, PublishesStreamedEdgeGraphLargeValueRootsInOneGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const NativeGraphLogicalIdReservation reservation = store->acquireNativeGraphLogicalIds(0U, 1U, 1U);
	ASSERT_EQ(reservation.edge_ranges.size(), 1U);
	const uint64_t edgeId = reservation.edge_ranges.front().first_id;
	const NativeBulkEdgeRecord record{
			edgeId,
			1U,
			2U,
			"KNOWS",
			{{"body", graph::PropertyValue(graph::PropertyValue::Bytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, std::byte{0x5A}))}}};
	PhysicalSegmentAllocator physical(io, *store);
	NativeBulkRunStore scratch(physical, *store,
								{PhysicalSlotId(221U), PhysicalSlotId(240U), RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U});
	NativeBulkEdgeRunStreamingWriter scratchWriter(
		scratch, 902U, {RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 1U, 4U}, LogSequenceNumber(3U));
	scratchWriter.add(record);
	const NativeBulkEdgeRunManifest scratchManifest = scratchWriter.finish();
	const NativeBulkEdgeRecordSource source = nativeBulkEdgeRecordSource(scratch, scratchManifest.location, 4U);
	const NativeBulkEdgePublicationPreflightOptions preflight{16U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 128U};
	const NativeStreamedGraphPublicationPlan plan =
		NativeStreamedGraphPublicationPlan::derive(source, reservation.edge_ranges, preflight, [](uint64_t) { return true; });
	ASSERT_EQ(plan.large_value_count, 1U);

	SlottedPagePool pagePools(io, *store);
	BTreePageStore indexes(pagePools, *store);
	SystemCatalog catalog(indexes, *store);
	SharedPagePoolAllocator sharedPools(io, *store);
	const SharedPagePoolAllocationRequest existingDataPool{
			.segment_id = SegmentId(901U),
			.pool_kind = SegmentKind::SGK_LARGE_VALUE_DATA_POOL,
			.pool_owner_id = RSS_LARGE_VALUE_SHARED_DATA_POOL_OWNER_ID,
			.generation = Generation(1U),
			.page_id = PageId(900U),
			.page_owner_id = 77U,
			.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_DATA,
			.payload = {std::byte{0x11}}};
	const SharedPagePoolAllocationRequest existingManifestPool{
			.segment_id = SegmentId(902U),
			.pool_kind = SegmentKind::SGK_LARGE_VALUE_MANIFEST_POOL,
			.pool_owner_id = RSS_LARGE_VALUE_SHARED_MANIFEST_POOL_OWNER_ID,
			.generation = Generation(1U),
			.page_id = PageId(901U),
			.page_owner_id = 77U,
			.page_owner_kind = PageOwnerKind::POK_LARGE_VALUE_MANIFEST,
			.payload = {std::byte{0x22}}};
	const auto nextCheckpointLsn = [&] {
		MetadataSnapshotPin snapshot = store->openSnapshot();
		const LogSequenceNumber next(snapshot.superblock().checkpoint_lsn.value() + 1U);
		snapshot.release();
		return next;
	};
	ASSERT_TRUE(sharedPools.allocateNewPoolPage(
							existingDataPool, PhysicalSlotId(241U), PhysicalSlotId(241U), nextCheckpointLsn())
							.has_value());
	ASSERT_TRUE(sharedPools.allocateNewPoolPage(
							existingManifestPool, PhysicalSlotId(261U), PhysicalSlotId(261U), nextCheckpointLsn())
							.has_value());
	LargeValueStore values(io, *store);
	const NativeStreamedGraphLargeValueContentRunPlan contentRun =
		NativeStreamedGraphLargeValueContentRunBuilder::derivePlanForValueCount(
			values, plan.large_value_count, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U);
	const NativeStreamedGraphLargeValueReferenceMapRunPlan referenceMapRun =
		NativeStreamedGraphLargeValueReferenceMapBuilder::derivePlanForValueCount(
			plan.large_value_count, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U);
	const NativeStreamedGraphCandidateLayout layout =
		NativeStreamedGraphCandidateLayout::derive(plan, contentRun.candidate_segment_count,
													 referenceMapRun.candidate_segment_count);

	MetadataSnapshotPin before = store->openSnapshot();
	const std::vector<CatalogEntry> dataPoolsBefore = store->findCurrentLargeValueDataPagePools(before);
	const std::vector<CatalogEntry> manifestPoolsBefore = store->findCurrentLargeValueManifestPagePools(before);
	ASSERT_EQ(dataPoolsBefore.size(), 1U);
	ASSERT_EQ(manifestPoolsBefore.size(), 1U);
	NativeStreamedGraphFinalPublicationRequest request;
	request.expected_metadata_generation = before.superblock().generation;
	request.edge_id_reservation = reservation;
	request.durable_source = {&scratch, scratchManifest.location, 4U};
	request.publication_plan = plan;
	request.candidate_layout = layout;
	request.private_candidate_plan = {SegmentKind::SGK_BULK_SCRATCH,
										  804U,
										  PhysicalSlotId(40U),
										  PhysicalSlotId(120U),
										  RSS_ENVELOPE_BYTES,
										  {},
										  layout.candidate_count};
	request.entity_pool_owner_id = 805U;
	request.property_pool_owner_id = 806U;
	request.first_entity_output_slot = PhysicalSlotId(121U);
	request.last_entity_output_slot = PhysicalSlotId(140U);
	request.first_property_output_slot = PhysicalSlotId(141U);
	request.last_property_output_slot = PhysicalSlotId(160U);
	request.first_large_value_data_output_slot = PhysicalSlotId(241U);
	request.last_large_value_data_output_slot = PhysicalSlotId(260U);
	request.first_large_value_manifest_output_slot = PhysicalSlotId(261U);
	request.last_large_value_manifest_output_slot = PhysicalSlotId(280U);
	request.preparation.families = {
		16U,
		16U,
		RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES,
		{2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 16U, 4U, NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	request.preparation.maximum_catalog_entries = 64U;
	request.preparation.large_value_store = &values;
	request.preparation.large_value_content_options = {RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, 16U, 4U};
	request.index_planning.catalog = {PhysicalSlotId(281U), PhysicalSlotId(300U), 64U, 16U};
	request.index_planning.adjacency = {PhysicalSlotId(301U), PhysicalSlotId(320U), 16U, 16U, 4U};
	request.index_planning.large_value_content = {PhysicalSlotId(321U), PhysicalSlotId(340U), 16U, 16U, 4U};
	const LogSequenceNumber publicationLsn(before.superblock().checkpoint_lsn.value() + 1U);
	const Superblock published = store->publishStreamedEdgeGraph(request, catalog, indexes, &values, publicationLsn);
	EXPECT_EQ(published.generation, Generation(before.superblock().generation.value() + 1U));
	EXPECT_FALSE(store->findEdgePageLocationOptional(before, edgeId).has_value());
	before.release();

	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_TRUE(store->findEdgePageLocationOptional(current, edgeId).has_value());
	EXPECT_TRUE(store->findEntityPropertyLocationOptional(current, {EntityPropertyOwnerKind::EPOK_EDGE, edgeId}).has_value());
	const LargeValueManifestLocation route = store->findLargeValueManifest(current, LargeValueId(1U));
	const std::optional<PageLocation> manifestPage =
			store->findPageLocationOptional(current, route.manifest_page_id);
	ASSERT_TRUE(manifestPage.has_value());
	EXPECT_EQ(manifestPage->segment_id, manifestPoolsBefore.front().segment_id);
	const LargeValueManifest manifest = values.manifest(current, LargeValueId(1U));
	EXPECT_EQ(manifest.value_id, LargeValueId(1U));
	EXPECT_GT(manifest.logical_bytes, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
	ASSERT_FALSE(manifest.chunks.empty());
	EXPECT_EQ(store->findPageLocation(current, manifest.chunks.front().first_data_page_id).segment_id,
			  dataPoolsBefore.front().segment_id);
	EXPECT_EQ(store->findCurrentLargeValueDataPagePools(current).size(), 1U);
	EXPECT_EQ(store->findCurrentLargeValueManifestPagePools(current).size(), 1U);
	const std::optional<IndexRootLocation> contentRoot =
			store->findIndexRootOptional(current, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
	ASSERT_TRUE(contentRoot.has_value());
	EXPECT_TRUE(store->findPageLocationOptional(current, contentRoot->root_page_id).has_value());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(40U)));
	current.release();
}

TEST_F(RelocatableMetadataStoreTest, KeepsTransactionPrivateScratchSegmentsUnpublishedAfterTheSingleGraphGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	NativeGraphDerivedStateMutation derived;
	NativeGraphPrivateSegmentRequest scratch;
	scratch.kind = SegmentKind::SGK_BULK_SCRATCH;
	scratch.owner_id = 71U;
	scratch.first_slot = PhysicalSlotId(40U);
	scratch.last_slot = PhysicalSlotId(40U);
	scratch.payload.assign(17U, std::byte{0x42});
	derived.private_segments.push_back(scratch);
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	std::optional<NativeGraphPrivateSegmentReader> escapedReader;
	NativeGraphPrivateSegmentMaterialization candidate;
	const Superblock published = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {}, MetadataPublishOptions{},
			NativeGraphLogicalIdReservation{}, std::nullopt, {},
			[&scratch, &escapedReader, &candidate](const NativeGraphPrivateSegmentSet &identities,
												   const NativeGraphPrivateSegmentReader &reader) {
				ASSERT_EQ(identities.size(), 1U);
				EXPECT_TRUE(identities.front().isValid());
				EXPECT_EQ(identities.front().owner_id, scratch.owner_id);
				EXPECT_EQ(reader(identities.front()), scratch.payload);
				candidate = identities.front();
				escapedReader = reader;
				NativeGraphPrivateSegmentMaterialization foreign = identities.front();
				foreign.owner_id += 1U;
				expectFormatError([&] { static_cast<void>(reader(foreign)); }, FormatError::FERR_INVALID_STATE);
			});
	ASSERT_TRUE(escapedReader.has_value());
	expectFormatError([&] { static_cast<void>((*escapedReader)(candidate)); }, FormatError::FERR_INVALID_STATE);

	ASSERT_EQ(published.generation, Generation(beforeSuperblock.generation.value() + 1U));
	MetadataSnapshotPin current = store->openSnapshot();
	const SegmentId scratchId(beforeSuperblock.next_logical_segment_id);
	// Scratch is readable only through the writer-scoped capability above. A
	// successful publication must leave no durable Catalog or allocator route.
	EXPECT_THROW(static_cast<void>(store->findCatalog(current, scratchId)), FormatException);
	EXPECT_FALSE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(40U)));
	current.release();
}

TEST_F(RelocatableMetadataStoreTest, RejectsPrivateScratchPublicationBeforeTheSuperblockWithoutCatalogLeak) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	NativeGraphDerivedStateMutation derived;
	NativeGraphPrivateSegmentRequest scratch;
	scratch.kind = SegmentKind::SGK_BULK_SCRATCH;
	scratch.owner_id = 72U;
	scratch.first_slot = PhysicalSlotId(40U);
	scratch.last_slot = PhysicalSlotId(40U);
	scratch.payload.assign(23U, std::byte{0x5A});
	derived.private_segments.push_back(std::move(scratch));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	MetadataPublishOptions failBeforePublication;
	failBeforePublication.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;

	EXPECT_THROW(
			static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
					noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
					noLargeValues, derived, LogSequenceNumber(2U),
					[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
					   std::span<EntityPageNewPoolBatchMaterialization>,
					   std::span<NativeGraphPropertyPageReplacementMaterialization>,
					   std::span<NativeGraphPropertyPageBatchMaterialization>,
					   std::span<LargeValueMaterializationResult>) {},
					[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {},
					failBeforePublication)),
			BootstrapPublishInterrupted);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
	EXPECT_THROW(static_cast<void>(store->findCatalog(unchanged, SegmentId(beforeSuperblock.next_logical_segment_id))),
				 FormatException);
	unchanged.release();
}

TEST_F(RelocatableMetadataStoreTest, RejectsDeferredPrivateScratchPlanningWithoutPublishingCandidateState) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	NativeGraphDerivedStateMutation derived;
	NativeGraphPrivateSegmentRequest scratch;
	scratch.kind = SegmentKind::SGK_BULK_SCRATCH;
	scratch.owner_id = 73U;
	scratch.first_slot = PhysicalSlotId(40U);
	scratch.last_slot = PhysicalSlotId(40U);
	scratch.payload.assign(29U, std::byte{0x3C});
	derived.private_segments.push_back(std::move(scratch));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, derived, LogSequenceNumber(2U),
						[](std::span<EntityPageReplacementMaterialization>,
						   std::span<EntityPageAppendBatchMaterialization>,
						   std::span<EntityPageNewPoolBatchMaterialization>,
						   std::span<NativeGraphPropertyPageReplacementMaterialization>,
						   std::span<NativeGraphPropertyPageBatchMaterialization>,
						   std::span<LargeValueMaterializationResult>) {},
						[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {},
						MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt, {},
						[](const NativeGraphPrivateSegmentSet &, const NativeGraphPrivateSegmentReader &) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
												  "deferred scratch planner rejected its candidate");
						}));
			},
			FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
	EXPECT_THROW(static_cast<void>(store->findCatalog(unchanged, SegmentId(beforeSuperblock.next_logical_segment_id))),
				 FormatException);
	unchanged.release();
}

TEST_F(RelocatableMetadataStoreTest,
	   RejectsDeferredIndexPlanWithConflictingFreshAndStableRootTransitionsWithoutPublishingScratch) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	NativeGraphDerivedStateMutation derived;
	NativeGraphPrivateSegmentRequest scratch;
	scratch.kind = SegmentKind::SGK_BULK_SCRATCH;
	scratch.owner_id = 78U;
	scratch.first_slot = PhysicalSlotId(40U);
	scratch.last_slot = PhysicalSlotId(40U);
	scratch.payload.assign(43U, std::byte{0x4E});
	derived.private_segments.push_back(std::move(scratch));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool plannerCalled = false;
	bool preparerCalled = false;

	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, derived, LogSequenceNumber(2U),
						[&preparerCalled](std::span<EntityPageReplacementMaterialization>,
											 std::span<EntityPageAppendBatchMaterialization>,
											 std::span<EntityPageNewPoolBatchMaterialization>,
											 std::span<NativeGraphPropertyPageReplacementMaterialization>,
											 std::span<NativeGraphPropertyPageBatchMaterialization>,
											 std::span<LargeValueMaterializationResult>) { preparerCalled = true; },
						[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {},
						MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt,
						{}, {}, [&plannerCalled](const NativeGraphDeferredIndexPlanningContext &) {
							plannerCalled = true;
							NativeGraphDeferredIndexPlan plan;
							plan.mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
							plan.mutation.new_pool_requests.push_back(
									{PhysicalSlotId(40U), PhysicalSlotId(40U), RSS_GLOBAL_ADJACENCY_INDEX_ID, 1U});
							plan.mutation.replacement_root = {0U, 0U, true};
							plan.mutation.stable_replacement_root = PageId(1U);
							plan.preparer = [](std::vector<IndexTreeMutationPoolBatch> &) {};
							return plan;
						}));
			},
			FormatError::FERR_INVALID_STATE);

	EXPECT_TRUE(plannerCalled);
	EXPECT_FALSE(preparerCalled);
	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
	EXPECT_THROW(static_cast<void>(store->findCatalog(unchanged, SegmentId(beforeSuperblock.next_logical_segment_id))),
				 FormatException);
	unchanged.release();

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock(), beforeSuperblock);
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	EXPECT_THROW(static_cast<void>(reopened->findCatalog(
					 recovered, SegmentId(beforeSuperblock.next_logical_segment_id))),
				 FormatException);
	recovered.release();
}

TEST_F(RelocatableMetadataStoreTest,
	   EarlyPrivateCandidatePlanningReadsOnlyTheWriterFencedCandidateBeforeFinalMaterialization) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	NativeGraphDerivedStateMutation derived;
	NativeGraphPrivateSegmentRequest scratch;
	scratch.kind = SegmentKind::SGK_BULK_SCRATCH;
	scratch.owner_id = 74U;
	scratch.first_slot = PhysicalSlotId(40U);
	scratch.last_slot = PhysicalSlotId(40U);
	scratch.payload.assign(31U, std::byte{0x4D});
	derived.private_segments.push_back(scratch);
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool earlyPlannerRan = false;
	bool downstreamConsumerRan = false;
	std::optional<NativeGraphPrivateSegmentReader> escapedReader;
	NativeGraphPrivateSegmentMaterialization candidate;

	const Superblock published = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {}, MetadataPublishOptions{},
			NativeGraphLogicalIdReservation{}, std::nullopt, {},
			[&](const NativeGraphPrivateSegmentSet &, const NativeGraphPrivateSegmentReader &) {
				downstreamConsumerRan = earlyPlannerRan;
			},
			{},
			[&](const NativeGraphEarlyPublicationPlanningContext &context) {
				ASSERT_EQ(context.current_snapshot.superblock(), beforeSuperblock);
				ASSERT_EQ(context.private_segments.size(), 1U);
				candidate = context.private_segments.front();
				EXPECT_EQ(context.private_segment_reader(candidate), scratch.payload);
				escapedReader = context.private_segment_reader;
				earlyPlannerRan = true;
			});

	EXPECT_TRUE(earlyPlannerRan);
	EXPECT_TRUE(downstreamConsumerRan);
	EXPECT_EQ(published.generation, Generation(beforeSuperblock.generation.value() + 1U));
	ASSERT_TRUE(escapedReader.has_value());
	expectFormatError([&] { static_cast<void>((*escapedReader)(candidate)); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableMetadataStoreTest,
	   EarlyPrivateCandidatePlanningCanFillOnlyReservedCandidatesBeforeTheSinglePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	NativeGraphDerivedStateMutation derived;
	derived.private_segment_plan = {SegmentKind::SGK_BULK_SCRATCH,
									78U,
									PhysicalSlotId(1U),
									PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
									RSS_ENVELOPE_BYTES,
									{},
									2U};
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	const std::vector<std::byte> sourcePayload(29U, std::byte{0x62});
	const std::vector<std::byte> materializedPayload(31U, std::byte{0x63});
	std::optional<NativeGraphPrivateSegmentWriter> escapedWriter;
	NativeGraphPrivateSegmentMaterialization reserved;

	const Superblock published = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {}, MetadataPublishOptions{},
			NativeGraphLogicalIdReservation{}, std::nullopt,
			[sourcePayload](const NativeGraphPrivateSegmentSet &, size_t ordinal) {
				return ordinal == 0U ? sourcePayload : NativeGraphPrivateSegmentReservation::payload();
			},
			{}, {},
			[&](const NativeGraphEarlyPublicationPlanningContext &context) {
				ASSERT_EQ(context.private_segments.size(), 2U);
				EXPECT_EQ(context.private_segment_reader(context.private_segments.front()), sourcePayload);
				reserved = context.private_segments[1U];
				expectFormatError([&] { static_cast<void>(context.private_segment_reader(reserved)); },
								  FormatError::FERR_INVALID_STATE);
				context.private_segment_writer(reserved, materializedPayload);
				EXPECT_EQ(context.private_segment_reader(reserved), materializedPayload);
				expectFormatError(
						[&] { context.private_segment_writer(reserved, std::vector<std::byte>(17U, std::byte{0x64})); },
						FormatError::FERR_INVALID_STATE);
				escapedWriter = context.private_segment_writer;
			});

	EXPECT_EQ(published.generation, Generation(beforeSuperblock.generation.value() + 1U));
	ASSERT_TRUE(escapedWriter.has_value());
	expectFormatError([&] { (*escapedWriter)(reserved, materializedPayload); }, FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_THROW(static_cast<void>(store->findCatalog(current, SegmentId(beforeSuperblock.next_logical_segment_id))),
				 FormatException);
	current.release();
}

TEST_F(RelocatableMetadataStoreTest, KeepsAnExternalPrivateSourceTailOutOfEveryFinalPublicPoolAllocation) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	const uint64_t initialFileSize = io->fileSize();
	ASSERT_GE(initialFileSize, beforeSuperblock.data_region_offset);
	ASSERT_EQ((initialFileSize - beforeSuperblock.data_region_offset) % RSS_SEGMENT_SIZE, 0U);
	const PhysicalSlotId sourceSlot((initialFileSize - beforeSuperblock.data_region_offset) / RSS_SEGMENT_SIZE + 1U);
	before.release();

	// The sealed streamed input remains outside the published allocator while
	// the candidate and final pools are materialized in this one generation.
	ASSERT_EQ(io->reserveAppendSpace(RSS_SEGMENT_SIZE),
			  PhysicalSegmentAllocator::checkedSlotOffset(beforeSuperblock.data_region_offset, sourceSlot));
	const PhysicalSlotId firstAvailablePublicSlot(sourceSlot.value() + 1U);

	NativeGraphDerivedStateMutation derived;
	derived.external_private_tail_reservation = {sourceSlot, sourceSlot};
	derived.private_segment_plan = {SegmentKind::SGK_BULK_SCRATCH,
									91U,
									PhysicalSlotId(1U),
									PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
									RSS_ENVELOPE_BYTES,
									{},
									1U};
	NativeGraphIndexTreeMutation index;
	index.index_id = 91U;
	index.new_pool_requests.push_back({sourceSlot, firstAvailablePublicSlot, index.index_id, 1U});
	index.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
	derived.index_tree_mutations.push_back(std::move(index));

	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	SlottedPagePool pagePools(io, *store);
	BTreePageStore indexes(pagePools, *store);

	const Superblock published = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[&indexes](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				ASSERT_EQ(batches.size(), 1U);
				ASSERT_EQ(batches.front().size(), 1U);
				IndexTreeMutationPoolBatch &batch = batches.front().front();
				ASSERT_TRUE(batch.newly_reserved);
				ASSERT_EQ(batch.appended_page_ids.size(), 1U);
				const PageOwnerHeader owner{batch.appended_page_ids.front(),
											91U,
											PageOwnerKind::POK_INDEX,
											0U,
											RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
											batch.pool.generation,
											LogSequenceNumber(2U)};
				BTreePageHeader header;
				header.index_id = 91U;
				header.page_id = batch.appended_page_ids.front();
				header.generation = batch.pool.generation;
				const std::array<BTreePage, 1U> pages{{BTreePage::create(owner, header)}};
				indexes.prepareNewPoolInitialPages(batch.pool, pages, batch.appended_locations, LogSequenceNumber(2U));
			},
			{}, {}, {}, {}, {}, MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt,
			[](const NativeGraphPrivateSegmentSet &, size_t) { return std::vector<std::byte>(1U, std::byte{0x6B}); });

	EXPECT_EQ(published.generation, Generation(beforeSuperblock.generation.value() + 1U));
	MetadataSnapshotPin current = store->openSnapshot();
	const IndexRootLocation root = store->findIndexRoot(current, 91U);
	const PageLocation rootPage = store->findPageLocation(current, root.root_page_id);
	const CatalogEntry publicPool = store->findCatalog(current, rootPage.segment_id);
	// The compact candidate is never consumed, so it must not preempt the first
	// allocator-visible public slot after the external source tail.
	EXPECT_EQ(publicPool.physical_slot, firstAvailablePublicSlot);
	EXPECT_FALSE(store->isPhysicalSlotAllocated(current, sourceSlot));
	EXPECT_TRUE(store->isPhysicalSlotAllocated(current, firstAvailablePublicSlot));
	current.release();
}

TEST_F(RelocatableMetadataStoreTest, EarlyPlanningDerivesAStreamedPublicationPlanWithoutCopyingTheDurableSource) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	const bool beforeSlotOneAllocated = store->isPhysicalSlotAllocated(before, PhysicalSlotId(1U));
	before.release();

	std::vector<NativeBulkEdgeRecord> records;
	for (uint64_t edgeId = 100U; edgeId < 104U; ++edgeId) {
		NativeBulkEdgeRecord record;
		record.edge_id = edgeId;
		record.source_node_id = edgeId - 99U;
		record.target_node_id = edgeId - 98U;
		record.type = "REL_" + std::to_string(edgeId);
		record.properties = {{"key_" + std::to_string(edgeId), graph::PropertyValue(int64_t(edgeId))}};
		records.push_back(std::move(record));
	}
	const NativeBulkEdgeRecordSource source = [records]() {
		size_t ordinal = 0U;
		return [records, ordinal]() mutable -> std::optional<NativeBulkEdgeRecord> {
			return ordinal == records.size() ? std::nullopt : std::optional<NativeBulkEdgeRecord>(records[ordinal++]);
		};
	};
	const size_t runBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + NativeBulkEdgeRecordCodec::encodedSize(records.front()) +
							RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + 1U;
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, static_cast<uint64_t>(records.size())}};
	const NativeBulkEdgePublicationPreflightOptions options{records.size(), runBytes, 2U, 512U};
	const NativeStreamedGraphPublicationPlan expectedPlan = NativeStreamedGraphPublicationPlan::derive(
			source, lease, options, [](uint64_t nodeId) { return nodeId >= 1U && nodeId <= 5U; });
	const NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(expectedPlan);
	NativeGraphDerivedStateMutation derived;
	derived.private_segment_plan = {SegmentKind::SGK_BULK_SCRATCH,
									75U,
									PhysicalSlotId(1U),
									PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
									RSS_ENVELOPE_BYTES,
									{},
									layout.candidate_count};
	std::optional<NativeStreamedGraphPublicationPlan> publicationPlan;
	const NativeGraphPrivateSegmentImagePreparer reservations =
			[](const NativeGraphPrivateSegmentSet &identities, size_t ordinal) {
				if (ordinal >= identities.size()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
									  "streamed publication reservation ordinal is out of range");
				}
				return NativeGraphPrivateSegmentReservation::payload();
			};

	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	NativeGraphPrivateSegmentFence escapedFence;
	const Superblock published = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {}, MetadataPublishOptions{},
				NativeGraphLogicalIdReservation{}, std::nullopt, reservations, {}, {},
				[&](const NativeGraphEarlyPublicationPlanningContext &context) {
					EXPECT_EQ(context.private_segments.size(), layout.candidate_count);
					EXPECT_THROW(context.deferred_index_plan_sink(NativeGraphDeferredIndexPlan{}), FormatException);
					escapedFence = context.private_segment_fence;
					publicationPlan = NativeStreamedGraphPublicationPlan::derive(
							source, lease, options, [](uint64_t nodeId) { return nodeId >= 1U && nodeId <= 5U; });
				});

	ASSERT_TRUE(publicationPlan.has_value());
	EXPECT_EQ(publicationPlan->record_count, records.size());
	EXPECT_EQ(publicationPlan->record_count, expectedPlan.record_count);
	EXPECT_EQ(publicationPlan->maximumCandidateSegmentCount(), expectedPlan.maximumCandidateSegmentCount());
	EXPECT_GT(publicationPlan->source_run_count, 0U);
	EXPECT_EQ(layout.candidate_count, publicationPlan->maximumCandidateSegmentCount());
	EXPECT_EQ(published.generation, Generation(beforeSuperblock.generation.value() + 1U));
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(store->isPhysicalSlotAllocated(current, PhysicalSlotId(1U)), beforeSlotOneAllocated);
	EXPECT_THROW(static_cast<void>(store->findCatalog(current, SegmentId(beforeSuperblock.next_logical_segment_id))),
				 FormatException);
	current.release();
	// Planner capabilities are scoped to the writer-fenced private candidate.
	// Retaining a callback after publication must not provide a later I/O path.
	EXPECT_THROW(escapedFence(), FormatException);
}

TEST_F(RelocatableMetadataStoreTest, EarlyPlanningMaterializesAllPrivateBulkFamiliesInsideOneCandidateLayout) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	std::vector<NativeBulkEdgeRecord> records;
	for (uint64_t edgeId = 100U; edgeId < 104U; ++edgeId) {
		NativeBulkEdgeRecord record;
		record.edge_id = edgeId;
		record.source_node_id = edgeId - 99U;
		record.target_node_id = edgeId - 98U;
		record.type = "REL_" + std::to_string(edgeId);
		record.properties = {{"key_" + std::to_string(edgeId), graph::PropertyValue(int64_t(edgeId))}};
		records.push_back(std::move(record));
	}
	const NativeBulkEdgeRecordSource source = [records]() {
		size_t ordinal = 0U;
		return [records, ordinal]() mutable -> std::optional<NativeBulkEdgeRecord> {
			return ordinal == records.size() ? std::nullopt : std::optional<NativeBulkEdgeRecord>(records[ordinal++]);
		};
	};
	const size_t runBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + NativeBulkEdgeRecordCodec::encodedSize(records.front()) +
							RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES + 1U;
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, static_cast<uint64_t>(records.size())}};
	const NativeBulkEdgePublicationPreflightOptions preflightOptions{records.size(), runBytes, 2U, 512U};
	const NativeStreamedGraphPublicationPlan publicationPlan = NativeStreamedGraphPublicationPlan::derive(
			source, lease, preflightOptions, [](uint64_t nodeId) { return nodeId >= 1U && nodeId <= 5U; });
	const NativeStreamedGraphCandidateLayout layout = NativeStreamedGraphCandidateLayout::derive(publicationPlan);

	NativeGraphDerivedStateMutation derived;
	derived.private_segment_plan = {SegmentKind::SGK_BULK_SCRATCH,
									75U,
									PhysicalSlotId(1U),
									PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
									RSS_ENVELOPE_BYTES,
									{},
									layout.candidate_count};
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	std::optional<NativeBulkRunFragmentLocation> tokenRoot;
	std::optional<NativeBulkEdgePrivateAdjacencyRoots> adjacencyRoots;
	const NativeBulkEdgePrivateFamilyMaterializationOptions familyOptions{
			records.size(),
			publicationPlan.token_entry_count,
			runBytes,
			{preflightOptions.maximum_sort_runs, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 1U, 4U,
				 NativeBulkEdgeRunDuplicatePolicy::NBERDP_REJECT}};
	const NativeGraphPrivateSegmentImagePreparer reservations =
			[](const NativeGraphPrivateSegmentSet &identities, size_t ordinal) {
				if (ordinal >= identities.size()) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
									  "streamed publication reservation ordinal is out of range");
				}
				return NativeGraphPrivateSegmentReservation::payload();
			};

	const Superblock published = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {}, MetadataPublishOptions{},
			NativeGraphLogicalIdReservation{}, std::nullopt, reservations, {}, {},
			[&](const NativeGraphEarlyPublicationPlanningContext &context) {
				tokenRoot = NativeBulkEdgePrivateFamilyMaterializer::materializeTokens(
						source,
						context.private_segments, {layout.tokens.first_ordinal, layout.tokens.count}, familyOptions,
						context.private_segment_reader, context.private_segment_writer, context.private_segment_fence);
				adjacencyRoots = NativeBulkEdgePrivateFamilyMaterializer::materializeAdjacency(
						source,
						lease, context.private_segments,
						{layout.outgoing_adjacency.first_ordinal, layout.outgoing_adjacency.count},
						{layout.incoming_adjacency.first_ordinal, layout.incoming_adjacency.count}, familyOptions,
						context.private_segment_reader, context.private_segment_writer, context.private_segment_fence);
			});

	ASSERT_TRUE(tokenRoot.has_value());
	ASSERT_TRUE(adjacencyRoots.has_value());
	EXPECT_TRUE(tokenRoot->isValid());
	EXPECT_TRUE(adjacencyRoots->outgoing.isValid());
	EXPECT_TRUE(adjacencyRoots->incoming.isValid());
	EXPECT_EQ(published.generation, Generation(beforeSuperblock.generation.value() + 1U));
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_THROW(static_cast<void>(store->findCatalog(current, SegmentId(beforeSuperblock.next_logical_segment_id))),
				 FormatException);
	current.release();
}

TEST_F(RelocatableMetadataStoreTest, EarlyPlanningCanQueueOneDeferredIndexPlanForTheFinalPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	SlottedPagePool pagePools(io, *store);
	BTreePageStore indexes(pagePools, *store);
	NativeGraphDerivedStateMutation derived;
	derived.private_segment_plan = {SegmentKind::SGK_BULK_SCRATCH,
									81U,
									PhysicalSlotId(1U),
									PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
									RSS_ENVELOPE_BYTES,
									{},
									1U};
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

	const Superblock published = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &ordinaryBatches) {
				EXPECT_TRUE(ordinaryBatches.empty());
			},
			{}, {}, {}, {}, {}, MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt,
			[](const NativeGraphPrivateSegmentSet &, size_t) { return std::vector<std::byte>(1U, std::byte{0x6C}); },
			{}, {},
			[&indexes](const NativeGraphEarlyPublicationPlanningContext &context) {
				NativeGraphDeferredIndexPlan plan;
				plan.mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
				plan.mutation.new_pool_requests.push_back(
						{PhysicalSlotId(40U), PhysicalSlotId(42U), RSS_GLOBAL_ADJACENCY_INDEX_ID, 1U});
				plan.mutation.replacement_root = {0U, 0U, true};
				plan.preparer = [&indexes](std::vector<IndexTreeMutationPoolBatch> &batches) {
					ASSERT_EQ(batches.size(), 1U);
					ASSERT_EQ(batches.front().appended_page_ids.size(), 1U);
					const PageOwnerHeader owner{batches.front().appended_page_ids.front(),
										 RSS_GLOBAL_ADJACENCY_INDEX_ID,
										 PageOwnerKind::POK_INDEX,
										 0U,
										 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
										 batches.front().pool.generation,
										 LogSequenceNumber(2U)};
					BTreePageHeader header;
					header.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
					header.page_id = batches.front().appended_page_ids.front();
					header.generation = batches.front().pool.generation;
					const std::array<BTreePage, 1U> pages{{BTreePage::create(owner, header)}};
					indexes.prepareNewPoolInitialPages(batches.front().pool, pages,
													batches.front().appended_locations, LogSequenceNumber(2U));
				};
				context.deferred_index_plan_sink(std::move(plan));
			});

	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(current.superblock(), published);
	const IndexRootLocation root = store->findIndexRoot(current, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	const PageLocation rootPage = store->findPageLocation(current, root.root_page_id);
	EXPECT_EQ(store->findCatalog(current, rootPage.segment_id).kind, SegmentKind::SGK_INDEX_PAGE_POOL);
	current.release();
}

TEST_F(RelocatableMetadataStoreTest, EarlyPrivateCandidatePlanningFailureLeavesNoPublishedGenerationOrScratchRoute) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	NativeGraphDerivedStateMutation derived;
	NativeGraphPrivateSegmentRequest scratch;
	scratch.kind = SegmentKind::SGK_BULK_SCRATCH;
	scratch.owner_id = 75U;
	scratch.first_slot = PhysicalSlotId(40U);
	scratch.last_slot = PhysicalSlotId(40U);
	scratch.payload.assign(37U, std::byte{0x77});
	derived.private_segments.push_back(std::move(scratch));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, derived, LogSequenceNumber(2U),
						[](std::span<EntityPageReplacementMaterialization>,
						   std::span<EntityPageAppendBatchMaterialization>,
						   std::span<EntityPageNewPoolBatchMaterialization>,
						   std::span<NativeGraphPropertyPageReplacementMaterialization>,
						   std::span<NativeGraphPropertyPageBatchMaterialization>,
						   std::span<LargeValueMaterializationResult>) {},
						[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {},
						MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt, {}, {}, {},
						[](const NativeGraphEarlyPublicationPlanningContext &) {
							throw FormatException(FormatError::FERR_INVALID_STATE,
												  "early candidate planner rejected its writer-scoped input");
						}));
			},
			FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
	EXPECT_THROW(static_cast<void>(store->findCatalog(unchanged, SegmentId(beforeSuperblock.next_logical_segment_id))),
				 FormatException);
	unchanged.release();
}

TEST_F(RelocatableMetadataStoreTest, AssignsAllPrivateScratchIdentitiesBeforePreparingDependentImages) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	constexpr size_t candidateCount = 64U;
	NativeGraphDerivedStateMutation derived;
	// A streamed producer describes the homogeneous candidate once rather than
	// retaining one duplicate request for every scratch fragment.
	derived.private_segment_plan = {SegmentKind::SGK_BULK_SCRATCH,
									74U,
									PhysicalSlotId(1U),
									PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
									RSS_ENVELOPE_BYTES,
									{},
									candidateCount};
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	std::vector<NativeGraphPrivateSegmentMaterialization> assigned;
	const Superblock published = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {}, MetadataPublishOptions{},
			NativeGraphLogicalIdReservation{}, std::nullopt,
		[&assigned, candidateCount](const NativeGraphPrivateSegmentSet &identities, size_t ordinal) {
				assigned.clear();
				for (size_t identityOrdinal = 0U; identityOrdinal < identities.size(); ++identityOrdinal) {
					assigned.push_back(identities[identityOrdinal]);
				}
				EXPECT_EQ(identities.size(), candidateCount);
				return std::vector<std::byte>{std::byte{static_cast<unsigned char>(ordinal)}};
			},
			{}, {},
			[candidateCount](const NativeGraphEarlyPublicationPlanningContext &context) {
				ASSERT_EQ(context.private_segments.size(), candidateCount);
				EXPECT_EQ(context.private_segment_reader(context.private_segments.front()),
						  std::vector<std::byte>({std::byte{0x00}}));
			});
	ASSERT_EQ(assigned.size(), candidateCount);
	for (size_t ordinal = 1U; ordinal < assigned.size(); ++ordinal) {
		EXPECT_EQ(assigned[ordinal].segment_id.value(), assigned.front().segment_id.value() + ordinal);
	}
	MetadataSnapshotPin current = store->openSnapshot();
	for (const NativeGraphPrivateSegmentMaterialization &identity: assigned) {
		EXPECT_THROW(static_cast<void>(store->findCatalog(current, identity.segment_id)), FormatException);
	}
	EXPECT_EQ(current.superblock(), published);
	current.release();
}

TEST_F(RelocatableMetadataStoreTest, FailsClosedWhenFallbackIoCannotAbandonCompactPrivateScratchTail) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const uint64_t initialFileSize = io->fileSize();
	NativeGraphDerivedStateMutation derived;
	derived.private_segment_plan = {SegmentKind::SGK_BULK_SCRATCH,
									79U,
									PhysicalSlotId(1U),
									PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
									RSS_ENVELOPE_BYTES,
									{},
									3U};
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	MetadataPublishOptions failBeforePublication;
	failBeforePublication.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;

	EXPECT_THROW(
			static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
					noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
					noLargeValues, derived, LogSequenceNumber(2U),
					[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
					   std::span<EntityPageNewPoolBatchMaterialization>,
					   std::span<NativeGraphPropertyPageReplacementMaterialization>,
					   std::span<NativeGraphPropertyPageBatchMaterialization>,
					   std::span<LargeValueMaterializationResult>) {},
					[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {},
					failBeforePublication, NativeGraphLogicalIdReservation{}, std::nullopt,
					[](const NativeGraphPrivateSegmentSet &, size_t ordinal) {
						return std::vector<std::byte>(ordinal + 1U, std::byte{0x6A});
					},
					[](const NativeGraphPrivateSegmentSet &identities, const NativeGraphPrivateSegmentReader &reader) {
						static_cast<void>(reader(identities.front()));
					})),
			BootstrapPublishInterrupted);

	// This fixture deliberately has no native truncate handle. The candidate
	// suffix is therefore retained but unreachable, and the allocator must reject
	// any later reuse rather than silently treating unknown bytes as free space.
	EXPECT_GT(io->fileSize(), initialFileSize);
	EXPECT_THROW(static_cast<void>(store->upsertCatalog({}, LogSequenceNumber(3U))), FormatException);
}

TEST_F(RelocatableMetadataStoreTest, NativeIoTruncatesFailedCompactPrivateScratchTailAndPermitsSafeRetryAfterReopen) {
	const file_handle_t nativeHandle = portable_open_rw(testFilePath.string().c_str());
	ASSERT_NE(nativeHandle, INVALID_FILE_HANDLE);
	io = std::make_shared<StorageIO>(file, nativeHandle, nativeHandle);
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const uint64_t initialFileSize = io->fileSize();
	const Superblock initialSuperblock = store->openSnapshot().superblock();
	ASSERT_GE(initialFileSize, initialSuperblock.data_region_offset);
	ASSERT_EQ((initialFileSize - initialSuperblock.data_region_offset) % RSS_SEGMENT_SIZE, 0U);
	const PhysicalSlotId firstScratchSlot((initialFileSize - initialSuperblock.data_region_offset) / RSS_SEGMENT_SIZE +
										  1U);

	NativeGraphDerivedStateMutation derived;
	derived.private_segment_plan = {SegmentKind::SGK_BULK_SCRATCH,
									81U,
									PhysicalSlotId(1U),
									PhysicalSlotId(std::numeric_limits<uint64_t>::max()),
									RSS_ENVELOPE_BYTES,
									{},
									3U};
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	const auto prepare = [](const NativeGraphPrivateSegmentSet &, size_t ordinal) {
		return std::vector<std::byte>(ordinal + 1U, std::byte{0x6A});
	};
	MetadataPublishOptions failBeforePublication;
	failBeforePublication.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;

	EXPECT_THROW(
			static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
					noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
					noLargeValues, derived, LogSequenceNumber(2U),
					[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
					   std::span<EntityPageNewPoolBatchMaterialization>,
					   std::span<NativeGraphPropertyPageReplacementMaterialization>,
					   std::span<NativeGraphPropertyPageBatchMaterialization>,
					   std::span<LargeValueMaterializationResult>) {},
					[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {},
					failBeforePublication, NativeGraphLogicalIdReservation{}, std::nullopt, prepare)),
			BootstrapPublishInterrupted);

	EXPECT_EQ(io->fileSize(), initialFileSize);
	MetadataSnapshotPin afterFailure = store->openSnapshot();
	EXPECT_EQ(afterFailure.superblock(), initialSuperblock);
	EXPECT_THROW(
			static_cast<void>(store->findCatalog(afterFailure, SegmentId(initialSuperblock.next_logical_segment_id))),
			FormatException);
	afterFailure.release();

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin afterReopen = reopened->openSnapshot();
	EXPECT_EQ(afterReopen.superblock(), initialSuperblock);
	afterReopen.release();

	const Superblock published = reopened->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {}, MetadataPublishOptions{},
			NativeGraphLogicalIdReservation{}, std::nullopt, prepare);
	EXPECT_EQ(published.generation, Generation(initialSuperblock.generation.value() + 1U));
	MetadataSnapshotPin afterRetry = reopened->openSnapshot();
	EXPECT_EQ(afterRetry.superblock(), published);
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(afterRetry, firstScratchSlot));
	afterRetry.release();

	reopened.reset();
	io.reset();
	EXPECT_EQ(portable_close_rw(nativeHandle), 0);
}

TEST_F(RelocatableMetadataStoreTest, RejectsConflictingOrEntityMappedAuxiliaryNewPoolsBeforeReservingAnyPhysicalSlot) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};

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

	EntityPageNewPoolBatchRequest entityMappedAuxiliary = first;
	entityMappedAuxiliary.inserted_nodes = {{1U, PageId(101U), 0U, 1U}};
	const std::array<EntityPageNewPoolBatchRequest, 1U> malformedAuxiliary{{entityMappedAuxiliary}};
	expectRejected(malformedAuxiliary);
}

TEST_F(RelocatableMetadataStoreTest,
	   RejectsExhaustedNewPoolRangeWithoutCallingTheMaterializerOrPublishingAnyPartialReservation) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(40U), true, LogSequenceNumber(2U)));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();

	CatalogSlotReservationRequest reservation;
	reservation.segment_id = SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID);
	reservation.kind = SegmentKind::SGK_ENTITY_PAGE_POOL;
	reservation.owner_id = 17U;
	reservation.segment_generation = Generation(1U);
	reservation.state = SegmentState::SGS_SEALED;
	reservation.require_absent = true;
	const std::array<EntityPageNewPoolBatchRequest, 1U> exhaustedPool{{
			{reservation,
			 PageId(101U),
			 PhysicalSlotId(40U),
			 PhysicalSlotId(40U),
			 {},
			 {},
			 PageOwnerKind::POK_ENTITY_LABEL},
	}};
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool materializerCalled = false;

	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, exhaustedPool, noPropertyReplacements, noPropertyBatches,
						noLargeValues, {}, LogSequenceNumber(3U),
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
	EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
	EXPECT_TRUE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
	expectFormatError([&] { static_cast<void>(store->findCatalog(unchanged, reservation.segment_id)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(store->findPageLocationOptional(unchanged, PageId(101U)).has_value());
	unchanged.release();

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock(), beforeSuperblock);
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	expectFormatError([&] { static_cast<void>(reopened->findCatalog(recovered, reservation.segment_id)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(reopened->findPageLocationOptional(recovered, PageId(101U)).has_value());
	recovered.release();
}

TEST_F(RelocatableMetadataStoreTest, RejectsEmptyOrIncoherentGraphPublicationBeforeInvokingAnyTypedPreparer) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool graphPreparerCalled = false;
	bool vectorPreparerCalled = false;

	const auto graphPreparer = [&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
													  std::span<EntityPageAppendBatchMaterialization>,
													  std::span<EntityPageNewPoolBatchMaterialization>,
													  std::span<NativeGraphPropertyPageReplacementMaterialization>,
													  std::span<NativeGraphPropertyPageBatchMaterialization>,
													  std::span<LargeValueMaterializationResult>) {
		graphPreparerCalled = true;
	};
	const NativeGraphDerivedStatePreparer noDerivedStatePreparer{};
	const NativeGraphVectorBootstrapPreparer unexpectedBootstrapPreparer =
			[&vectorPreparerCalled](std::vector<NativeGraphVectorBootstrapMaterialization> &) {
				vectorPreparerCalled = true;
			};
	const NativeGraphVectorDeltaPreparer noDeltaPreparer{};
	const std::array<PageRetirement, 0U> noRetiredLargeValuePages{};
	const std::array<LargeValueId, 0U> noErasedLargeValueIds{};
	const ExistingPoolPagesRetirementPreparer noLargeValueRetirementPreparer{};
	const MetadataPublishOptions noPublishOptions{};

	// A graph publication is not a generic metadata write. Empty work must not
	// advance a generation merely because a caller supplied a callback.
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, {}, LogSequenceNumber(2U), graphPreparer, noDerivedStatePreparer));
			},
			FormatError::FERR_INVALID_HEADER);

	// A durable logical-ID lease is independently publishable graph work. It
	// must still reject an unsolicited Vector materializer before it can claim
	// the lease or enter either callback.
	NativeGraphLogicalIdReservation logicalIds;
	logicalIds.node_ranges.push_back({1U, 1U});
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, {}, LogSequenceNumber(2U), graphPreparer, noDerivedStatePreparer,
						unexpectedBootstrapPreparer, noDeltaPreparer, noRetiredLargeValuePages, noErasedLargeValueIds,
						noLargeValueRetirementPreparer, noPublishOptions, logicalIds));
			},
			FormatError::FERR_INVALID_HEADER);

	EXPECT_FALSE(graphPreparerCalled);
	EXPECT_FALSE(vectorPreparerCalled);
	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
	unchanged.release();
}

TEST_F(RelocatableMetadataStoreTest, RejectsIncompleteCoordinatorInputsBeforeTakingTheWriterOrCallingAnyPreparer) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	before.release();
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	const std::array<PageRetirement, 1U> oneRetirement{{
			{{PageId(9U), SegmentId(7U), RSS_PAGE_POOL_FIRST_DATA_FRAME, Generation(1U), 0U},
			 1U,
			 PageOwnerKind::POK_LARGE_VALUE_DATA,
			 LogSequenceNumber(1U)},
	}};
	const std::array<LargeValueId, 0U> noErasedLargeValueIds{};

	bool graphPreparerCalled = false;
	bool derivedPreparerCalled = false;
	const NativeGraphPageTransactionWithLargeValuesPreparer graphPreparer =
			[&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
								   std::span<EntityPageAppendBatchMaterialization>,
								   std::span<EntityPageNewPoolBatchMaterialization>,
								   std::span<NativeGraphPropertyPageReplacementMaterialization>,
								   std::span<NativeGraphPropertyPageBatchMaterialization>,
								   std::span<LargeValueMaterializationResult>) { graphPreparerCalled = true; };
	const NativeGraphDerivedStatePreparer derivedPreparer =
			[&derivedPreparerCalled](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {
				derivedPreparerCalled = true;
			};
	const ExistingPoolPagesRetirementPreparer noRetirementPreparer{};

	NativeGraphLogicalIdReservation nodeLease;
	nodeLease.node_ranges.push_back({1U, 1U});
	// Input-shape failure must precede the write guard: none of these invalid
	// requests may reserve IDs, publish a generation, or touch a typed store.
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, {}, LogSequenceNumber{}, graphPreparer, {}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, {}, LogSequenceNumber(2U), {}, {}, {}, {}, {}, {}, {}, {}, nodeLease));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, {}, LogSequenceNumber(2U), graphPreparer, {}, {}, {}, oneRetirement,
						noErasedLargeValueIds, noRetirementPreparer));
			},
			FormatError::FERR_INVALID_HEADER);

	NativeGraphDerivedStateMutation derived;
	NativeGraphIndexTreeMutation incompleteIndex;
	incompleteIndex.index_id = 77U;
	derived.index_tree_mutations.push_back(std::move(incompleteIndex));
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, derived, LogSequenceNumber(2U), graphPreparer, {}));
			},
			FormatError::FERR_INVALID_HEADER);

	NativeGraphDerivedStateMutation bootstrapDerived;
	bootstrapDerived.vector_bootstraps.push_back({});
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, bootstrapDerived, LogSequenceNumber(2U), graphPreparer, derivedPreparer));
			},
			FormatError::FERR_INVALID_HEADER);

	NativeGraphDerivedStateMutation deltaDerived;
	deltaDerived.vector_deltas.push_back({});
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, deltaDerived, LogSequenceNumber(2U), graphPreparer, derivedPreparer, {}, {}));
			},
			FormatError::FERR_INVALID_HEADER);

	EXPECT_FALSE(graphPreparerCalled);
	EXPECT_FALSE(derivedPreparerCalled);
	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
	unchanged.release();
}

TEST_F(RelocatableMetadataStoreTest, RejectsAnAuxiliaryLabelPageThatAttemptsToPublishAnEntityMapDelta) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	EntityPageStore entities(io, *store);
	NodeEntityRecord initialNode;
	initialNode.inline_label_ids = {3U, 5U, 7U};
	initialNode.inline_label_count = 3U;
	const EntityPageNewPoolRequest entityPool{
			SegmentId(31U), 901U, Generation(1U), PageId(101U), PhysicalSlotId(40U), PhysicalSlotId(40U)};
	const auto inserted = entities.allocateNode(entityPool, 1001U, initialNode, LogSequenceNumber(2U));
	ASSERT_TRUE(inserted.has_value());

	MetadataSnapshotPin beforeLabel = store->openSnapshot();
	const CatalogEntry pool = store->findCatalog(beforeLabel, SegmentId(31U));
	const PageOwnerHeader labelOwner{PageId(102U),
									 pool.owner_id,
									 PageOwnerKind::POK_ENTITY_LABEL,
									 0U,
									 RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
									 pool.generation,
									 LogSequenceNumber(3U)};
	const std::array<uint64_t, 1U> overflowLabels{{11U}};
	SlottedPage labelPage = SlottedPage::create(labelOwner);
	const SlottedSlotReference labelSlot = labelPage.insert(EntityLabelSetCodec::encode(overflowLabels));
	NodeEntityRecord labelledNode = initialNode;
	labelledNode.label_overflow = {labelSlot.page_id, labelSlot.slot_id, labelSlot.generation};
	const std::array<EntityPageBatchMutation, 1U> nodeMutation{{
			{.page_id = PageId(101U),
			 .record_kind = EntityPageRecordKind::EPRK_NODE,
			 .node_updates = {{1001U, labelledNode}}},
	}};
	EntityPageAuxiliaryMutationBatch auxiliary;
	auxiliary.appends.push_back({pool, PageId(102U), PageOwnerKind::POK_ENTITY_LABEL, labelPage.serialize(),
								 labelPage.stats().contiguous_free_bytes});
	auxiliary.node_update_bindings.push_back({1001U, inserted->location, labelledNode});
	static_cast<void>(entities.applyAuxiliaryEntityBatchTransaction(beforeLabel, nodeMutation, {}, {}, auxiliary,
																	LogSequenceNumber(3U)));
	beforeLabel.release();

	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock beforeSuperblock = before.superblock();
	const PageLocation labelLocation = store->findPageLocation(before, PageId(102U));
	EntityPageReplacementRequest forged;
	forged.expected_pool = pool;
	forged.expected_location = labelLocation;
	forged.owner_kind = PageOwnerKind::POK_ENTITY_LABEL;
	forged.inserted_nodes = {{1002U, PageId(102U), 0U, 1U}};
	const std::array<EntityPageReplacementRequest, 1U> requests{{forged}};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool materializerCalled = false;

	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						requests, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, {}, LogSequenceNumber(4U),
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
	EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
	EXPECT_EQ(entities.readNode(unchanged, 1001U), labelledNode);
	EXPECT_FALSE(store->findNodePageLocationOptional(unchanged, 1002U).has_value());
	EXPECT_EQ(store->findPageLocation(unchanged, PageId(102U)), labelLocation);
	unchanged.release();
	before.release();

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	EntityPageStore reopenedEntities(io, *reopened);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock(), beforeSuperblock);
	EXPECT_EQ(reopenedEntities.readNode(recovered, 1001U), labelledNode);
	EXPECT_FALSE(reopened->findNodePageLocationOptional(recovered, 1002U).has_value());
	EXPECT_EQ(reopened->findPageLocation(recovered, PageId(102U)), labelLocation);
	recovered.release();
}
