/**
 * @file test_BTreePageStoreBulkMaterialization.cpp
 * @brief Multi-pool atomic materialization tests for relocatable B+Tree pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <functional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BTreePageStore.hpp"
#include "tests/src/storage/relocatable/index/RelocatableBTreePageStoreTestUtils.hpp"

using namespace graph::storage;
using namespace relocatable_btree_test;

namespace {

	constexpr uint64_t INDEX_ID = 17;

	[[nodiscard]] std::array<IndexTreeMaterializationPoolOptions, 2> poolOptions() {
		return {{{PhysicalSlotId(40), PhysicalSlotId(41), INDEX_ID, RSS_PAGE_POOL_DATA_FRAME_COUNT},
				 {PhysicalSlotId(40), PhysicalSlotId(41), INDEX_ID, 2U}}};
	}

	[[nodiscard]] BTreePage buildCrossPoolTreePage(std::span<const IndexTreeMaterializedPool> pools,
													  size_t poolOrdinal, size_t pageOrdinal,
													  LogSequenceNumber checkpointLsn) {
		const IndexTreeMaterializedPool &pool = pools[poolOrdinal];
		const PageId pageId = pool.page_ids[pageOrdinal];
		BTreePageHeader header;
		header.index_id = INDEX_ID;
		header.page_id = pageId;
		header.generation = pool.pool.generation;

		if (poolOrdinal == 1U && pageOrdinal == 0U) {
			header.kind = BTreeNodeKind::BPN_INTERNAL;
			header.level = 1;
			header.leftmost_child = pools[0].page_ids[0];
			BTreePage root = BTreePage::create(treeOwner(pool.pool, pageId, checkpointLsn), header);
			root.insert({{std::byte{'m'}}, pools[1].page_ids[1].value()});
			return root;
		}

		if (poolOrdinal == 0U && pageOrdinal == 0U) {
			header.right_sibling = pools[1].page_ids[1];
			BTreePage leaf = BTreePage::create(treeOwner(pool.pool, pageId, checkpointLsn), header);
			leaf.insert({{std::byte{'a'}}, 1U});
			return leaf;
		}
		if (poolOrdinal == 1U && pageOrdinal == 1U) {
			header.left_sibling = pools[0].page_ids[0];
			BTreePage leaf = BTreePage::create(treeOwner(pool.pool, pageId, checkpointLsn), header);
			leaf.insert({{std::byte{'z'}}, 2U});
			return leaf;
		}
		return BTreePage::create(treeOwner(pool.pool, pageId, checkpointLsn), header);
	}

	[[nodiscard]] BTreePageStore::MultiPoolNewTreePageFactory crossPoolFactory(LogSequenceNumber checkpointLsn) {
		return [checkpointLsn](std::span<const IndexTreeMaterializedPool> pools, size_t poolOrdinal, size_t pageOrdinal) {
			return buildCrossPoolTreePage(pools, poolOrdinal, pageOrdinal, checkpointLsn);
		};
	}

} // namespace

TEST_F(RelocatableBTreePageStoreTest, AtomicallyMaterializesOneTreeAcrossMoreThanOnePagePool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);
	const auto options = poolOptions();
	MetadataSnapshotPin before = metadata->openSnapshot();
	ASSERT_FALSE(metadata->findIndexRootOptional(before, INDEX_ID).has_value());

	static_cast<void>(store.materializeAndPublishTreeAcrossPools(
			options, INDEX_ID, {1U, 0U}, LogSequenceNumber(2), crossPoolFactory(LogSequenceNumber(2))));
	EXPECT_FALSE(metadata->findIndexRootOptional(before, INDEX_ID).has_value());

	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(current, INDEX_ID);
	const BTreePage rootPage = store.read(current, root.root_page_id);
	ASSERT_EQ(rootPage.header().kind, BTreeNodeKind::BPN_INTERNAL);
	const PageId leftLeaf = rootPage.header().leftmost_child;
	const PageId rightLeaf = PageId(rootPage.entries().front().value_or_child);
	const PageLocation rootLocation = metadata->findPageLocation(current, root.root_page_id);
	const PageLocation leftLocation = metadata->findPageLocation(current, leftLeaf);
	const PageLocation rightLocation = metadata->findPageLocation(current, rightLeaf);
	EXPECT_NE(rootLocation.segment_id, leftLocation.segment_id);
	EXPECT_EQ(rootLocation.segment_id, rightLocation.segment_id);
	EXPECT_EQ(metadata->findCatalog(current, rootLocation.segment_id).physical_slot, PhysicalSlotId(41));
	EXPECT_EQ(metadata->findCatalog(current, leftLocation.segment_id).physical_slot, PhysicalSlotId(40));

	BTreeIndex index(INDEX_ID, store, *metadata);
	EXPECT_EQ(index.find(current, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
	EXPECT_EQ(index.find(current, std::vector<std::byte>{std::byte{'z'}}), std::optional<uint64_t>(2U));
	EXPECT_EQ(index.scan(current, std::nullopt, 10U),
			  (std::vector<BTreePageEntry>{{{std::byte{'a'}}, 1U}, {{std::byte{'z'}}, 2U}}));
	before.release();
	current.release();

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPages, *reopenedMetadata);
	BTreeIndex reopenedIndex(INDEX_ID, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedIndex.find(reopened, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
	EXPECT_EQ(reopenedIndex.find(reopened, std::vector<std::byte>{std::byte{'z'}}), std::optional<uint64_t>(2U));
	reopened.release();
}

TEST_F(RelocatableBTreePageStoreTest,
		TransactionallySplitsAPublishedCrossPoolTreeWithoutBreakingPinnedOrReopenedReads) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	NativeBTreeWriteContext writes(io, *metadata, pages);
	BTreePageStore store(pages, *metadata);
	const std::array<IndexTreeMaterializationPoolOptions, 2U> pools = {
			{{PhysicalSlotId(40U), PhysicalSlotId(41U), INDEX_ID, 1U},
			 {PhysicalSlotId(40U), PhysicalSlotId(41U), INDEX_ID, 2U}}};
	const auto structuralKey = [](uint16_t value) {
		std::vector<std::byte> key(1800U, std::byte{0x5AU});
		key[0] = static_cast<std::byte>(value >> 8U);
		key[1] = static_cast<std::byte>(value);
		return key;
	};
	const std::vector<std::byte> firstKey = structuralKey(0x0100U);
	const std::vector<std::byte> insertedKey = structuralKey(0x0140U);
	const std::vector<std::byte> secondKey = structuralKey(0x0180U);
	const std::vector<std::byte> rightKey = structuralKey(0x0300U);

	static_cast<void>(store.materializeAndPublishTreeAcrossPools(
			pools, INDEX_ID, {1U, 0U}, LogSequenceNumber(2U),
			[&](std::span<const IndexTreeMaterializedPool> materialized, size_t poolOrdinal, size_t pageOrdinal) {
				const IndexTreeMaterializedPool &pool = materialized[poolOrdinal];
				const PageId pageId = pool.page_ids[pageOrdinal];
				BTreePageHeader header;
				header.index_id = INDEX_ID;
				header.page_id = pageId;
				header.generation = pool.pool.generation;
				if (poolOrdinal == 1U && pageOrdinal == 0U) {
					header.kind = BTreeNodeKind::BPN_INTERNAL;
					header.level = 1U;
					header.leftmost_child = materialized[0].page_ids[0];
					BTreePage root = BTreePage::create(treeOwner(pool.pool, pageId, LogSequenceNumber(2U)), header);
					root.insert({rightKey, materialized[1].page_ids[1].value()});
					return root;
				}
				if (poolOrdinal == 0U && pageOrdinal == 0U) {
					header.right_sibling = materialized[1].page_ids[1];
					BTreePage leaf = BTreePage::create(treeOwner(pool.pool, pageId, LogSequenceNumber(2U)), header);
					leaf.insert({firstKey, 1U});
					leaf.insert({secondKey, 2U});
					return leaf;
				}
				header.left_sibling = materialized[0].page_ids[0];
				BTreePage leaf = BTreePage::create(treeOwner(pool.pool, pageId, LogSequenceNumber(2U)), header);
				leaf.insert({rightKey, 3U});
				return leaf;
			}));

	BTreeIndex index(INDEX_ID, store, *metadata);
	MetadataSnapshotPin before = metadata->openSnapshot();
	const IndexRootLocation beforeRoot = metadata->findIndexRoot(before, INDEX_ID);
	EXPECT_EQ(index.find(before, firstKey), std::optional<uint64_t>(1U));
	EXPECT_EQ(index.find(before, secondKey), std::optional<uint64_t>(2U));
	EXPECT_EQ(index.find(before, insertedKey), std::nullopt);
	EXPECT_EQ(index.find(before, rightKey), std::optional<uint64_t>(3U));

	NativeGraphWriteTransaction transaction = writes.beginTransaction();
	transaction.stageScalarIndexUpsert(store, INDEX_ID, insertedKey, 14U);
	ASSERT_TRUE(transaction.commit().committed());

	// The old mapping remains a complete pre-split tree until its snapshot is released.
	EXPECT_EQ(metadata->findIndexRoot(before, INDEX_ID), beforeRoot);
	EXPECT_EQ(index.find(before, insertedKey), std::nullopt);
	EXPECT_EQ(index.scan(before, std::nullopt, 8U).size(), 3U);

	MetadataSnapshotPin current = metadata->openSnapshot();
	const IndexRootLocation currentRoot = metadata->findIndexRoot(current, INDEX_ID);
	const BTreePage root = store.read(current, currentRoot.root_page_id);
	ASSERT_EQ(root.header().kind, BTreeNodeKind::BPN_INTERNAL);
	ASSERT_EQ(root.header().level, 1U);
	ASSERT_EQ(root.entries().size(), 2U);
	const PageLocation rootLocation = metadata->findPageLocation(current, currentRoot.root_page_id);
	const PageLocation leftLocation = metadata->findPageLocation(current, root.header().leftmost_child);
	EXPECT_NE(rootLocation.segment_id, leftLocation.segment_id);
	EXPECT_EQ(metadata->findCatalog(current, rootLocation.segment_id).physical_slot, PhysicalSlotId(41U));
	EXPECT_EQ(metadata->findCatalog(current, leftLocation.segment_id).physical_slot, PhysicalSlotId(40U));
	EXPECT_EQ(index.find(current, firstKey), std::optional<uint64_t>(1U));
	EXPECT_EQ(index.find(current, insertedKey), std::optional<uint64_t>(14U));
	EXPECT_EQ(index.find(current, secondKey), std::optional<uint64_t>(2U));
	EXPECT_EQ(index.find(current, rightKey), std::optional<uint64_t>(3U));
	EXPECT_EQ(index.scan(current, std::nullopt, 8U).size(), 4U);
	before.release();
	current.release();

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPages, *reopenedMetadata);
	BTreeIndex reopenedIndex(INDEX_ID, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedIndex.find(reopened, firstKey), std::optional<uint64_t>(1U));
	EXPECT_EQ(reopenedIndex.find(reopened, insertedKey), std::optional<uint64_t>(14U));
	EXPECT_EQ(reopenedIndex.find(reopened, secondKey), std::optional<uint64_t>(2U));
	EXPECT_EQ(reopenedIndex.find(reopened, rightKey), std::optional<uint64_t>(3U));
	EXPECT_EQ(reopenedIndex.scan(reopened, std::nullopt, 8U).size(), 4U);
	reopened.release();
}

TEST_F(RelocatableBTreePageStoreTest, PlansCrossPoolTreeRetirementInStablePoolAndPageOrder) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);
	const auto options = poolOptions();
	static_cast<void>(store.materializeAndPublishTreeAcrossPools(
			options, INDEX_ID, {1U, 0U}, LogSequenceNumber(2U), crossPoolFactory(LogSequenceNumber(2U))));

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, INDEX_ID);
	const BTreePage rootPage = store.read(snapshot, root.root_page_id);
	const PageId leftLeaf = rootPage.header().leftmost_child;
	const PageId rightLeaf(rootPage.entries().front().value_or_child);
	const PageLocation rootLocation = metadata->findPageLocation(snapshot, root.root_page_id);
	const PageLocation leftLocation = metadata->findPageLocation(snapshot, leftLeaf);
	const PageLocation rightLocation = metadata->findPageLocation(snapshot, rightLeaf);

	const BTreeTreeRetirementPlan plan = store.planTreeRetirement(snapshot, INDEX_ID, 3U);
	ASSERT_EQ(plan.expected_root, root);
	ASSERT_EQ(plan.pool_requests.size(), 2U);
	EXPECT_LT(plan.pool_requests[0].segment_id, plan.pool_requests[1].segment_id);
	for (const IndexTreeMutationPoolRequest &request: plan.pool_requests) {
		ASSERT_FALSE(request.expected_retirements.empty());
		for (size_t ordinal = 1U; ordinal < request.expected_retirements.size(); ++ordinal) {
			EXPECT_LT(request.expected_retirements[ordinal - 1U].location.page_id,
					  request.expected_retirements[ordinal].location.page_id);
		}
	}

	const auto leftIterator =
			std::ranges::find(plan.pool_requests, leftLocation.segment_id, &IndexTreeMutationPoolRequest::segment_id);
	const auto rightIterator =
			std::ranges::find(plan.pool_requests, rootLocation.segment_id, &IndexTreeMutationPoolRequest::segment_id);
	ASSERT_NE(leftIterator, plan.pool_requests.end());
	ASSERT_NE(rightIterator, plan.pool_requests.end());
	const IndexTreeMutationPoolRequest &leftRequest = *leftIterator;
	const IndexTreeMutationPoolRequest &rightRequest = *rightIterator;
	ASSERT_EQ(leftRequest.expected_retirements.size(), 1U);
	EXPECT_EQ(leftRequest.expected_retirements.front().location, leftLocation);
	ASSERT_EQ(rightRequest.expected_retirements.size(), 2U);
	EXPECT_EQ(rightRequest.expected_retirements[0].location, rootLocation);
	EXPECT_EQ(rightRequest.expected_retirements[1].location, rightLocation);

	std::vector<IndexTreeMutationPoolBatch> batches;
	batches.reserve(plan.pool_requests.size());
	for (const IndexTreeMutationPoolRequest &request: plan.pool_requests) {
		IndexTreeMutationPoolBatch batch;
		batch.pool = metadata->findCatalog(snapshot, request.segment_id);
		batch.expected_retirements = request.expected_retirements;
		batches.push_back(std::move(batch));
	}
	EXPECT_NO_THROW(plan.preparer(batches, LogSequenceNumber(3U)));
	for (const IndexTreeMutationPoolBatch &batch: batches) {
		EXPECT_TRUE(batch.replacement_locations.empty());
		EXPECT_TRUE(batch.appended_locations.empty());
	}
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RetiresACrossPoolTreeAtomicallyAndReclaimsOnlyAfterSnapshotRelease) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);
	const auto options = poolOptions();
	static_cast<void>(store.materializeAndPublishTreeAcrossPools(
			options, INDEX_ID, {1U, 0U}, LogSequenceNumber(2U), crossPoolFactory(LogSequenceNumber(2U))));

	MetadataSnapshotPin oldSnapshot = metadata->openSnapshot();
	const IndexRootLocation oldRoot = metadata->findIndexRoot(oldSnapshot, INDEX_ID);
	const BTreePage oldRootPage = store.read(oldSnapshot, oldRoot.root_page_id);
	const PageId oldLeftLeaf = oldRootPage.header().leftmost_child;
	const PageId oldRightLeaf(oldRootPage.entries().front().value_or_child);
	const PageLocation oldRootLocation = metadata->findPageLocation(oldSnapshot, oldRoot.root_page_id);
	const PageLocation oldLeftLocation = metadata->findPageLocation(oldSnapshot, oldLeftLeaf);
	const PageLocation oldRightLocation = metadata->findPageLocation(oldSnapshot, oldRightLeaf);
	ASSERT_NE(oldRootLocation.segment_id, oldLeftLocation.segment_id);
	ASSERT_EQ(oldRootLocation.segment_id, oldRightLocation.segment_id);
	const uint8_t leftAvailableBefore = pages.availableFrameCount(oldSnapshot, oldLeftLocation.segment_id);
	const uint8_t rightAvailableBefore = pages.availableFrameCount(oldSnapshot, oldRootLocation.segment_id);

	NativeBTreeWriteContext context(io, *metadata, pages);
	NativeGraphWriteTransaction transaction = context.beginTransaction();
	transaction.stageScalarIndexDrop(store, INDEX_ID, 3U);
	static_cast<void>(transaction.commit());

	BTreeIndex index(INDEX_ID, store, *metadata);
	EXPECT_EQ(index.find(oldSnapshot, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
	EXPECT_EQ(index.find(oldSnapshot, std::vector<std::byte>{std::byte{'z'}}), std::optional<uint64_t>(2U));
	MetadataSnapshotPin current = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(current, INDEX_ID).has_value());
	EXPECT_EQ(pages.availableFrameCount(current, oldLeftLocation.segment_id), leftAvailableBefore);
	EXPECT_EQ(pages.availableFrameCount(current, oldRootLocation.segment_id), rightAvailableBefore);
	current.release();

	oldSnapshot.release();
	store.reclaimRetiredFrames(oldLeftLocation.segment_id);
	store.reclaimRetiredFrames(oldRootLocation.segment_id);
	MetadataSnapshotPin reclaimed = metadata->openSnapshot();
	EXPECT_EQ(pages.availableFrameCount(reclaimed, oldLeftLocation.segment_id),
			  static_cast<uint8_t>(leftAvailableBefore + 1U));
	EXPECT_EQ(pages.availableFrameCount(reclaimed, oldRootLocation.segment_id),
			  static_cast<uint8_t>(rightAvailableBefore + 2U));
	reclaimed.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInvalidBulkPlansAndReusesAnUnpublishedPhysicalPool) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);
	const auto options = poolOptions();

	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTreeAcrossPools(
						{}, INDEX_ID, {}, LogSequenceNumber(2), crossPoolFactory(LogSequenceNumber(2))));
			},
			FormatError::FERR_INVALID_HEADER);
	std::array<IndexTreeMaterializationPoolOptions, 1> invalidPool{{
			{PhysicalSlotId{}, PhysicalSlotId(40), INDEX_ID, 1U},
	}};
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTreeAcrossPools(
						invalidPool, INDEX_ID, {}, LogSequenceNumber(2), crossPoolFactory(LogSequenceNumber(2))));
			},
			FormatError::FERR_INVALID_HEADER);
	invalidPool[0] = {PhysicalSlotId(40), PhysicalSlotId(40), INDEX_ID,
						  static_cast<size_t>(RSS_PAGE_POOL_DATA_FRAME_COUNT + 1U)};
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTreeAcrossPools(
						invalidPool, INDEX_ID, {}, LogSequenceNumber(2), crossPoolFactory(LogSequenceNumber(2))));
			},
			FormatError::FERR_INVALID_HEADER);

	auto wrongFactory = [](std::span<const IndexTreeMaterializedPool> pools, size_t poolOrdinal, size_t pageOrdinal) {
		BTreePage page = buildCrossPoolTreePage(pools, poolOrdinal, pageOrdinal, LogSequenceNumber(2));
		if (poolOrdinal == 0U && pageOrdinal == 0U) {
			PageOwnerHeader foreignOwner = treeOwner(pools[poolOrdinal].pool, PageId(999), LogSequenceNumber(2));
			BTreePageHeader foreignHeader = page.header();
			foreignHeader.page_id = foreignOwner.page_id;
			foreignHeader.entry_count = 0;
			return BTreePage::create(foreignOwner, foreignHeader);
		}
		return page;
	};
	expectFormatError(
			[&] {
				static_cast<void>(store.materializeAndPublishTreeAcrossPools(
						options, INDEX_ID, {1U, 0U}, LogSequenceNumber(2), wrongFactory));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin afterFailure = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(afterFailure, INDEX_ID).has_value());
	afterFailure.release();

	static_cast<void>(store.materializeAndPublishTreeAcrossPools(
			options, INDEX_ID, {1U, 0U}, LogSequenceNumber(2), crossPoolFactory(LogSequenceNumber(2))));
	MetadataSnapshotPin current = metadata->openSnapshot();
	BTreeIndex index(INDEX_ID, store, *metadata);
	EXPECT_EQ(index.find(current, std::vector<std::byte>{std::byte{'z'}}), std::optional<uint64_t>(2U));
	current.release();
}

TEST_F(RelocatableBTreePageStoreTest, RecoversAnInterruptedMultiPoolTreePublicationFromTheWal) {
	{
		auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
		SlottedPagePool pages(io, *metadata);
		BTreePageStore store(pages, *metadata);
		const auto pools = poolOptions();
		MetadataPublishOptions publishOptions;
		publishOptions.bootstrap_failpoint = BootstrapPublishFailpoint::BPF_AFTER_INACTIVE_WRITE;
		EXPECT_THROW(static_cast<void>(store.materializeAndPublishTreeAcrossPools(
						 pools, INDEX_ID, {1U, 0U}, LogSequenceNumber(2), crossPoolFactory(LogSequenceNumber(2)),
						 publishOptions)),
				 BootstrapPublishInterrupted);
	}

	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPages, *reopenedMetadata);
	BTreeIndex index(INDEX_ID, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(index.find(reopened, std::vector<std::byte>{std::byte{'a'}}), std::optional<uint64_t>(1U));
	EXPECT_EQ(index.find(reopened, std::vector<std::byte>{std::byte{'z'}}), std::optional<uint64_t>(2U));
	reopened.release();
}
