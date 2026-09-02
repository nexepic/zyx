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
}
