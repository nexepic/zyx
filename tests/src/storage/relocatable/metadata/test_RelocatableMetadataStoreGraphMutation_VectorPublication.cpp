/**
 * @file test_RelocatableMetadataStoreGraphMutation_VectorPublication.cpp
 * @brief Successful Vector delta publication through the native graph coordinator.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "storage/relocatable/metadata/RelocatableMetadataStoreValidationTestFixture.hpp"

TEST_F(RelocatableMetadataStoreValidationTest,
	   PublishesNativeGraphVectorDeltaWithMappingRootsRawPageAndManifestAtomically) {
	auto store = RelocatableMetadataStore::initializeNew(io, initialSuperblock());
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	constexpr uint64_t indexId = 47U;
	constexpr uint64_t reverseIndexId = 48U;
	constexpr uint64_t deltaIndexId = RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX | indexId;
	const std::array<uint64_t, 2U> bootstrapIndexIds{{indexId, reverseIndexId}};

	NativeGraphDerivedStateMutation bootstrap;
	for (size_t ordinal = 0U; ordinal < bootstrapIndexIds.size(); ++ordinal) {
		NativeGraphIndexTreeMutation mapping;
		mapping.index_id = bootstrapIndexIds[ordinal];
		mapping.new_pool_requests.push_back(
				{PhysicalSlotId(80U + ordinal), PhysicalSlotId(80U + ordinal), mapping.index_id, 1U});
		mapping.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
		bootstrap.index_tree_mutations.push_back(std::move(mapping));
	}
	bootstrap.vector_bootstraps.push_back(
			{indexId, reverseIndexId, 1U, 1U, 2U, VectorDistanceMetric::VDM_L2, 1U, PhysicalSlotId(82U),
			 PhysicalSlotId(83U)});
	SlottedPagePool indexPages(io, *store);
	BTreePageStore indexes(indexPages, *store);
	VectorPageStore vectors(io, *store);
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
			[&vectors, &manifests](std::vector<NativeGraphVectorBootstrapMaterialization> &plans) {
				materializeInitialVectorBootstrap(vectors, manifests, plans, LogSequenceNumber(2U));
			}));

	MetadataSnapshotPin before = store->openSnapshot();
	const IndexRootLocation nodeRoot = store->findIndexRoot(before, indexId);
	const IndexRootLocation reverseRoot = store->findIndexRoot(before, reverseIndexId);
	const VectorManifestLocation oldManifestLocation = store->findVectorManifest(before, indexId);
	const PageLocation oldManifestPage = store->findPageLocation(before, oldManifestLocation.manifest_page_id);
	const CatalogEntry oldManifestPool = store->findCatalog(before, oldManifestPage.segment_id);
	const std::array<std::byte, 1U> key{{std::byte{0x01}}};
	const BTreeRootLeafMutationPlan nodePlan = indexes.planRootLeafUpsert(before, indexId, key, 1U);
	const BTreeRootLeafMutationPlan reversePlan = indexes.planRootLeafUpsert(before, reverseIndexId, key, 1U);

	const auto replacementMutation = [](const BTreeRootLeafMutationPlan &plan) {
		NativeGraphIndexTreeMutation mutation;
		mutation.index_id = plan.expected_root.index_id;
		mutation.expected_root = plan.expected_root;
		mutation.pool_requests.push_back({plan.expected_pool.segment_id, {plan.expected_location}, 0U, {}});
		mutation.stable_replacement_root = plan.expected_location.page_id;
		return mutation;
	};
	NativeGraphDerivedStateMutation deltaState;
	deltaState.index_tree_mutations.push_back(replacementMutation(nodePlan));
	deltaState.index_tree_mutations.push_back(replacementMutation(reversePlan));
	NativeGraphIndexTreeMutation deltaMapping;
	deltaMapping.index_id = deltaIndexId;
	deltaMapping.new_pool_requests.push_back({PhysicalSlotId(84U), PhysicalSlotId(84U), deltaIndexId, 1U});
	deltaMapping.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
	deltaState.index_tree_mutations.push_back(std::move(deltaMapping));
	NativeGraphVectorDeltaMutation delta;
	delta.index_id = indexId;
	delta.vector_node_mapping_index_id = reverseIndexId;
	delta.delta_mapping_index_id = deltaIndexId;
	delta.expected_node_mapping_root = nodeRoot;
	delta.expected_vector_node_mapping_root = reverseRoot;
	delta.expected_manifest_pool = oldManifestPool;
	delta.expected_manifest_location = oldManifestPage;
	delta.expected_manifest = oldManifestLocation;
	delta.expected_manifest_owner_lsn = LogSequenceNumber(2U);
	delta.dimensions = 2U;
	delta.metric = VectorDistanceMetric::VDM_L2;
	delta.first_vector_id = 2U;
	delta.inserted_vector_count = 1U;
	delta.delta_entry_count = 1U;
	delta.raw_pages.push_back({NativeGraphVectorDeltaRawPlacement::NGVDRP_NEW_POOL, {}, {}, {}, 0U, 1U, 1U});
	delta.first_slot = PhysicalSlotId(85U);
	delta.last_slot = PhysicalSlotId(86U);
	deltaState.vector_deltas.push_back(std::move(delta));

	// The mapping and Manifest fences are current, so a gap in the caller's
	// logical-ID reservation must be rejected by the writer-side continuity
	// check before any new typed frame is materialized.
	NativeGraphDerivedStateMutation skippedVectorIdState = deltaState;
	skippedVectorIdState.vector_deltas.front().first_vector_id = 3U;
	bool skippedPagePreparerCalled = false;
	bool skippedIndexPreparerCalled = false;
	bool skippedVectorPreparerCalled = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, skippedVectorIdState, LogSequenceNumber(3U),
						[&](std::span<EntityPageReplacementMaterialization>,
							std::span<EntityPageAppendBatchMaterialization>,
							std::span<EntityPageNewPoolBatchMaterialization>,
							std::span<NativeGraphPropertyPageReplacementMaterialization>,
							std::span<NativeGraphPropertyPageBatchMaterialization>,
							std::span<LargeValueMaterializationResult>) { skippedPagePreparerCalled = true; },
						[&](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) { skippedIndexPreparerCalled = true; },
						{}, [&](std::vector<NativeGraphVectorDeltaMaterialization> &) { skippedVectorPreparerCalled = true; }));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(skippedPagePreparerCalled);
	EXPECT_FALSE(skippedIndexPreparerCalled);
	EXPECT_FALSE(skippedVectorPreparerCalled);
	EXPECT_EQ(store->openSnapshot().superblock().generation, Generation(2U));

	static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools, noLargeValues,
			deltaState, LogSequenceNumber(3U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[&indexes, &nodePlan, &reversePlan](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				ASSERT_EQ(batches.size(), 3U);
				indexes.prepareRootLeafMutation(nodePlan, batches[0].front().pool, nodePlan.expected_location,
						batches[0].front().replacement_locations.front(), LogSequenceNumber(3U));
				indexes.prepareRootLeafMutation(reversePlan, batches[1].front().pool, reversePlan.expected_location,
						batches[1].front().replacement_locations.front(), LogSequenceNumber(3U));
				IndexTreeMutationPoolBatch &deltaBatch = batches[2].front();
				const PageOwnerHeader owner{deltaBatch.appended_page_ids.front(), deltaIndexId, PageOwnerKind::POK_INDEX,
						0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, deltaBatch.pool.generation, LogSequenceNumber(3U)};
				BTreePageHeader header;
				header.index_id = deltaIndexId;
				header.page_id = owner.page_id;
				header.generation = deltaBatch.pool.generation;
				const std::array pages{BTreePage::create(owner, header)};
				indexes.prepareNewPoolInitialPages(deltaBatch.pool, pages, deltaBatch.appended_locations,
						LogSequenceNumber(3U));
			},
			{}, [&before, &vectors, &manifests](std::vector<NativeGraphVectorDeltaMaterialization> &plans) {
				ASSERT_EQ(plans.size(), 1U);
				NativeGraphVectorDeltaMaterialization &plan = plans.front();
				ASSERT_EQ(plan.raw_new_pools.size(), 1U);
				ASSERT_EQ(plan.raw_pages.size(), 1U);
				NativeGraphVectorDeltaRawPageMaterialization &raw = plan.raw_pages.front();
				const uint32_t recordBytes = sizeof(uint64_t) +
						static_cast<uint32_t>(plan.mutation.dimensions) * sizeof(uint16_t);
				VectorPageHeader header;
				header.index_id = plan.mutation.index_id;
				header.page_id = raw.location.page_id;
				header.generation = raw.pool.generation;
				header.kind = VectorStorageKind::VSK_RAW;
				header.record_bytes = recordBytes;
				header.slot_capacity = VectorPageStore::rawSlotCapacity(recordBytes);
				header.bitmap_bytes = static_cast<uint16_t>((static_cast<uint32_t>(header.slot_capacity) + 7U) / 8U);
				header.committed_lsn = LogSequenceNumber(3U);
				const PageOwnerHeader rawOwner{header.page_id, header.index_id, PageOwnerKind::POK_VECTOR_RAW, 0U,
						RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, header.generation, LogSequenceNumber(3U)};
				VectorPage rawPage = VectorPage::create(rawOwner, header);
				std::vector<std::byte> record(recordBytes);
				LittleEndianCodec::writeU64(record, 0U, plan.mutation.first_vector_id);
				const uint16_t slot = rawPage.insert(record);
				vectors.prepareRawNewPool(raw.pool, rawPage, LogSequenceNumber(3U), raw.location);
				plan.vector_locations = {{plan.mutation.first_vector_id, raw.location.page_id, slot,
									  rawPage.slotGeneration(slot)}};

				VectorManifest manifest = manifests.read(before, plan.mutation.index_id).manifest();
				manifest.committed_lsn = LogSequenceNumber(3U);
				manifest.delta_mapping_index_id = plan.mutation.delta_mapping_index_id;
				manifest.delta_mapping_root_page_id = plan.delta_mapping_root.root_page_id;
				manifest.delta_mapping_root_generation = plan.delta_mapping_root.root_generation;
				manifest.delta_entry_count = plan.mutation.delta_entry_count;
				manifest.raw_write_tail_page_id = raw.location.page_id;
				manifest.entries.push_back({raw.location.page_id, VectorStorageKind::VSK_RAW, 1U});
				const PageOwnerHeader manifestOwner{manifest.page_id, manifest.index_id, PageOwnerKind::POK_VECTOR_MANIFEST,
						0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, manifest.generation, LogSequenceNumber(3U)};
				const VectorManifestPage manifestPage = VectorManifestPage::create(manifestOwner, manifest);
				manifests.prepareReplacement(plan.mutation.expected_manifest_pool,
						plan.mutation.expected_manifest_location, plan.mutation.expected_manifest_owner_lsn, manifestPage,
						LogSequenceNumber(3U), plan.manifest_replacement);
			}));

	const MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_EQ(current.superblock().generation, Generation(3U));
	const VectorLocation publishedVector = store->findVectorLocation(current, 2U);
	EXPECT_EQ(publishedVector.vector_id, 2U);
	EXPECT_NE(publishedVector.page_id, PageId{});
	EXPECT_TRUE(store->findIndexRootOptional(current, deltaIndexId).has_value());
	const VectorManifestPage publishedManifest = manifests.read(current, indexId);
	EXPECT_EQ(publishedManifest.manifest().delta_mapping_index_id, deltaIndexId);
	EXPECT_EQ(publishedManifest.manifest().delta_entry_count, 1U);
	EXPECT_EQ(publishedManifest.manifest().raw_write_tail_page_id, publishedVector.page_id);
	EXPECT_FALSE(store->findVectorLocationOptional(before, 2U).has_value());
	EXPECT_EQ(manifests.read(before, indexId).manifest().delta_mapping_index_id, 0U);

	// Keep every durable mapping and Manifest fence current, then request an
	// erase for an ID that has never been published. This reaches the writer-side
	// reachability check instead of being rejected as a malformed or stale plan.
	const BTreeRootLeafMutationPlan currentNodePlan = indexes.planRootLeafUpsert(current, indexId, key, 1U);
	const BTreeRootLeafMutationPlan currentReversePlan = indexes.planRootLeafUpsert(current, reverseIndexId, key, 1U);
	const BTreeTreeRetirementPlan currentDeltaRetirement = indexes.planTreeRetirement(current, deltaIndexId, 4U);
	const VectorManifestLocation currentManifestLocation = store->findVectorManifest(current, indexId);
	const PageLocation currentManifestPage = store->findPageLocation(current, currentManifestLocation.manifest_page_id);
	const CatalogEntry currentManifestPool = store->findCatalog(current, currentManifestPage.segment_id);
	NativeGraphDerivedStateMutation unreachableEraseState;
	unreachableEraseState.index_tree_mutations.push_back(replacementMutation(currentNodePlan));
	unreachableEraseState.index_tree_mutations.push_back(replacementMutation(currentReversePlan));
	NativeGraphIndexTreeMutation retireDeltaRoot;
	retireDeltaRoot.index_id = deltaIndexId;
	retireDeltaRoot.expected_root = currentDeltaRetirement.expected_root;
	retireDeltaRoot.pool_requests = currentDeltaRetirement.pool_requests;
	retireDeltaRoot.retire_root = true;
	unreachableEraseState.index_tree_mutations.push_back(std::move(retireDeltaRoot));
	NativeGraphVectorDeltaMutation unreachableErase = deltaState.vector_deltas.front();
	unreachableErase.expected_node_mapping_root = currentNodePlan.expected_root;
	unreachableErase.expected_vector_node_mapping_root = currentReversePlan.expected_root;
	unreachableErase.expected_delta_mapping_index_id = deltaIndexId;
	unreachableErase.expected_delta_mapping_root = currentDeltaRetirement.expected_root;
	unreachableErase.delta_mapping_index_id = 0U;
	unreachableErase.expected_manifest_pool = currentManifestPool;
	unreachableErase.expected_manifest_location = currentManifestPage;
	unreachableErase.expected_manifest = currentManifestLocation;
	unreachableErase.expected_manifest_owner_lsn = LogSequenceNumber(3U);
	unreachableErase.first_vector_id = 0U;
	unreachableErase.inserted_vector_count = 0U;
	unreachableErase.delta_entry_count = 0U;
	unreachableErase.raw_pages.clear();
	unreachableErase.erased_vector_ids = {3U};
	unreachableEraseState.vector_deltas.push_back(std::move(unreachableErase));
	bool unreachablePagePreparerCalled = false;
	bool unreachableIndexPreparerCalled = false;
	bool unreachableVectorPreparerCalled = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, unreachableEraseState, LogSequenceNumber(4U),
						[&](std::span<EntityPageReplacementMaterialization>,
							std::span<EntityPageAppendBatchMaterialization>,
							std::span<EntityPageNewPoolBatchMaterialization>,
							std::span<NativeGraphPropertyPageReplacementMaterialization>,
							std::span<NativeGraphPropertyPageBatchMaterialization>,
							std::span<LargeValueMaterializationResult>) { unreachablePagePreparerCalled = true; },
						[&](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) { unreachableIndexPreparerCalled = true; },
						{},
						[&](std::vector<NativeGraphVectorDeltaMaterialization> &) {
							unreachableVectorPreparerCalled = true;
						}));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(unreachablePagePreparerCalled);
	EXPECT_FALSE(unreachableIndexPreparerCalled);
	EXPECT_FALSE(unreachableVectorPreparerCalled);
	EXPECT_EQ(store->openSnapshot().superblock().generation, Generation(3U));

	// The first publication makes every fence carried by deltaState stale. A
	// replay must fail before any callback can materialize another immutable page.
	bool pagePreparerCalled = false;
	bool indexPreparerCalled = false;
	bool vectorPreparerCalled = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, deltaState, LogSequenceNumber(4U),
						[&](std::span<EntityPageReplacementMaterialization>,
							std::span<EntityPageAppendBatchMaterialization>,
							std::span<EntityPageNewPoolBatchMaterialization>,
							std::span<NativeGraphPropertyPageReplacementMaterialization>,
							std::span<NativeGraphPropertyPageBatchMaterialization>,
							std::span<LargeValueMaterializationResult>) { pagePreparerCalled = true; },
						[&](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) { indexPreparerCalled = true; }, {},
						[&](std::vector<NativeGraphVectorDeltaMaterialization> &) { vectorPreparerCalled = true; }));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(pagePreparerCalled);
	EXPECT_FALSE(indexPreparerCalled);
	EXPECT_FALSE(vectorPreparerCalled);
	EXPECT_EQ(store->openSnapshot().superblock().generation, Generation(3U));

	bool bootstrapPreparerCalled = false;
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools,
						noLargeValues, bootstrap, LogSequenceNumber(4U),
						[&](std::span<EntityPageReplacementMaterialization>,
							std::span<EntityPageAppendBatchMaterialization>,
							std::span<EntityPageNewPoolBatchMaterialization>,
							std::span<NativeGraphPropertyPageReplacementMaterialization>,
							std::span<NativeGraphPropertyPageBatchMaterialization>,
							std::span<LargeValueMaterializationResult>) { pagePreparerCalled = true; },
						[&](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) { indexPreparerCalled = true; },
						[&](std::vector<NativeGraphVectorBootstrapMaterialization> &) { bootstrapPreparerCalled = true; }));
			},
			FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(pagePreparerCalled);
	EXPECT_FALSE(indexPreparerCalled);
	EXPECT_FALSE(bootstrapPreparerCalled);
	EXPECT_EQ(store->openSnapshot().superblock().generation, Generation(3U));

	// Retiring the final delta mapping is a normal publication when the referenced
	// vector is still reachable. The mapping-root retirement and Manifest update
	// must remain one atomic coordinator transaction.
	NativeGraphDerivedStateMutation retirementState;
	retirementState.index_tree_mutations.push_back(replacementMutation(currentNodePlan));
	retirementState.index_tree_mutations.push_back(replacementMutation(currentReversePlan));
	NativeGraphIndexTreeMutation successfulRetireDeltaRoot;
	successfulRetireDeltaRoot.index_id = deltaIndexId;
	successfulRetireDeltaRoot.expected_root = currentDeltaRetirement.expected_root;
	successfulRetireDeltaRoot.pool_requests = currentDeltaRetirement.pool_requests;
	successfulRetireDeltaRoot.retire_root = true;
	retirementState.index_tree_mutations.push_back(std::move(successfulRetireDeltaRoot));
	NativeGraphVectorDeltaMutation retirement = deltaState.vector_deltas.front();
	retirement.expected_node_mapping_root = currentNodePlan.expected_root;
	retirement.expected_vector_node_mapping_root = currentReversePlan.expected_root;
	retirement.expected_delta_mapping_index_id = deltaIndexId;
	retirement.expected_delta_mapping_root = currentDeltaRetirement.expected_root;
	retirement.delta_mapping_index_id = 0U;
	retirement.expected_manifest_pool = currentManifestPool;
	retirement.expected_manifest_location = currentManifestPage;
	retirement.expected_manifest = currentManifestLocation;
	retirement.expected_manifest_owner_lsn = LogSequenceNumber(3U);
	retirement.first_vector_id = 0U;
	retirement.inserted_vector_count = 0U;
	retirement.delta_entry_count = 0U;
	retirement.raw_pages.clear();
	retirement.erased_vector_ids = {publishedVector.vector_id};
	retirementState.vector_deltas.push_back(std::move(retirement));

	const Superblock retired = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools, noLargeValues,
			retirementState, LogSequenceNumber(4U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[&indexes, &currentNodePlan, &currentReversePlan, &currentDeltaRetirement](
					std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				ASSERT_EQ(batches.size(), 3U);
				indexes.prepareRootLeafMutation(currentNodePlan, batches[0].front().pool,
						currentNodePlan.expected_location, batches[0].front().replacement_locations.front(),
						LogSequenceNumber(4U));
				indexes.prepareRootLeafMutation(currentReversePlan, batches[1].front().pool,
						currentReversePlan.expected_location, batches[1].front().replacement_locations.front(),
						LogSequenceNumber(4U));
				currentDeltaRetirement.preparer(batches[2], LogSequenceNumber(4U));
			},
			{}, [&current, &manifests](std::vector<NativeGraphVectorDeltaMaterialization> &plans) {
				ASSERT_EQ(plans.size(), 1U);
				NativeGraphVectorDeltaMaterialization &plan = plans.front();
				EXPECT_TRUE(plan.raw_pages.empty());
				EXPECT_TRUE(plan.vector_locations.empty());
				VectorManifest manifest = manifests.read(current, plan.mutation.index_id).manifest();
				manifest.committed_lsn = LogSequenceNumber(4U);
				manifest.delta_mapping_index_id = 0U;
				manifest.delta_mapping_root_page_id = {};
				manifest.delta_mapping_root_generation = {};
				manifest.delta_entry_count = 0U;
				const PageOwnerHeader manifestOwner{manifest.page_id, manifest.index_id,
						PageOwnerKind::POK_VECTOR_MANIFEST, 0U, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
						manifest.generation, LogSequenceNumber(4U)};
				const VectorManifestPage manifestPage = VectorManifestPage::create(manifestOwner, manifest);
				manifests.prepareReplacement(plan.mutation.expected_manifest_pool,
						plan.mutation.expected_manifest_location, plan.mutation.expected_manifest_owner_lsn, manifestPage,
						LogSequenceNumber(4U), plan.manifest_replacement);
			});
	EXPECT_EQ(retired.generation, Generation(4U));
	MetadataSnapshotPin afterRetirement = store->openSnapshot();
	EXPECT_FALSE(store->findVectorLocationOptional(afterRetirement, publishedVector.vector_id).has_value());
	EXPECT_FALSE(store->findIndexRootOptional(afterRetirement, deltaIndexId).has_value());
	const VectorManifestPage retiredManifest = manifests.read(afterRetirement, indexId);
	EXPECT_EQ(retiredManifest.manifest().delta_mapping_index_id, 0U);
	EXPECT_EQ(retiredManifest.manifest().delta_entry_count, 0U);
	afterRetirement.release();
}
