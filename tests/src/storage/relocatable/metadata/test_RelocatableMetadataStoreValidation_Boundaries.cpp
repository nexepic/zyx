/**
 * @file test_RelocatableMetadataStoreValidation_Boundaries.cpp
 * @brief Validation coverage for relocatable Metadata Store public operations.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreValidationTestFixture.hpp"

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsLargeValuePreparerIdentityChangesWithoutPublishingTheGraphGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::vector<std::byte> emptyBytes;
	const NativeGraphLargeValueMutation mutation{
			{LargeValueId(1U), Generation(1U), 0U, graph::utils::Sha256::digest(emptyBytes),
			 LargeValueKind::LVK_BYTES, LargeValueCodec::LVC_RAW, "application/octet-stream"},
			emptyBytes,
			PhysicalSlotId(40U),
			PhysicalSlotId(41U),
			PhysicalSlotId(42U),
			PhysicalSlotId(43U),
			{},
			LargeValueExtentClass::LVEC_32K,
			0U,
			1U};
	const std::array<NativeGraphLargeValueMutation, 1U> largeValues{{mutation}};
	NativeGraphDerivedStateMutation contentIndexState;
	contentIndexState.index_tree_mutations.push_back({
			.index_id = RSS_LARGE_VALUE_CONTENT_INDEX_ID,
			.new_pool_requests = {{PhysicalSlotId(44U), PhysicalSlotId(44U), RSS_LARGE_VALUE_CONTENT_INDEX_ID, 1U}},
			.replacement_root = {{0U, 0U, true}},
	});
	const NativeGraphDerivedStatePreparer contentIndexPreparer =
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {};

	const auto expectRejected = [&](const NativeGraphPageTransactionWithLargeValuesPreparer &preparer) {
		expectFormatError(
				[&] {
					static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
							noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
							noPropertyPools, largeValues, contentIndexState, LogSequenceNumber(2U), preparer,
							contentIndexPreparer));
				},
				FormatError::FERR_INVALID_STATE);
		MetadataSnapshotPin unchanged = store->openSnapshot();
		EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
		EXPECT_EQ(unchanged.superblock().next_large_value_id, 1U);
		EXPECT_FALSE(store->findLargeValueManifestOptional(unchanged, LargeValueId(1U)).has_value());
		for (const uint64_t slot: {40U, 41U, 42U, 43U}) {
			EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(slot)));
		}
		unchanged.release();
	};

	expectRejected([](std::span<EntityPageReplacementMaterialization>,
						  std::span<EntityPageAppendBatchMaterialization>,
						  std::span<EntityPageNewPoolBatchMaterialization>,
						  std::span<NativeGraphPropertyPageReplacementMaterialization>,
						  std::span<NativeGraphPropertyPageBatchMaterialization>,
						  std::span<LargeValueMaterializationResult> materializations) {
		ASSERT_EQ(materializations.size(), 1U);
		// Pool identity is coordinator-owned; a typed writer cannot retarget it.
		++materializations.front().manifest_pools.front().pool.owner_id;
	});
	expectRejected([](std::span<EntityPageReplacementMaterialization>,
						  std::span<EntityPageAppendBatchMaterialization>,
						  std::span<EntityPageNewPoolBatchMaterialization>,
						  std::span<NativeGraphPropertyPageReplacementMaterialization>,
						  std::span<NativeGraphPropertyPageBatchMaterialization>,
						  std::span<LargeValueMaterializationResult> materializations) {
		ASSERT_EQ(materializations.size(), 1U);
		// The manifest root must remain the coordinator's assigned first frame.
		materializations.front().manifest_location.page_index = RSS_PAGE_POOL_FIRST_DATA_FRAME + 1U;
	});
	// A metadata-shaped callback that does not materialize its assigned frame
	// must fail before it can publish a dangling Manifest route.
	EXPECT_THROW(
		static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
					noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
					largeValues, contentIndexState, LogSequenceNumber(2U),
					[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
					   std::span<EntityPageNewPoolBatchMaterialization>,
					   std::span<NativeGraphPropertyPageReplacementMaterialization>,
					   std::span<NativeGraphPropertyPageBatchMaterialization>,
					   std::span<LargeValueMaterializationResult>) {},
					contentIndexPreparer)),
			FormatException);
	MetadataSnapshotPin afterMissingMaterialization = store->openSnapshot();
	EXPECT_EQ(afterMissingMaterialization.superblock().generation, Generation(1U));
	EXPECT_FALSE(store->findLargeValueManifestOptional(afterMissingMaterialization, LargeValueId(1U)).has_value());
	afterMissingMaterialization.release();

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock().generation, Generation(1U));
	EXPECT_EQ(recovered.superblock().next_large_value_id, 1U);
	EXPECT_FALSE(reopened->findLargeValueManifestOptional(recovered, LargeValueId(1U)).has_value());
	for (const uint64_t slot: {40U, 41U, 42U, 43U}) {
		EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(slot)));
	}
	recovered.release();
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsAnAlreadyPublishedLargeValueBeforeCallingEitherMaterializer) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	LargeValueStore values(io, *store);
	LargeValueWriteOptions options;
	options.first_data_slot = PhysicalSlotId(40U);
	options.last_data_slot = PhysicalSlotId(80U);
	options.first_manifest_slot = PhysicalSlotId(41U);
	options.last_manifest_slot = PhysicalSlotId(90U);
	options.kind = LargeValueKind::LVK_BYTES;
	options.media_type = "application/octet-stream";
	const std::vector<std::byte> payload{std::byte{0x7AU}};
	const LargeValueWriteResult written = values.write(payload, options, LogSequenceNumber(2U));

	MetadataSnapshotPin snapshot = store->openSnapshot();
	const IndexRootLocation contentRoot =
			store->findIndexRoot(snapshot, RSS_LARGE_VALUE_CONTENT_INDEX_ID);
	const PageLocation contentRootLocation = store->findPageLocation(snapshot, contentRoot.root_page_id);
	const Generation beforeGeneration = snapshot.superblock().generation;
	snapshot.release();

	NativeGraphDerivedStateMutation derived;
	NativeGraphIndexTreeMutation contentIndex;
	contentIndex.index_id = RSS_LARGE_VALUE_CONTENT_INDEX_ID;
	contentIndex.expected_root = contentRoot;
	contentIndex.pool_requests.push_back({contentRootLocation.segment_id, {contentRootLocation}, 0U, {}});
	derived.index_tree_mutations.push_back(std::move(contentIndex));
	const NativeGraphLargeValueMutation duplicate{
			written.reference,
			payload,
			PhysicalSlotId(91U),
			PhysicalSlotId(91U),
			PhysicalSlotId(92U),
			PhysicalSlotId(92U),
			{},
			LargeValueExtentClass::LVEC_16K,
			1U,
			1U};
	const std::array<NativeGraphLargeValueMutation, 1U> duplicates{{duplicate}};
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	bool graphPreparerCalled = false;
	bool indexPreparerCalled = false;

	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
						noPropertyBatches, duplicates, derived, LogSequenceNumber(3U),
						[&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
												  std::span<EntityPageAppendBatchMaterialization>,
												  std::span<EntityPageNewPoolBatchMaterialization>,
												  std::span<NativeGraphPropertyPageReplacementMaterialization>,
												  std::span<NativeGraphPropertyPageBatchMaterialization>,
												  std::span<LargeValueMaterializationResult>) { graphPreparerCalled = true; },
						[&indexPreparerCalled](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {
							indexPreparerCalled = true;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(graphPreparerCalled);
	EXPECT_FALSE(indexPreparerCalled);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock().generation, beforeGeneration);
	EXPECT_EQ(values.read(unchanged, written.reference), payload);
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(91U)));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(92U)));
}

TEST_F(RelocatableMetadataStoreValidationTest,
	   RejectsMultipleLargeValuesWhenTheFirstWouldExhaustTheLogicalIdSpace) {
	Superblock exhausted = initialSuperblock();
	exhausted.next_large_value_id = std::numeric_limits<uint64_t>::max();
	auto store = RelocatableMetadataStore::initializeNew(io, exhausted);
	const std::vector<std::byte> emptyBytes;
	const LargeValueRef exhaustedReference{
			LargeValueId(std::numeric_limits<uint64_t>::max()), Generation(1U), 0U,
			graph::utils::Sha256::digest(emptyBytes), LargeValueKind::LVK_BYTES, LargeValueCodec::LVC_RAW,
			"application/octet-stream"};
	const NativeGraphLargeValueMutation mutation{
			exhaustedReference,
			emptyBytes,
			PhysicalSlotId(40U),
			PhysicalSlotId(40U),
			PhysicalSlotId(41U),
			PhysicalSlotId(41U),
			{},
			LargeValueExtentClass::LVEC_16K,
			0U,
			1U};
	const std::array<NativeGraphLargeValueMutation, 2U> mutations{{mutation, mutation}};
	NativeGraphDerivedStateMutation derived;
	derived.index_tree_mutations.push_back({
			.index_id = RSS_LARGE_VALUE_CONTENT_INDEX_ID,
			.new_pool_requests = {{PhysicalSlotId(42U), PhysicalSlotId(42U),
									  RSS_LARGE_VALUE_CONTENT_INDEX_ID, 1U}},
			.replacement_root = {{0U, 0U, true}},
	});
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	bool graphPreparerCalled = false;
	bool indexPreparerCalled = false;

	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements,
						noPropertyBatches, mutations, derived, LogSequenceNumber(2U),
						[&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
												  std::span<EntityPageAppendBatchMaterialization>,
												  std::span<EntityPageNewPoolBatchMaterialization>,
												  std::span<NativeGraphPropertyPageReplacementMaterialization>,
												  std::span<NativeGraphPropertyPageBatchMaterialization>,
												  std::span<LargeValueMaterializationResult>) { graphPreparerCalled = true; },
						[&indexPreparerCalled](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {
							indexPreparerCalled = true;
						}));
			},
			FormatError::FERR_ARITHMETIC_OVERFLOW);
	EXPECT_FALSE(graphPreparerCalled);
	EXPECT_FALSE(indexPreparerCalled);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock().generation, Generation(1U));
	EXPECT_EQ(unchanged.superblock().next_large_value_id, std::numeric_limits<uint64_t>::max());
	EXPECT_FALSE(store->findLargeValueManifestOptional(unchanged, exhaustedReference.value_id).has_value());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(41U)));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(42U)));
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

TEST_F(RelocatableMetadataStoreValidationTest, ValidatesStatePoolMaterializationBeforePublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const StatePagePoolPreparer validPreparer =
			[](const CatalogEntry &pool, PageId pageId, PageLocation &page, StateLocation &state) {
				page = {pageId, pool.segment_id, RSS_PAGE_POOL_FIRST_DATA_FRAME, pool.generation, 0U};
				state = {state.state_id, pageId, 0U, 1U};
			};

	expectFormatError(
			[&] {
				static_cast<void>(store->materializeStatePagePool(
						0U, 0U, PhysicalSlotId{}, PhysicalSlotId{}, LogSequenceNumber{}, {}));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store->materializeStatePagePool(
						17U, 19U, PhysicalSlotId(41U), PhysicalSlotId(40U), LogSequenceNumber(2U), validPreparer));
			},
			FormatError::FERR_INVALID_HEADER);

	// A typed preparer cannot alter the writer-owned page or State identity.
	expectFormatError(
			[&] {
				static_cast<void>(store->materializeStatePagePool(
						17U, 19U, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(2U),
						[](const CatalogEntry &, PageId, PageLocation &page, StateLocation &) {
							page.page_index = 0U;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin afterRejectedPreparer = store->openSnapshot();
	EXPECT_FALSE(store->findStateLocationOptional(afterRejectedPreparer, 19U).has_value());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(afterRejectedPreparer, PhysicalSlotId(40U)));
	afterRejectedPreparer.release();

	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(41U), true, LogSequenceNumber(3U)));
	const std::optional<StatePagePoolMaterialization> unavailable = store->materializeStatePagePool(
			17U, 20U, PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(4U), validPreparer);
	EXPECT_FALSE(unavailable.has_value());
	static_cast<void>(store->setPhysicalSlotAllocated(PhysicalSlotId(41U), false, LogSequenceNumber(5U)));

	const std::optional<StatePagePoolMaterialization> published = store->materializeStatePagePool(
			17U, 19U, PhysicalSlotId(40U), PhysicalSlotId(40U), LogSequenceNumber(6U), validPreparer);
	ASSERT_TRUE(published.has_value());
	EXPECT_EQ(published->state.state_id, 19U);
	MetadataSnapshotPin snapshot = store->openSnapshot();
	std::vector<StateLocation> visited;
	store->visitStateLocations(snapshot, [&visited](const StateLocation &state) { visited.push_back(state); });
	ASSERT_EQ(visited.size(), 1U);
	EXPECT_EQ(visited.front(), published->state);
	std::vector<StateLocation> resumed;
	size_t visitedMetadataPages = 0U;
	EXPECT_FALSE(store->visitStateLocationsAfter(
			snapshot, 0U, 2U,
			[&resumed](const StateLocation &state) {
				resumed.push_back(state);
				return false;
			},
			&visitedMetadataPages));
	EXPECT_EQ(resumed, (std::vector<StateLocation>{published->state}));
	EXPECT_EQ(visitedMetadataPages, 2U);

	resumed.clear();
	visitedMetadataPages = 0U;
	EXPECT_TRUE(store->visitStateLocationsAfter(
			snapshot, published->state.state_id, 2U,
			[&resumed](const StateLocation &state) {
				resumed.push_back(state);
				return true;
			},
			&visitedMetadataPages));
	EXPECT_TRUE(resumed.empty());
	EXPECT_EQ(visitedMetadataPages, 2U);

	const std::vector<CatalogEntry> statePools = store->findCurrentStatePagePools(snapshot, 17U);
	ASSERT_EQ(statePools.size(), 1U);
	EXPECT_EQ(statePools.front().segment_id, published->pool.segment_id);
	EXPECT_EQ(statePools.front().physical_slot, published->pool.physical_slot);
	EXPECT_EQ(statePools.front().kind, SegmentKind::SGK_STATE_PAGE_POOL);
	EXPECT_EQ(statePools.front().owner_id, 17U);
	EXPECT_EQ(statePools.front().generation, published->pool.generation);
	EXPECT_EQ(statePools.front().state, SegmentState::SGS_SEALED);
	EXPECT_EQ(statePools.front().committed_lsn, published->pool.committed_lsn);
	EXPECT_EQ(store->findPageLocation(snapshot, published->page.page_id), published->page);
	EXPECT_EQ(store->findPageLocationOptional(snapshot, published->page.page_id), published->page);
	std::vector<PageLocation> pageLocations;
	EXPECT_FALSE(store->visitPageLocationsUntil(snapshot, [&pageLocations](const PageLocation &page) {
		pageLocations.push_back(page);
		return false;
	}));
	EXPECT_EQ(pageLocations, (std::vector<PageLocation>{published->page}));
	EXPECT_EQ(store->findPageLocationsForSegment(snapshot, published->page.segment_id), pageLocations);
	EXPECT_TRUE(store->findPageLocationsForSegment(snapshot, SegmentId(99U)).empty());
	snapshot.release();
	expectFormatError(
			[&] {
				static_cast<void>(store->materializeStatePagePool(
						17U, 19U, PhysicalSlotId(41U), PhysicalSlotId(41U), LogSequenceNumber(7U), validPreparer));
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

TEST_F(RelocatableMetadataStoreValidationTest, RejectsReleasedSnapshotPinsAcrossPublicDirectoryBoundaries) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	MetadataSnapshotPin released = store->openSnapshot();
	released.release();
	const auto expectRejected = [](const auto &operation) {
		expectFormatError([&operation] { operation(); }, FormatError::FERR_INVALID_STATE);
	};

	// A released pin must not be reusable as a read capability. Each public
	// directory entry point rejects it before examining caller-supplied IDs.
	expectRejected([&] { static_cast<void>(store->findCatalog(released, SegmentId(1U))); });
	expectRejected([&] { static_cast<void>(store->findRetireQueueEntryOptional(released, PhysicalSlotId(1U))); });
	expectRejected([&] { store->visitRetireQueueEntries(released, [](const RetireQueueEntry &) {}); });
	expectRejected([&] { store->visitCatalogEntries(released, [](const CatalogEntry &) {}); });
	expectRejected([&] { static_cast<void>(store->findCurrentIndexPagePools(released)); });
	expectRejected([&] { static_cast<void>(store->findCurrentEntityPagePools(released)); });
	expectRejected([&] { static_cast<void>(store->findCurrentVectorAdjacencyPagePools(released)); });
	expectRejected([&] { static_cast<void>(store->findCurrentVectorRawPagePools(released)); });
	expectRejected([&] { static_cast<void>(store->findRetiredVectorRawPagePools(released)); });
	expectRejected([&] { static_cast<void>(store->findCurrentVectorPqPagePools(released)); });
	expectRejected([&] { static_cast<void>(store->findCurrentLargeValueDataPagePools(released)); });
	expectRejected([&] { static_cast<void>(store->findCurrentLargeValueManifestPagePools(released)); });
	expectRejected([&] { static_cast<void>(store->findCurrentPropertyPagePools(released, 1U)); });
	expectRejected([&] { static_cast<void>(store->findCurrentStatePagePools(released, 1U)); });

	expectRejected([&] { static_cast<void>(store->findPageLocation(released, PageId(1U))); });
	expectRejected([&] { static_cast<void>(store->findPageLocationOptional(released, PageId(1U))); });
	expectRejected([&] { static_cast<void>(store->findExtentLocation(released, ExtentId(1U))); });
	expectRejected([&] { static_cast<void>(store->findExtentLocationOptional(released, ExtentId(1U))); });
	expectRejected([&] { static_cast<void>(store->findNodePageLocation(released, 1U)); });
	expectRejected([&] { static_cast<void>(store->findEdgePageLocation(released, 1U)); });
	expectRejected([&] { static_cast<void>(store->findNodePageLocationOptional(released, 1U)); });
	expectRejected([&] { static_cast<void>(store->findEdgePageLocationOptional(released, 1U)); });
	expectRejected([&] { store->visitNodePageLocations(released, [](const EntityPageLocation &) {}); });
	expectRejected([&] { store->visitEdgePageLocations(released, [](const EntityPageLocation &) {}); });
	expectRejected([&] { static_cast<void>(store->visitNodePageLocationsUntil(released, [](const EntityPageLocation &) {
		return true;
	})); });
	expectRejected([&] { static_cast<void>(store->visitEdgePageLocationsUntil(released, [](const EntityPageLocation &) {
		return true;
	})); });
	expectRejected([&] { static_cast<void>(store->visitNodePageLocationsAfter(
			released, 0U, 1U, [](const EntityPageLocation &) { return true; })); });
	expectRejected([&] { static_cast<void>(store->visitPageLocationsUntil(released, [](const PageLocation &) { return true; })); });
	expectRejected([&] { store->visitEntityPropertyLocations(released, [](const EntityPropertyLocation &) {}); });
	expectRejected([&] { static_cast<void>(store->visitEntityPropertyLocationsAfter(
			released, 0U, 1U, [](const EntityPropertyLocation &) { return true; })); });
	expectRejected([&] { store->visitStateLocations(released, [](const StateLocation &) {}); });
	expectRejected([&] { static_cast<void>(store->visitStateLocationsAfter(
			released, 0U, 1U, [](const StateLocation &) { return true; })); });
	expectRejected([&] { store->visitExtentLocations(released, [](const ExtentLocation &) {}); });
	expectRejected([&] { store->visitIndexRoots(released, [](const IndexRootLocation &) {}); });
	expectRejected([&] { store->visitVectorLocations(released, [](const VectorLocation &) {}); });
	expectRejected([&] { store->visitVectorManifestLocations(released, [](const VectorManifestLocation &) {}); });
	expectRejected([&] { static_cast<void>(store->visitVectorManifestLocationsAfter(
			released, 0U, 1U, [](const VectorManifestLocation &) { return true; })); });
	expectRejected([&] { store->visitLargeValueManifestLocations(released, [](const LargeValueManifestLocation &) {}); });
	expectRejected([&] { static_cast<void>(store->visitLargeValueManifestLocationsAfter(
			released, 0U, 1U, [](const LargeValueManifestLocation &) { return true; })); });

	expectRejected([&] { static_cast<void>(store->findEntityPropertyLocation(released, {})); });
	expectRejected([&] { static_cast<void>(store->findEntityPropertyLocationOptional(released, {})); });
	expectRejected([&] { static_cast<void>(store->findStateLocation(released, 1U)); });
	expectRejected([&] { static_cast<void>(store->findStateLocationOptional(released, 1U)); });
	expectRejected([&] { static_cast<void>(store->findVectorLocation(released, 1U)); });
	expectRejected([&] { static_cast<void>(store->findVectorLocationOptional(released, 1U)); });
	expectRejected([&] { static_cast<void>(store->findVectorManifest(released, 1U)); });
	expectRejected([&] { static_cast<void>(store->findVectorManifestOptional(released, 1U)); });
	expectRejected([&] { static_cast<void>(store->findLargeValueManifest(released, LargeValueId(1U))); });
	expectRejected([&] { static_cast<void>(store->findLargeValueManifestOptional(released, LargeValueId(1U))); });
	expectRejected([&] { static_cast<void>(store->findIndexRoot(released, 1U)); });
	expectRejected([&] { static_cast<void>(store->findIndexRootOptional(released, 1U)); });
	expectRejected([&] { static_cast<void>(store->findPageLocationsForSegment(released, SegmentId(1U))); });
	expectRejected([&] { static_cast<void>(store->isPhysicalSlotAllocated(released, PhysicalSlotId(1U))); });

	MetadataSnapshotPin active = store->openSnapshot();
	std::function<void(const CatalogEntry &)> noCatalogVisitor;
	std::function<bool(const EntityPageLocation &)> noEntityVisitor;
	EXPECT_THROW(store->visitCatalogEntries(active, noCatalogVisitor), FormatException);
	EXPECT_THROW(static_cast<void>(store->visitNodePageLocationsUntil(active, noEntityVisitor)), FormatException);
	// Traversal callbacks are required read capabilities. Rejecting an empty
	// callback before consulting the directory keeps every facade fail-closed.
	std::function<void(const RetireQueueEntry &)> noRetireQueueVisitor;
	std::function<void(const EntityPageLocation &)> noEntityLocationVisitor;
	std::function<bool(const PageLocation &)> noPageLocationVisitor;
	std::function<void(const EntityPropertyLocation &)> noPropertyLocationVisitor;
	std::function<bool(const EntityPropertyLocation &)> noResumablePropertyLocationVisitor;
	std::function<void(const StateLocation &)> noStateLocationVisitor;
	std::function<bool(const StateLocation &)> noResumableStateLocationVisitor;
	std::function<void(const ExtentLocation &)> noExtentLocationVisitor;
	std::function<void(const IndexRootLocation &)> noIndexRootVisitor;
	std::function<void(const VectorLocation &)> noVectorLocationVisitor;
	std::function<void(const VectorManifestLocation &)> noVectorManifestVisitor;
	std::function<bool(const VectorManifestLocation &)> noResumableVectorManifestVisitor;
	std::function<void(const LargeValueManifestLocation &)> noLargeValueManifestVisitor;
	std::function<bool(const LargeValueManifestLocation &)> noResumableLargeValueManifestVisitor;
	const auto expectInvalidVisitor = [](const auto &operation) {
		expectFormatError(operation, FormatError::FERR_INVALID_STATE);
	};
	expectInvalidVisitor([&] { store->visitRetireQueueEntries(active, noRetireQueueVisitor); });
	expectInvalidVisitor([&] { store->visitNodePageLocations(active, noEntityLocationVisitor); });
	expectInvalidVisitor([&] { store->visitEdgePageLocations(active, noEntityLocationVisitor); });
	expectInvalidVisitor([&] { static_cast<void>(store->visitEdgePageLocationsUntil(active, noEntityVisitor)); });
	expectInvalidVisitor([&] { static_cast<void>(store->visitPageLocationsUntil(active, noPageLocationVisitor)); });
	expectInvalidVisitor([&] { store->visitEntityPropertyLocations(active, noPropertyLocationVisitor); });
	expectInvalidVisitor([&] {
		static_cast<void>(store->visitEntityPropertyLocationsAfter(active, 0U, 1U, noResumablePropertyLocationVisitor));
	});
	expectInvalidVisitor([&] { store->visitStateLocations(active, noStateLocationVisitor); });
	expectInvalidVisitor([&] {
		static_cast<void>(store->visitStateLocationsAfter(active, 0U, 1U, noResumableStateLocationVisitor));
	});
	expectInvalidVisitor([&] { store->visitExtentLocations(active, noExtentLocationVisitor); });
	expectInvalidVisitor([&] { store->visitIndexRoots(active, noIndexRootVisitor); });
	expectInvalidVisitor([&] { store->visitVectorLocations(active, noVectorLocationVisitor); });
	expectInvalidVisitor([&] { store->visitVectorManifestLocations(active, noVectorManifestVisitor); });
	expectInvalidVisitor([&] {
		static_cast<void>(store->visitVectorManifestLocationsAfter(active, 0U, 1U, noResumableVectorManifestVisitor));
	});
	expectInvalidVisitor([&] { store->visitLargeValueManifestLocations(active, noLargeValueManifestVisitor); });
	expectInvalidVisitor([&] {
		static_cast<void>(store->visitLargeValueManifestLocationsAfter(
				active, 0U, 1U, noResumableLargeValueManifestVisitor));
	});
	EXPECT_THROW(static_cast<void>(store->visitNodePageLocationsAfter(active, 0U, 0U,
			[](const EntityPageLocation &) { return true; })), FormatException);
	EXPECT_THROW(static_cast<void>(store->findEntityPropertyLocation(active, {})), FormatException);
	EXPECT_THROW(static_cast<void>(store->findEntityPropertyLocationOptional(active, {})), FormatException);
	EXPECT_THROW(static_cast<void>(store->findPageLocationsForSegment(active, SegmentId{})), FormatException);
	active.release();
}
