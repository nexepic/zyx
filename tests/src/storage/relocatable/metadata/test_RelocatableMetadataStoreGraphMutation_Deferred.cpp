/**
 * @file test_RelocatableMetadataStoreGraphMutation_Deferred.cpp
 * @brief Deferred-planning and generation-fence tests for metadata publication.
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
#include "graph/storage/relocatable/entity/NativeStreamedGraphPublicationPlan.hpp"
#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "graph/storage/relocatable/index/NativeBulkRun.hpp"
TEST_F(RelocatableMetadataStoreTest, BuildsAndRewindsAnUnpublishedBulkRunInsideOneGraphCandidate) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	NativeGraphDerivedStateMutation derived;
	for (size_t ordinal = 0U; ordinal < 3U; ++ordinal) {
		NativeGraphPrivateSegmentRequest scratch;
		scratch.kind = SegmentKind::SGK_BULK_SCRATCH;
		scratch.owner_id = 75U;
		scratch.first_slot = PhysicalSlotId(40U);
		scratch.last_slot = PhysicalSlotId(42U);
		// The identity-aware image preparer produces the final payload only after
		// every fragment and manifest SegmentId is known.
		derived.private_segments.push_back(std::move(scratch));
	}
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	const std::vector<BTreePageEntry> expected = {
			{{std::byte{0x10}}, 11U}, {{std::byte{0x20}}, 22U}, {{std::byte{0x30}}, 33U}, {{std::byte{0x40}}, 44U}};

	static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {}, MetadataPublishOptions{},
			NativeGraphLogicalIdReservation{}, std::nullopt,
			[&expected](const NativeGraphPrivateSegmentSet &identities, size_t ordinal) -> std::vector<std::byte> {
				if (identities.size() != 3U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "bulk-run candidate image preparation lost a segment identity");
				}
				const auto location = [&identities](size_t sourceOrdinal) {
					return NativeBulkRunFragmentLocation{identities[sourceOrdinal].segment_id,
														 identities[sourceOrdinal].generation,
														 identities[sourceOrdinal].owner_id};
				};
				if (ordinal == 0U) {
					return NativeBulkRunFragmentCodec::encode({{expected[0U], expected[1U]}});
				}
				if (ordinal == 1U) {
					return NativeBulkRunFragmentCodec::encode({{expected[2U], expected[3U]}});
				}
				NativeBulkRunManifest manifest;
				manifest.fragments = {location(0U), location(1U)};
				manifest.entry_count = expected.size();
				manifest.encoded_bytes = NativeBulkRunFragmentCodec::encode({{expected[0U], expected[1U]}}).size() +
										 NativeBulkRunFragmentCodec::encode({{expected[2U], expected[3U]}}).size();
				return NativeBulkRunManifestCodec::encode(manifest);
			},
			[&expected](const NativeGraphPrivateSegmentSet &identities, const NativeGraphPrivateSegmentReader &reader) {
				ASSERT_EQ(identities.size(), 3U);
				const NativeBulkRunFragmentLocation manifestLocation{
						identities[2U].segment_id, identities[2U].generation, identities[2U].owner_id};
				const NativeBulkRunPayloadReader payloadReader =
						[&identities, &reader](const NativeBulkRunFragmentLocation &location) {
							const NativeGraphPrivateSegmentMaterialization candidate{
									location.segment_id, location.generation, location.transaction_id};
							try {
								static_cast<void>(identities.ordinalOf(candidate));
							} catch (const FormatException &) {
								throw FormatException(
										FormatError::FERR_INVALID_STATE,
										"bulk-run candidate manifest references a foreign scratch segment");
							}
							return reader(candidate);
						};
				const OrderedBTreeEntrySource source =
						NativeBulkRunStore::orderedSourceFromPayloadReader(manifestLocation, 2U, payloadReader);
				for (size_t pass = 0U; pass < 2U; ++pass) {
					OrderedBTreeEntryCursor cursor = source();
					std::vector<BTreePageEntry> actual;
					for (std::optional<BTreePageEntry> next = cursor(); next.has_value(); next = cursor()) {
						actual.push_back(std::move(*next));
					}
					EXPECT_EQ(actual, expected);
				}
			}));
}

TEST_F(RelocatableMetadataStoreTest,
       DeferredIndexPlannerPublishesARewindableScratchBuiltTreeAndRejectsStaleRetirementAtomically) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	NativeGraphDerivedStateMutation derived;
	NativeGraphPrivateSegmentRequest scratch;
	scratch.kind = SegmentKind::SGK_BULK_SCRATCH;
	scratch.owner_id = 76U;
	scratch.first_slot = PhysicalSlotId(40U);
	scratch.last_slot = PhysicalSlotId(42U);
	derived.private_segments.push_back(std::move(scratch));
	NativeGraphPrivateSegmentRequest manifestScratch;
	manifestScratch.kind = SegmentKind::SGK_BULK_SCRATCH;
	manifestScratch.owner_id = 76U;
	manifestScratch.first_slot = PhysicalSlotId(40U);
	manifestScratch.last_slot = PhysicalSlotId(42U);
	derived.private_segments.push_back(std::move(manifestScratch));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	SlottedPagePool pagePools(io, *store);
	BTreePageStore indexes(pagePools, *store);
	const std::vector<BTreePageEntry> expected = {
			{{std::byte{0x10}}, 0U}, {{std::byte{0x20}}, 0U}, {{std::byte{0x30}}, 0U}};
	bool ordinaryPreparerCalled = false;
	bool deferredPreparerCalled = false;
	const Superblock published = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[&ordinaryPreparerCalled](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				ordinaryPreparerCalled = true;
				EXPECT_TRUE(batches.empty());
			},
			{}, {}, {}, {}, {}, MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt,
			[&expected](const NativeGraphPrivateSegmentSet &identities, size_t ordinal) -> std::vector<std::byte> {
				if (identities.size() != 2U || ordinal > 1U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "deferred Index fixture lost its scratch identities");
				}
				if (ordinal == 0U) {
					return NativeBulkRunFragmentCodec::encode({expected});
				}
				const std::vector<std::byte> fragment = NativeBulkRunFragmentCodec::encode({expected});
				NativeBulkRunManifest manifest;
				manifest.fragments.push_back(
						{identities.front().segment_id, identities.front().generation, identities.front().owner_id});
				manifest.entry_count = expected.size();
				manifest.encoded_bytes = fragment.size();
				return NativeBulkRunManifestCodec::encode(manifest);
			},
			{},
			[&indexes, &store, &deferredPreparerCalled](const NativeGraphDeferredIndexPlanningContext &context) {
				const NativeGraphPrivateSegmentSet &identities = context.private_segments;
				const NativeGraphPrivateSegmentReader &reader = context.private_segment_reader;
				if (identities.size() != 2U) {
					throw FormatException(FormatError::FERR_INVALID_STATE,
										  "deferred Index planner lost its scratch identities");
				}
				const NativeBulkRunFragmentLocation location{identities.back().segment_id, identities.back().generation,
															 identities.back().owner_id};
				const NativeBulkRunPayloadReader payloadReader =
						[&identities, &reader](const NativeBulkRunFragmentLocation &requested) {
							const NativeGraphPrivateSegmentMaterialization identity{
									requested.segment_id, requested.generation, requested.transaction_id};
							try {
								static_cast<void>(identities.ordinalOf(identity));
							} catch (const FormatException &) {
								throw FormatException(FormatError::FERR_INVALID_STATE,
													  "deferred Index planner received a foreign scratch reference");
							}
							return reader(identity);
						};
				const OrderedBTreeEntrySource source =
						NativeBulkRunStore::orderedSourceFromPayloadReader(location, 1U, payloadReader);
				BulkBTreeBuildOptions options;
				options.first_slot = PhysicalSlotId(40U);
				options.last_slot = PhysicalSlotId(42U);
				options.pool_owner_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
				options.maximum_entries = 16U;
				options.maximum_pages = 4U;
				options.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
				const BTreeInitialTreeMutationPlan plan = BulkBTreeBuilder::planNativeTree(
						indexes, *store, context.current_snapshot, RSS_GLOBAL_ADJACENCY_INDEX_ID, source, options);
				NativeGraphDeferredIndexPlan result;
				result.mutation.index_id = plan.index_id;
				for (const BTreeNewIndexTreePoolMutation &pool: plan.new_pool_mutations) {
					result.mutation.new_pool_requests.push_back(
							{options.first_slot, options.last_slot, pool.owner_id, pool.appended_page_count});
				}
				result.mutation.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal,
													true};
				result.preparer = [preparer = plan.preparer,
								   &deferredPreparerCalled](std::vector<IndexTreeMutationPoolBatch> &batches) {
					deferredPreparerCalled = true;
					preparer(batches, LogSequenceNumber(2U));
				};
				return result;
			});
	EXPECT_TRUE(ordinaryPreparerCalled);
	EXPECT_TRUE(deferredPreparerCalled);
	EXPECT_EQ(published.generation, Generation(2U));
	MetadataSnapshotPin current = store->openSnapshot();
	const IndexRootLocation root = store->findIndexRoot(current, RSS_GLOBAL_ADJACENCY_INDEX_ID);
	const PageLocation rootPage = store->findPageLocation(current, root.root_page_id);
	const CatalogEntry rootPool = store->findCatalog(current, rootPage.segment_id);
	BTreeIndex index(RSS_GLOBAL_ADJACENCY_INDEX_ID, indexes, *store, BTreeLeafValueLayout::BLVL_EMPTY);
	EXPECT_EQ(index.scan(current, std::nullopt, expected.size()), expected);
	EXPECT_THROW(static_cast<void>(store->findCatalog(current, SegmentId(RSS_FIRST_ALLOCATABLE_SEGMENT_ID))),
				 FormatException);
	EXPECT_TRUE(root.root_page_id.isValid());
	current.release();

	NativeGraphDerivedStateMutation staleReplacement;
	NativeGraphPrivateSegmentRequest staleScratch;
	staleScratch.kind = SegmentKind::SGK_BULK_SCRATCH;
	staleScratch.owner_id = 79U;
	staleScratch.first_slot = PhysicalSlotId(43U);
	staleScratch.last_slot = PhysicalSlotId(43U);
	staleScratch.payload.assign(17U, std::byte{0x4F});
	staleReplacement.private_segments.push_back(std::move(staleScratch));
	MetadataSnapshotPin beforeFailure = store->openSnapshot();
	const Superblock beforeFailureSuperblock = beforeFailure.superblock();
	beforeFailure.release();
	bool pagePreparerCalled = false;
	bool indexPreparerCalled = false;
	bool plannerCalled = false;

	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, staleReplacement, LogSequenceNumber(3U),
						[&pagePreparerCalled](std::span<EntityPageReplacementMaterialization>,
											std::span<EntityPageAppendBatchMaterialization>,
											std::span<EntityPageNewPoolBatchMaterialization>,
											std::span<NativeGraphPropertyPageReplacementMaterialization>,
											std::span<NativeGraphPropertyPageBatchMaterialization>,
											std::span<LargeValueMaterializationResult>) { pagePreparerCalled = true; },
						[&indexPreparerCalled](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {
							indexPreparerCalled = true;
						},
						{}, {}, {}, {}, {}, MetadataPublishOptions{}, NativeGraphLogicalIdReservation{}, std::nullopt,
						{}, {}, [&plannerCalled, root, rootPage](const NativeGraphDeferredIndexPlanningContext &) {
							plannerCalled = true;
							NativeGraphDeferredIndexPlan plan;
							plan.mutation.index_id = RSS_GLOBAL_ADJACENCY_INDEX_ID;
							plan.mutation.expected_root = root;
							plan.mutation.new_pool_requests.push_back(
									{PhysicalSlotId(44U), PhysicalSlotId(44U), RSS_GLOBAL_ADJACENCY_INDEX_ID, 1U});
							plan.mutation.replacement_root = {0U, 0U, true};
							IndexTreeMutationPoolRequest sourcePool;
							sourcePool.segment_id = rootPage.segment_id;
							sourcePool.expected_retirements.push_back(
									{{rootPage.page_id,
									  rootPage.segment_id,
									  rootPage.page_index,
									  Generation(rootPage.generation.value() + 1U),
									  rootPage.free_bytes},
									 LogSequenceNumber(2U)});
							plan.mutation.pool_requests.push_back(std::move(sourcePool));
							plan.preparer = [](std::vector<IndexTreeMutationPoolBatch> &) {};
							return plan;
						}));
			},
			FormatError::FERR_INVALID_STATE);

	EXPECT_TRUE(plannerCalled);
	EXPECT_FALSE(pagePreparerCalled);
	EXPECT_FALSE(indexPreparerCalled);
	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), beforeFailureSuperblock);
	EXPECT_EQ(store->findIndexRoot(unchanged, RSS_GLOBAL_ADJACENCY_INDEX_ID), root);
	EXPECT_EQ(store->findPageLocation(unchanged, root.root_page_id), rootPage);
	EXPECT_TRUE(store->isPhysicalSlotAllocated(unchanged, rootPool.physical_slot));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(43U)));
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(44U)));
	EXPECT_THROW(static_cast<void>(store->findCatalog(
					 unchanged, SegmentId(beforeFailureSuperblock.next_logical_segment_id))),
				 FormatException);
	unchanged.release();

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock(), beforeFailureSuperblock);
	EXPECT_EQ(reopened->findIndexRoot(recovered, RSS_GLOBAL_ADJACENCY_INDEX_ID), root);
	EXPECT_EQ(reopened->findPageLocation(recovered, root.root_page_id), rootPage);
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(recovered, rootPool.physical_slot));
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(43U)));
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(44U)));
	recovered.release();
}

TEST_F(RelocatableMetadataStoreTest, DeferredIndexPlansPublishCatalogAndAdjacencyRootsInOneGeneration) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	NativeGraphDerivedStateMutation derived;
	NativeGraphPrivateSegmentRequest scratch;
	scratch.kind = SegmentKind::SGK_BULK_SCRATCH;
	scratch.owner_id = 77U;
	scratch.first_slot = PhysicalSlotId(40U);
	scratch.last_slot = PhysicalSlotId(45U);
	derived.private_segments.push_back(scratch);
	derived.private_segments.push_back(scratch);
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	SlottedPagePool pagePools(io, *store);
	BTreePageStore indexes(pagePools, *store);
	const std::vector<BTreePageEntry> catalogEntries = {{{std::byte{0x01}}, 1U}};
	const std::vector<BTreePageEntry> adjacencyEntries = {{{std::byte{0x02}}, 0U}};
	const Superblock published = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization>, std::span<EntityPageAppendBatchMaterialization>,
			   std::span<EntityPageNewPoolBatchMaterialization>,
			   std::span<NativeGraphPropertyPageReplacementMaterialization>,
			   std::span<NativeGraphPropertyPageBatchMaterialization>, std::span<LargeValueMaterializationResult>) {},
			[](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {}, {}, {}, {}, {}, {}, MetadataPublishOptions{},
			NativeGraphLogicalIdReservation{}, std::nullopt,
			[&](const NativeGraphPrivateSegmentSet &identities, size_t ordinal) {
				return NativeBulkRunFragmentCodec::encode({ordinal == 0U ? catalogEntries : adjacencyEntries});
			},
			{}, {}, {},
			[&](const NativeGraphDeferredIndexPlanningContext &context) {
				NativeGraphDeferredIndexPlans result;
				for (size_t ordinal = 0U; ordinal < context.private_segments.size(); ++ordinal) {
					const uint64_t indexId =
							ordinal == 0U ? RSS_SYSTEM_CATALOG_INDEX_ID : RSS_GLOBAL_ADJACENCY_INDEX_ID;
					const BTreeLeafValueLayout layout =
							ordinal == 0U ? BTreeLeafValueLayout::BLVL_U64 : BTreeLeafValueLayout::BLVL_EMPTY;
					const NativeBulkRunFragment fragment = NativeBulkRunFragmentCodec::decode(
							context.private_segment_reader(context.private_segments[ordinal]));
					const OrderedBTreeEntrySource source = [entries = fragment.entries] {
						return [entries, position = size_t{0U}]() mutable -> std::optional<BTreePageEntry> {
							return position == entries.size() ? std::nullopt
															  : std::optional<BTreePageEntry>(entries[position++]);
						};
					};
					BulkBTreeBuildOptions options;
					options.first_slot = PhysicalSlotId(40U);
					options.last_slot = PhysicalSlotId(45U);
					options.pool_owner_id = indexId;
					options.maximum_entries = 16U;
					options.maximum_pages = 4U;
					options.leaf_value_layout = layout;
					const BTreeInitialTreeMutationPlan tree = BulkBTreeBuilder::planNativeTree(
							indexes, *store, context.current_snapshot, indexId, source, options);
					NativeGraphDeferredIndexPlan plan;
					plan.mutation.index_id = indexId;
					for (const BTreeNewIndexTreePoolMutation &pool: tree.new_pool_mutations) {
						plan.mutation.new_pool_requests.push_back(
								{options.first_slot, options.last_slot, pool.owner_id, pool.appended_page_count});
					}
					plan.mutation.replacement_root = {tree.root_page.pool_ordinal, tree.root_page.appended_page_ordinal,
													  true};
					plan.preparer = [preparer = tree.preparer](std::vector<IndexTreeMutationPoolBatch> &batches) {
						preparer(batches, LogSequenceNumber(2U));
					};
					result.plans.push_back(std::move(plan));
				}
				return result;
			});
	EXPECT_EQ(published.generation, Generation(2U));
	MetadataSnapshotPin current = store->openSnapshot();
	EXPECT_TRUE(store->findIndexRootOptional(current, RSS_SYSTEM_CATALOG_INDEX_ID).has_value());
	EXPECT_TRUE(store->findIndexRootOptional(current, RSS_GLOBAL_ADJACENCY_INDEX_ID).has_value());
	current.release();
}

TEST_F(RelocatableMetadataStoreTest,
	   RejectsStaleGraphMutationGenerationBeforeCallingAnyTypedMaterializerOrReservingSlots) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));

	// Advance the durable generation after a caller has conceptually planned its
	// derived-index work. The plan itself is valid; only its pinned generation is stale.
	static_cast<void>(store->reserveNativeGraphLogicalIds(1U, 0U));
	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock publishedBefore = before.superblock();
	ASSERT_EQ(publishedBefore.generation, Generation(2U));
	before.release();

	NativeGraphDerivedStateMutation derived;
	NativeGraphIndexTreeMutation index;
	index.index_id = 77U;
	index.new_pool_requests.push_back({PhysicalSlotId(40U), PhysicalSlotId(40U), index.index_id, 1U});
	index.replacement_root = NativeGraphIndexTreeAppendedPageReference{0U, 0U, true};
	derived.index_tree_mutations.push_back(std::move(index));

	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool graphMaterializerCalled = false;
	bool indexMaterializerCalled = false;

	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, derived, LogSequenceNumber(3U),
						[&graphMaterializerCalled](std::span<EntityPageReplacementMaterialization>,
												   std::span<EntityPageAppendBatchMaterialization>,
												   std::span<EntityPageNewPoolBatchMaterialization>,
												   std::span<NativeGraphPropertyPageReplacementMaterialization>,
												   std::span<NativeGraphPropertyPageBatchMaterialization>,
												   std::span<LargeValueMaterializationResult>) {
							graphMaterializerCalled = true;
						},
						[&indexMaterializerCalled](std::vector<std::vector<IndexTreeMutationPoolBatch>> &) {
							indexMaterializerCalled = true;
						},
						{}, {}, {}, {}, {}, {}, {}, Generation(1U)));
			},
			FormatError::FERR_INVALID_STATE);

	EXPECT_FALSE(graphMaterializerCalled);
	EXPECT_FALSE(indexMaterializerCalled);

	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), publishedBefore);
	EXPECT_FALSE(store->findIndexRootOptional(unchanged, 77U).has_value());
	EXPECT_FALSE(store->isPhysicalSlotAllocated(unchanged, PhysicalSlotId(40U)));
	unchanged.release();

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock(), publishedBefore);
	EXPECT_FALSE(reopened->findIndexRootOptional(recovered, 77U).has_value());
	EXPECT_FALSE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	recovered.release();
}

TEST_F(RelocatableMetadataStoreTest, RejectsAnAbandonedLogicalIdLeaseAtTheMetadataPublicationBoundary) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const NativeGraphLogicalIdReservation abandoned = store->acquireNativeGraphLogicalIds(1U, 0U, 1U);
	store->abandonNativeGraphLogicalIdReservation(abandoned);
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

	// The durable high-water mark intentionally remains advanced after a rollback,
	// but the released in-memory lease must not authorize a direct metadata call.
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, {}, LogSequenceNumber(3U),
						[&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
											   std::span<EntityPageAppendBatchMaterialization>,
											   std::span<EntityPageNewPoolBatchMaterialization>,
											   std::span<NativeGraphPropertyPageReplacementMaterialization>,
											   std::span<NativeGraphPropertyPageBatchMaterialization>,
											   std::span<LargeValueMaterializationResult>) {
							graphPreparerCalled = true;
						},
						{}, {}, {}, {}, {}, {}, {}, abandoned));
			},
			FormatError::FERR_INVALID_STATE);

	EXPECT_FALSE(graphPreparerCalled);
	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), beforeSuperblock);
	unchanged.release();

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock(), beforeSuperblock);
	recovered.release();
}

TEST_F(RelocatableMetadataStoreTest, BurnsAClaimedLogicalIdLeaseWhenTheWriterGenerationFenceRejectsPublication) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	const NativeGraphLogicalIdReservation reservation = store->acquireNativeGraphLogicalIds(1U, 0U, 1U);
	ASSERT_EQ(reservation.node_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{1U, 1U}}));

	MetadataSnapshotPin before = store->openSnapshot();
	const Superblock publishedBefore = before.superblock();
	ASSERT_EQ(publishedBefore.generation, Generation(2U));
	before.release();

	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyBatches{};
	const std::array<NativeGraphLargeValueMutation, 0U> noLargeValues{};
	bool graphPreparerCalled = false;

	// The lease is claimed before the writer to preserve lock ordering. Once it
	// becomes visible to a caller, a failed commit must burn it rather than make
	// the same logical ID replayable against a newer metadata generation.
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, {}, LogSequenceNumber(3U),
						[&graphPreparerCalled](std::span<EntityPageReplacementMaterialization>,
											   std::span<EntityPageAppendBatchMaterialization>,
											   std::span<EntityPageNewPoolBatchMaterialization>,
											   std::span<NativeGraphPropertyPageReplacementMaterialization>,
											   std::span<NativeGraphPropertyPageBatchMaterialization>,
											   std::span<LargeValueMaterializationResult>) {
							graphPreparerCalled = true;
						},
						{}, {}, {}, {}, {}, {}, {}, reservation, Generation(1U)));
			},
			FormatError::FERR_INVALID_STATE);

	EXPECT_FALSE(graphPreparerCalled);
	MetadataSnapshotPin unchanged = store->openSnapshot();
	EXPECT_EQ(unchanged.superblock(), publishedBefore);
	unchanged.release();

	// Replaying the same process-local capability is forbidden even though the
	// durable high-water mark still contains its logical ID.
	expectFormatError(
			[&] {
				static_cast<void>(store->mutateNativeGraphPagesWithLargeValues(
						noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
						noLargeValues, {}, LogSequenceNumber(3U),
						[](std::span<EntityPageReplacementMaterialization>,
						   std::span<EntityPageAppendBatchMaterialization>,
						   std::span<EntityPageNewPoolBatchMaterialization>,
						   std::span<NativeGraphPropertyPageReplacementMaterialization>,
						   std::span<NativeGraphPropertyPageBatchMaterialization>,
						   std::span<LargeValueMaterializationResult>) {},
						{}, {}, {}, {}, {}, {}, {}, reservation));
			},
			FormatError::FERR_INVALID_STATE);

	const NativeGraphLogicalIdReservation replacement = store->acquireNativeGraphLogicalIds(1U, 0U, 1U);
	EXPECT_EQ(replacement.node_ranges, (std::vector<NativeGraphLogicalIdLeaseRange>{{2U, 1U}}));
	store->abandonNativeGraphLogicalIdReservation(replacement);
}

TEST_F(RelocatableMetadataStoreTest, PublishesFreshDerivedIndexRootAtomicallyAndRecoversItAfterReopen) {
	auto store = RelocatableMetadataStore::initializeNew(io, superblock(1U, 1U));
	NativeGraphDerivedStateMutation derived;
	NativeGraphIndexTreeMutation index;
	index.index_id = 77U;
	index.new_pool_requests.push_back({PhysicalSlotId(40U), PhysicalSlotId(40U), index.index_id, 1U});
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
	bool graphMaterializerCalled = false;
	bool indexMaterializerCalled = false;

	const Superblock published = store->mutateNativeGraphPagesWithLargeValues(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyBatches,
			noLargeValues, derived, LogSequenceNumber(2U),
			[&graphMaterializerCalled](std::span<EntityPageReplacementMaterialization>,
									   std::span<EntityPageAppendBatchMaterialization>,
									   std::span<EntityPageNewPoolBatchMaterialization>,
									   std::span<NativeGraphPropertyPageReplacementMaterialization>,
									   std::span<NativeGraphPropertyPageBatchMaterialization>,
									   std::span<LargeValueMaterializationResult>) { graphMaterializerCalled = true; },
			[&indexes, &indexMaterializerCalled](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				indexMaterializerCalled = true;
				ASSERT_EQ(batches.size(), 1U);
				ASSERT_EQ(batches.front().size(), 1U);
				IndexTreeMutationPoolBatch &batch = batches.front().front();
				ASSERT_TRUE(batch.newly_reserved);
				ASSERT_EQ(batch.appended_page_ids.size(), 1U);
				ASSERT_EQ(batch.appended_locations.size(), 1U);
				const PageOwnerHeader owner{batch.appended_page_ids.front(),
											77U,
											PageOwnerKind::POK_INDEX,
											0U,
											RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES,
											batch.pool.generation,
											LogSequenceNumber(2U)};
				BTreePageHeader header;
				header.index_id = 77U;
				header.page_id = batch.appended_page_ids.front();
				header.generation = batch.pool.generation;
				const std::array<BTreePage, 1U> pages{{BTreePage::create(owner, header)}};
				indexes.prepareNewPoolInitialPages(batch.pool, pages, batch.appended_locations, LogSequenceNumber(2U));
			});

	EXPECT_TRUE(graphMaterializerCalled);
	EXPECT_TRUE(indexMaterializerCalled);
	EXPECT_EQ(published.generation, Generation(2U));

	MetadataSnapshotPin current = store->openSnapshot();
	const IndexRootLocation root = store->findIndexRoot(current, 77U);
	EXPECT_EQ(root.index_id, 77U);
	EXPECT_EQ(root.root_generation, Generation(1U));
	const PageLocation rootPage = store->findPageLocation(current, root.root_page_id);
	EXPECT_EQ(rootPage.segment_id, store->findCatalog(current, rootPage.segment_id).segment_id);
	EXPECT_TRUE(store->isPhysicalSlotAllocated(current, PhysicalSlotId(40U)));
	current.release();

	store.reset();
	auto reopened = RelocatableMetadataStore::open(io);
	MetadataSnapshotPin recovered = reopened->openSnapshot();
	EXPECT_EQ(recovered.superblock(), published);
	EXPECT_EQ(reopened->findIndexRoot(recovered, 77U), root);
	EXPECT_EQ(reopened->findPageLocation(recovered, root.root_page_id), rootPage);
	EXPECT_TRUE(reopened->isPhysicalSlotAllocated(recovered, PhysicalSlotId(40U)));
	recovered.release();
}
