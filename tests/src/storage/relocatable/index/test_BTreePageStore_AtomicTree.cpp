/**
 * @file test_BTreePageStore_AtomicTree.cpp
 * @brief Atomic materialization and recovery tests for relocatable B+Tree index pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "tests/src/storage/relocatable/index/RelocatableBTreePageStoreTestUtils.hpp"

using namespace graph::storage;
using namespace relocatable_btree_test;

TEST_F(RelocatableBTreePageStoreTest, AtomicallyMaterializesNewTreePagesAndSwitchesTheIndexRoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation oldRoot{17, PageId(101), Generation(1)};
	static_cast<void>(store.publishRoots(std::span(&oldRoot, 1), {}, LogSequenceNumber(3)));
	MetadataSnapshotPin before = metadata->openSnapshot();
	static_cast<void>(
			store.materializeAndPublishTree(request.segment_id, 17, 2, 0, LogSequenceNumber(4),
											[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
												return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4));
											}));
	EXPECT_EQ(metadata->findIndexRoot(before, 17), oldRoot);
	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation newRoot = metadata->findIndexRoot(current, 17);
	EXPECT_EQ(newRoot.root_page_id, PageId(102));
	EXPECT_EQ(newRoot.root_generation, Generation(1));
	BTreeIndex index(17, store, *metadata);
	EXPECT_EQ(index.find(current, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1));
	EXPECT_EQ(metadata->findPageLocation(current, PageId(102)).segment_id, request.segment_id);
	EXPECT_EQ(metadata->findPageLocation(current, PageId(103)).segment_id, request.segment_id);
	before.release();
	current.release();

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(reopened, 17), newRoot);
	EXPECT_EQ(reopenedIndex.find(reopened, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1));
	reopened.release();
	EXPECT_EQ(reopenedMetadata->allocateLogicalPageId(LogSequenceNumber(5)), PageId(104));
}

TEST_F(RelocatableBTreePageStoreTest, DoesNotPublishAnIncompleteTreeFactoryBatch) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation oldRoot{17, PageId(101), Generation(1)};
	static_cast<void>(store.publishRoots(std::span(&oldRoot, 1), {}, LogSequenceNumber(3)));
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTree(
						request.segment_id, 17, 2, 0, LogSequenceNumber(4),
						[](const CatalogEntry &, std::span<const PageId>) { return std::vector<BTreePage>{}; }));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin afterFactoryFailure = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(afterFactoryFailure, 17), oldRoot);
	afterFactoryFailure.release();
	EXPECT_EQ(metadata->allocateLogicalPageId(LogSequenceNumber(4)), PageId(102));
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInvalidTreeRequestsAndFactoryPageIdentities) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTree(
						SegmentId{}, 17, 1, 0, LogSequenceNumber(2),
						[](const CatalogEntry &, std::span<const PageId>) { return std::vector<BTreePage>{}; }));
			},
			FormatError::FERR_INVALID_HEADER);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation oldRoot{17, PageId(101), Generation(1)};
	static_cast<void>(store.publishRoots(std::span(&oldRoot, 1), {}, LogSequenceNumber(3)));
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTree(
						request.segment_id, 17, 2, 0, LogSequenceNumber(4),
						[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
							return twoPageTree(poolEntry, pageIds, LogSequenceNumber(3));
						}));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(metadata->findIndexRoot(snapshot, 17), oldRoot);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsOversizedBatchesAndNonIndexPoolsBeforeMaterialization) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTree(
						request.segment_id, 17, RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U, 0, LogSequenceNumber(3),
						[](const CatalogEntry &, std::span<const PageId>) { return std::vector<BTreePage>{}; }));
			},
			FormatError::FERR_INVALID_HEADER);

	request.segment_id = SegmentId(72);
	request.pool_kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	request.page_id = PageId(102);
	request.page_owner_id = 99;
	request.page_owner_kind = PageOwnerKind::POK_PROPERTY;
	ASSERT_TRUE(
			pool.allocateEmptyPage(request, PhysicalSlotId(41), PhysicalSlotId(41), LogSequenceNumber(3)).has_value());
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTree(
						request.segment_id, 17, 1, 0, LogSequenceNumber(4),
						[](const CatalogEntry &, std::span<const PageId>) { return std::vector<BTreePage>{}; }));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsAtomicTreeBatchWhenThePoolLacksEnoughFrames) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest first = allocationRequest();
	ASSERT_TRUE(store.allocate(first, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	for (uint64_t page = 102; page <= 131; ++page) {
		const LogSequenceNumber lsn(page - 99U);
		SlottedPagePoolAppendRequest request;
		request.segment_id = first.segment_id;
		request.generation = first.generation;
		request.page_id = PageId(page);
		request.page_owner_id = first.page_owner_id;
		request.page_owner_kind = PageOwnerKind::POK_INDEX;
		PageOwnerHeader owner = pageOwner(lsn);
		owner.page_id = request.page_id;
		BTreePageHeader header;
		header.index_id = owner.owner_id;
		header.page_id = owner.page_id;
		header.generation = owner.generation;
		static_cast<void>(store.append(request, BTreePage::create(owner, header), lsn));
	}
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTree(
						first.segment_id, 17, 2, 0, LogSequenceNumber(33),
						[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
							return twoPageTree(poolEntry, pageIds, LogSequenceNumber(33));
						}));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, RecoversAnInterruptedAtomicTreePublicationFromTheWal) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
							   LogSequenceNumber(2))
						.has_value());
	const IndexRootLocation oldRoot{17, PageId(101), Generation(1)};
	static_cast<void>(store.publishRoots(std::span(&oldRoot, 1), {}, LogSequenceNumber(3)));
	MetadataPublishOptions options;
	options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
	EXPECT_THROW(static_cast<void>(store.materializeAndPublishTree(
						 request.segment_id, 17, 2, 0, LogSequenceNumber(4),
						 [](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
							 return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4));
						 },
						 options)),
				 BootstrapPublishInterrupted);

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	BTreeIndex reopenedIndex(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(reopened, 17).root_page_id, PageId(102));
	EXPECT_EQ(reopenedIndex.find(reopened, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1));
}

TEST_F(RelocatableBTreePageStoreTest, DiscardsTreeFramesWhenTheWalPublicationHeaderWasNotDurable) {
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pool(io, *metadata);
		BTreePageStore store(pool, *metadata);
		const SlottedPagePoolAllocationRequest request = allocationRequest();
		ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2)), PhysicalSlotId(40), PhysicalSlotId(40),
								   LogSequenceNumber(2))
							.has_value());
		const IndexRootLocation oldRoot{17, PageId(101), Generation(1)};
		static_cast<void>(store.publishRoots(std::span(&oldRoot, 1), {}, LogSequenceNumber(3)));
		MetadataPublishOptions options;
		options.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_BEFORE_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(store.materializeAndPublishTree(
							 request.segment_id, 17, 2, 0, LogSequenceNumber(4),
							 [](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
								 return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4));
							 },
							 options)),
					 BootstrapPublishInterrupted);
	}

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPool(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPool, *reopenedMetadata);
	MetadataSnapshotPin old = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(old, 17).root_page_id, PageId(101));
	old.release();
	static_cast<void>(
			reopenedStore.materializeAndPublishTree(allocationRequest().segment_id, 17, 2, 0, LogSequenceNumber(4),
													[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
														return twoPageTree(poolEntry, pageIds, LogSequenceNumber(4));
													}));
	BTreeIndex index(17, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin current = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedMetadata->findIndexRoot(current, 17).root_page_id, PageId(102));
	EXPECT_EQ(index.find(current, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1));
}

TEST_F(RelocatableBTreePageStoreTest, BindsInitialLeafValuesAfterTheMetadataBatchAssignsPageIds) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	MetadataSnapshotPin before = metadata->openSnapshot();
	const std::array<BTreePageEntry, 2U> entries = {BTreePageEntry{{std::byte{'a'}}, 0U},
													BTreePageEntry{{std::byte{'b'}}, 0U}};
	bool binderCalled = false;
	std::vector<PageId> assignedPages;
	const BTreeInitialTreeMutationPlan plan = store.planInitialTreeMutation(
			before, 17U, entries,
			[&](std::span<BTreePageEntry> boundEntries, const BTreeInitialTreeValueBindingContext &context) {
				if (boundEntries.size() != entries.size() || context.pool_batches.empty() || context.page_ids.empty() ||
					context.page_ids.size() != context.generations.size()) {
					throw std::runtime_error("test initial-tree binder did not receive a complete metadata assignment");
				}
				binderCalled = true;
				assignedPages.assign(context.page_ids.begin(), context.page_ids.end());
				boundEntries[0].value_or_child = context.page_ids.front().value();
				boundEntries[1].value_or_child = context.page_ids.back().value() + 1U;
			});

	NativeGraphIndexTreeMutation treeMutation;
	treeMutation.index_id = plan.index_id;
	for (const BTreeNewIndexTreePoolMutation &newPool: plan.new_pool_mutations) {
		treeMutation.new_pool_requests.push_back(
				{PhysicalSlotId(40U), PhysicalSlotId(40U), newPool.owner_id, newPool.appended_page_count});
	}
	treeMutation.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
	NativeGraphDerivedStateMutation derived;
	derived.index_tree_mutations.push_back(std::move(treeMutation));
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};
	static_cast<void>(metadata->mutateNativeGraphPages(
			noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools, derived,
			LogSequenceNumber(2U),
			[](std::span<EntityPageReplacementMaterialization> replacements,
			   std::span<EntityPageAppendBatchMaterialization> appends,
			   std::span<EntityPageNewPoolBatchMaterialization> newPools,
			   std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
			   std::span<NativeGraphPropertyPageBatchMaterialization> newPropertyPools) {
				if (!replacements.empty() || !appends.empty() || !newPools.empty() || !properties.empty() ||
					!newPropertyPools.empty()) {
					throw std::runtime_error("test initial-tree publication unexpectedly materialized graph pages");
				}
			},
			[&plan](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
				if (batches.size() != 1U) {
					throw std::runtime_error("test initial-tree publication has an invalid Index batch count");
				}
				plan.preparer(batches.front(), LogSequenceNumber(2U));
			}));

	EXPECT_TRUE(binderCalled);
	ASSERT_FALSE(assignedPages.empty());
	EXPECT_FALSE(metadata->findIndexRootOptional(before, 17U).has_value());
	MetadataSnapshotPin after = metadata->openSnapshot();
	BTreeIndex index(17U, store, *metadata);
	EXPECT_EQ(index.find(after, std::array<std::byte, 1U>{std::byte{'a'}}),
			  std::optional<uint64_t>(assignedPages.front().value()));
	EXPECT_EQ(index.find(after, std::array<std::byte, 1U>{std::byte{'b'}}),
			  std::optional<uint64_t>(assignedPages.back().value() + 1U));
	after.release();
	before.release();
}

TEST_F(RelocatableBTreePageStoreTest, PublishesAndReplacesKeyOnlyFreshTreesThroughTheNativeTransaction) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const std::array<EntityPageReplacementRequest, 0U> noEntityReplacements{};
	const std::array<EntityPageAppendBatchRequest, 0U> noEntityAppends{};
	const std::array<EntityPageNewPoolBatchRequest, 0U> noEntityPools{};
	const std::array<NativeGraphPropertyPageReplacementRequest, 0U> noPropertyReplacements{};
	const std::array<NativeGraphPropertyPageBatchRequest, 0U> noPropertyPools{};

	const auto publish = [&](const BTreeInitialTreeMutationPlan &plan,
							std::optional<IndexRootLocation> expectedRoot, PhysicalSlotId slot,
							LogSequenceNumber checkpointLsn) {
		ASSERT_EQ(plan.new_pool_mutations.size(), 1U);
		NativeGraphIndexTreeMutation treeMutation;
		treeMutation.index_id = plan.index_id;
		treeMutation.expected_root = expectedRoot;
		for (const BTreeNewIndexTreePoolMutation &newPool: plan.new_pool_mutations) {
			treeMutation.new_pool_requests.push_back(
					{slot, slot, newPool.owner_id, newPool.appended_page_count});
		}
		treeMutation.replacement_root = {plan.root_page.pool_ordinal, plan.root_page.appended_page_ordinal, true};
		NativeGraphDerivedStateMutation derived;
		derived.index_tree_mutations.push_back(std::move(treeMutation));
		static_cast<void>(metadata->mutateNativeGraphPages(
				noEntityReplacements, noEntityAppends, noEntityPools, noPropertyReplacements, noPropertyPools, derived,
				checkpointLsn,
				[](std::span<EntityPageReplacementMaterialization> replacements,
				   std::span<EntityPageAppendBatchMaterialization> appends,
				   std::span<EntityPageNewPoolBatchMaterialization> newPools,
				   std::span<NativeGraphPropertyPageReplacementMaterialization> properties,
				   std::span<NativeGraphPropertyPageBatchMaterialization> newPropertyPools) {
					if (!replacements.empty() || !appends.empty() || !newPools.empty() || !properties.empty() ||
						!newPropertyPools.empty()) {
						throw std::runtime_error("test key-only tree publication unexpectedly materialized graph pages");
					}
				},
				[&plan, checkpointLsn](std::vector<std::vector<IndexTreeMutationPoolBatch>> &batches) {
					if (batches.size() != 1U) {
						throw std::runtime_error("test key-only tree publication has an invalid Index batch count");
					}
					plan.preparer(batches.front(), checkpointLsn);
				}));
	};

	const std::array<BTreePageEntry, 0U> noEntries{};
	MetadataSnapshotPin beforeCreate = metadata->openSnapshot();
	const BTreeInitialTreeMutationPlan emptyPlan =
			store.planInitialTreeMutation(beforeCreate, 17U, noEntries, {}, BTreeLeafValueLayout::BLVL_EMPTY);
	publish(emptyPlan, std::nullopt, PhysicalSlotId(40U), LogSequenceNumber(2U));
	EXPECT_FALSE(metadata->findIndexRootOptional(beforeCreate, 17U).has_value());
	beforeCreate.release();

	MetadataSnapshotPin afterCreate = metadata->openSnapshot();
	const IndexRootLocation emptyRoot = metadata->findIndexRoot(afterCreate, 17U);
	BTreeIndex index(17U, store, *metadata, BTreeLeafValueLayout::BLVL_EMPTY);
	EXPECT_TRUE(index.scan(afterCreate, std::nullopt, 1U).empty());
	EXPECT_EQ(store.read(afterCreate, emptyRoot.root_page_id).header().leaf_value_layout,
			  BTreeLeafValueLayout::BLVL_EMPTY);
	const std::array<BTreePageEntry, 2U> entries = {BTreePageEntry{{std::byte{'a'}}, 0U},
														 BTreePageEntry{{std::byte{'b'}}, 0U}};
	const BTreeInitialTreeMutationPlan replacementPlan =
			store.planReplacementTreeMutation(afterCreate, 17U, entries, {}, BTreeLeafValueLayout::BLVL_EMPTY);
	publish(replacementPlan, emptyRoot, PhysicalSlotId(41U), LogSequenceNumber(3U));
	EXPECT_EQ(metadata->findIndexRoot(afterCreate, 17U), emptyRoot);
	afterCreate.release();

	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_NE(metadata->findIndexRoot(current, 17U), emptyRoot);
	EXPECT_EQ(index.scan(current, std::nullopt, entries.size()),
			  std::vector<BTreePageEntry>(entries.begin(), entries.end()));
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInitialValueBindingThatChangesAKeyBeforeWritingFrames) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::array<BTreePageEntry, 1U> entries = {BTreePageEntry{{std::byte{'a'}}, 0U}};
	const BTreeInitialTreeMutationPlan plan = store.planInitialTreeMutation(
			snapshot, 17U, entries,
			[](std::span<BTreePageEntry> boundEntries, const BTreeInitialTreeValueBindingContext &) {
				boundEntries.front().key = {std::byte{'b'}};
				boundEntries.front().value_or_child = 1U;
			});
	IndexTreeMutationPoolBatch batch;
	batch.pool = {SegmentId(71U),			PhysicalSlotId(40U),  SegmentKind::SGK_INDEX_PAGE_POOL, 17U, Generation(1U),
				  SegmentState::SGS_SEALED, LogSequenceNumber(2U)};
	batch.newly_reserved = true;
	batch.appended_page_ids = {PageId(101U)};
	batch.appended_locations = {{PageId(101U), batch.pool.segment_id, 0U, batch.pool.generation, 0U}};
	std::vector<std::vector<IndexTreeMutationPoolBatch>> allBatches;
	allBatches.push_back({batch});
	expectFormatError([&] { plan.preparer(allBatches.front(), LogSequenceNumber(2U)); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, 17U).has_value());
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsFreshTreeStagingAndTamperedPreparationBeforeWritingPages) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::array<BTreePageEntry, 1U> valueEntry = {BTreePageEntry{{std::byte{'a'}}, 0U}};
	const std::array<BTreePageEntry, 1U> keyOnlyEntry = {BTreePageEntry{{std::byte{'a'}}, 0U}};
	const size_t stagedInputLimit =
			static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT) * RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES;
	const std::vector<std::byte> oversizedKey(stagedInputLimit, std::byte{'x'});
	const std::array<BTreePageEntry, 1U> oversizedEntry = {BTreePageEntry{oversizedKey, 1U}};

	expectFormatError([&] { static_cast<void>(store.planInitialTreeMutation(snapshot, 17U, oversizedEntry)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.planInitialTreeMutation(
						snapshot, 17U, keyOnlyEntry,
						[](std::span<BTreePageEntry>, const BTreeInitialTreeValueBindingContext &) {},
						BTreeLeafValueLayout::BLVL_EMPTY));
			},
			FormatError::FERR_INVALID_HEADER);

	const BTreeInitialTreeMutationPlan plan = store.planInitialTreeMutation(
			snapshot, 17U, valueEntry,
			[](std::span<BTreePageEntry> boundEntries, const BTreeInitialTreeValueBindingContext &) {
				boundEntries.front().value_or_child = 0U;
			});
	IndexTreeMutationPoolBatch batch;
	batch.pool = {SegmentId(71U), PhysicalSlotId(40U), SegmentKind::SGK_INDEX_PAGE_POOL, 17U, Generation(1U),
				  SegmentState::SGS_SEALED, LogSequenceNumber(2U)};
	batch.newly_reserved = true;
	batch.appended_page_ids = {PageId(101U)};
	batch.appended_locations = {{PageId(101U), batch.pool.segment_id, 0U, batch.pool.generation, 0U}};
	const auto prepareSingleBatch = [&plan](IndexTreeMutationPoolBatch candidate, LogSequenceNumber checkpointLsn) {
		std::vector<IndexTreeMutationPoolBatch> batches;
		batches.push_back(std::move(candidate));
		plan.preparer(batches, checkpointLsn);
	};

	expectFormatError([&] { prepareSingleBatch(batch, LogSequenceNumber{}); }, FormatError::FERR_INVALID_STATE);
	IndexTreeMutationPoolBatch wrongKind = batch;
	wrongKind.pool.kind = SegmentKind::SGK_PROPERTY_PAGE_POOL;
	expectFormatError([&] { prepareSingleBatch(wrongKind, LogSequenceNumber(2U)); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { prepareSingleBatch(batch, LogSequenceNumber(2U)); }, FormatError::FERR_INVALID_STATE);
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, 17U).has_value());
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInvalidRetirementAndFreshTreePlanningContracts) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const std::array<BTreePageEntry, 1U> resolvedEntries = {BTreePageEntry{{std::byte{'a'}}, 1U}};
	const std::array<BTreePageEntry, 1U> unresolvedEntries = {BTreePageEntry{{std::byte{'a'}}, 0U}};

	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, 0U, 1U)); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, 17U, 0U)); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(store.planInitialTreeMutation(snapshot, 0U, resolvedEntries)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(store.planInitialTreeMutation(
						snapshot, 17U, resolvedEntries, {}, static_cast<BTreeLeafValueLayout>(0xffU)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.planInitialTreeMutation(snapshot, 17U, unresolvedEntries)); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(store.planReplacementTreeMutation(snapshot, 17U, resolvedEntries)); },
				  FormatError::FERR_INVALID_STATE);
	snapshot.release();
	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, 17U, 1U)); },
				  FormatError::FERR_INVALID_HEADER);
}

TEST_F(RelocatableBTreePageStoreTest, RejectsRootLeafWithPersistedSiblingLinkBeforeErasing) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation root{request.page_owner_id, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry currentPool = metadata->findCatalog(snapshot, request.segment_id);
	BTreePageHeader malformedHeader;
	malformedHeader.index_id = request.page_owner_id;
	malformedHeader.page_id = request.page_id;
	malformedHeader.generation = request.generation;
	malformedHeader.right_sibling = PageId(999U);
	const BTreePage malformed =
			BTreePage::create(treeOwner(currentPool, request.page_id, LogSequenceNumber(2U)), malformedHeader);
	overwriteCurrentBTreePage(*io, *metadata, request.page_id,
						  treeOwner(currentPool, request.page_id, LogSequenceNumber(2U)), malformed);

	const std::array<std::byte, 1U> key = {std::byte{'a'}};
	expectFormatError([&] { static_cast<void>(store.erase(request.page_owner_id, key, LogSequenceNumber(4U))); },
				  FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, PlansRootLeafRetirementAndRejectsTamperedPreparationBatches) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation root{request.page_owner_id, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_NE(metadata->findCatalog(snapshot, request.segment_id).owner_id, request.page_owner_id);
	const BTreeTreeRetirementPlan plan = store.planTreeRetirement(snapshot, request.page_owner_id, 1U);
	EXPECT_EQ(plan.expected_root, root);
	ASSERT_EQ(plan.pool_requests.size(), 1U);
	ASSERT_EQ(plan.pool_requests.front().expected_retirements.size(), 1U);
	EXPECT_EQ(plan.pool_requests.front().expected_retirements.front().location.page_id, request.page_id);
	std::vector<IndexTreeMutationPoolBatch> tamperedBatches;
	expectFormatError([&] { plan.preparer(tamperedBatches, LogSequenceNumber(4U)); }, FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, TraversesEveryInternalTreePageWhenPlanningRetirement) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation initialRoot{request.page_owner_id, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1U), {}, LogSequenceNumber(3U)));

	static_cast<void>(store.materializeAndPublishTree(
			request.segment_id, request.page_owner_id, 3U, 0U, LogSequenceNumber(4U),
			[](const CatalogEntry &poolEntry, std::span<const PageId> pageIds) {
				BTreePageHeader rootHeader;
				rootHeader.index_id = 17U;
				rootHeader.page_id = pageIds[0];
				rootHeader.generation = poolEntry.generation;
				rootHeader.kind = BTreeNodeKind::BPN_INTERNAL;
				rootHeader.level = 1U;
				rootHeader.leftmost_child = pageIds[1];
				BTreePage root = BTreePage::create(treeOwner(poolEntry, pageIds[0], LogSequenceNumber(4U)), rootHeader);
				root.insert({{std::byte{'m'}}, pageIds[2].value()});

				std::vector<BTreePage> pages;
				pages.push_back(std::move(root));
				for (const PageId leafId: std::span(pageIds).subspan(1U)) {
					BTreePageHeader leafHeader;
					leafHeader.index_id = 17U;
					leafHeader.page_id = leafId;
					leafHeader.generation = poolEntry.generation;
					pages.push_back(BTreePage::create(treeOwner(poolEntry, leafId, LogSequenceNumber(4U)), leafHeader));
				}
				return pages;
			}));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, request.page_owner_id);
	const BTreeTreeRetirementPlan plan = store.planTreeRetirement(snapshot, request.page_owner_id, 3U);
	ASSERT_EQ(plan.pool_requests.size(), 1U);
	const std::vector<IndexTreePageRetirement> &retirements = plan.pool_requests.front().expected_retirements;
	ASSERT_EQ(retirements.size(), 3U);
	EXPECT_EQ(retirements[0].location.page_id, root.root_page_id);
	EXPECT_NE(retirements[1].location.page_id, root.root_page_id);
	EXPECT_NE(retirements[2].location.page_id, root.root_page_id);
	EXPECT_EQ(retirements[0].location.segment_id, request.segment_id);
	EXPECT_EQ(retirements[1].location.segment_id, request.segment_id);
	EXPECT_EQ(retirements[2].location.segment_id, request.segment_id);
	IndexTreeMutationPoolBatch batch;
	batch.pool = metadata->findCatalog(snapshot, request.segment_id);
	batch.expected_retirements = retirements;
	std::vector<IndexTreeMutationPoolBatch> batches;
	batches.push_back(std::move(batch));
	EXPECT_NO_THROW(plan.preparer(batches, LogSequenceNumber(5U)));
	EXPECT_TRUE(batches.front().replacement_locations.empty());
	EXPECT_TRUE(batches.front().appended_locations.empty());
	std::vector<IndexTreeMutationPoolBatch> tamperedReserved = batches;
	tamperedReserved.front().newly_reserved = true;
	expectFormatError([&] { plan.preparer(tamperedReserved, LogSequenceNumber(5U)); },
				  FormatError::FERR_INVALID_STATE);
	std::vector<IndexTreeMutationPoolBatch> tamperedRetirements = batches;
	tamperedRetirements.front().expected_retirements.clear();
	expectFormatError([&] { plan.preparer(tamperedRetirements, LogSequenceNumber(5U)); },
				  FormatError::FERR_INVALID_STATE);

	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, request.page_owner_id, 1U)); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsAnUnroutedInternalChildDuringTreeRetirementPlanning) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation initialRoot{request.page_owner_id, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&initialRoot, 1U), {}, LogSequenceNumber(3U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry currentPool = metadata->findCatalog(snapshot, request.segment_id);
	BTreePageHeader malformedHeader;
	malformedHeader.index_id = request.page_owner_id;
	malformedHeader.page_id = request.page_id;
	malformedHeader.generation = request.generation;
	malformedHeader.kind = BTreeNodeKind::BPN_INTERNAL;
	malformedHeader.level = 1U;
	malformedHeader.leftmost_child = PageId(999U);
	const BTreePage malformed = BTreePage::create(treeOwner(currentPool, request.page_id, LogSequenceNumber(2U)),
													malformedHeader);
	overwriteCurrentBTreePage(*io, *metadata, request.page_id,
						  treeOwner(currentPool, request.page_id, LogSequenceNumber(2U)), malformed);
	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, request.page_owner_id, 2U)); },
					  FormatError::FERR_INVALID_STATE);
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsACyclicInternalTreeDuringRetirementPlanning) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pool(io, *metadata);
	BTreePageStore store(pool, *metadata);
	const SlottedPagePoolAllocationRequest request = allocationRequest();
	ASSERT_TRUE(store.allocate(request, emptyPage(LogSequenceNumber(2U)), PhysicalSlotId(40U), PhysicalSlotId(40U),
							   LogSequenceNumber(2U))
					.has_value());
	const IndexRootLocation root{request.page_owner_id, request.page_id, request.generation};
	static_cast<void>(store.publishRoots(std::span(&root, 1U), {}, LogSequenceNumber(3U)));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const CatalogEntry currentPool = metadata->findCatalog(snapshot, request.segment_id);
	BTreePageHeader malformedHeader;
	malformedHeader.index_id = request.page_owner_id;
	malformedHeader.page_id = request.page_id;
	malformedHeader.generation = request.generation;
	malformedHeader.kind = BTreeNodeKind::BPN_INTERNAL;
	malformedHeader.level = 1U;
	malformedHeader.leftmost_child = request.page_id;
	const BTreePage malformed = BTreePage::create(treeOwner(currentPool, request.page_id, LogSequenceNumber(2U)),
															 malformedHeader);
	overwriteCurrentBTreePage(*io, *metadata, request.page_id,
						  treeOwner(currentPool, request.page_id, LogSequenceNumber(2U)), malformed);

	expectFormatError([&] { static_cast<void>(store.planTreeRetirement(snapshot, request.page_owner_id, 2U)); },
				  FormatError::FERR_INVALID_STATE);
	snapshot.release();
}
