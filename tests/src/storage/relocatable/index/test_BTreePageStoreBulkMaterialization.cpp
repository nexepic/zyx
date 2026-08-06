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
