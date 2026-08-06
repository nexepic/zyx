/**
 * @file test_BulkBTreeBuilder.cpp
 * @brief Dense bottom-up relocatable B+Tree bulk build tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <memory>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "tests/src/storage/relocatable/index/RelocatableBTreePageStoreTestUtils.hpp"

using namespace graph::storage;
using namespace relocatable_btree_test;

namespace {

	constexpr uint64_t INDEX_ID = 41;

	[[nodiscard]] BulkBTreeBuildOptions options(size_t maximumEntries = 2000U, size_t maximumPages = 2048U) {
		return {PhysicalSlotId(40), PhysicalSlotId(42), INDEX_ID, maximumEntries, maximumPages};
	}

	[[nodiscard]] std::vector<BTreePageEntry> entries(size_t count) {
		std::vector<BTreePageEntry> result;
		result.reserve(count);
		for (size_t index = 1; index <= count; ++index) {
			result.push_back({smallKey(static_cast<uint16_t>(index)), index});
		}
		return result;
	}

	[[nodiscard]] OrderedBTreeEntrySource sourceFor(std::vector<BTreePageEntry> entries) {
		const auto shared = std::make_shared<const std::vector<BTreePageEntry>>(std::move(entries));
		return [shared] {
			auto position = std::make_shared<size_t>(0U);
			return [shared, position]() -> std::optional<BTreePageEntry> {
				if (*position == shared->size()) {
					return std::nullopt;
				}
				return (*shared)[(*position)++];
			};
		};
	}

	[[nodiscard]] std::vector<BTreePageEntry> wideEntries(size_t count) {
		std::vector<BTreePageEntry> result;
		result.reserve(count);
		for (size_t index = 1; index <= count; ++index) {
			std::vector<std::byte> key(1500U, std::byte{'w'});
			key[0] = static_cast<std::byte>(index >> 8U);
			key[1] = static_cast<std::byte>(index);
			result.push_back({std::move(key), index});
		}
		return result;
	}

	[[nodiscard]] std::vector<BTreePageEntry> poolSizedEntries(size_t count) {
		std::vector<BTreePageEntry> result;
		result.reserve(count);
		for (size_t index = 1; index <= count; ++index) {
			std::vector<std::byte> key(72U, std::byte{'w'});
			key[0] = static_cast<std::byte>(index >> 8U);
			key[1] = static_cast<std::byte>(index);
			result.push_back({std::move(key), index});
		}
		return result;
	}

} // namespace

TEST_F(RelocatableBTreePageStoreTest, BulkBuildsAndReopensADenseTreeAcrossMultiplePagePools) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);
	// Use low-prefix-sharing keys so the test continues to exercise multiple
	// physical pools after packed prefix compression reduces per-entry overhead.
	const std::vector<BTreePageEntry> expected = poolSizedEntries(1800U);
	const BulkBTreeBuildResult result =
			BulkBTreeBuilder::build(store, *metadata, INDEX_ID, sourceFor(expected), options(), LogSequenceNumber(2));
	EXPECT_EQ(result.entry_count, expected.size());
	EXPECT_GT(result.page_count, RSS_PAGE_POOL_DATA_FRAME_COUNT);
	EXPECT_GE(result.root_level, 1U);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, INDEX_ID);
	const PageLocation rootLocation = metadata->findPageLocation(snapshot, root.root_page_id);
	EXPECT_EQ(metadata->findCatalog(snapshot, rootLocation.segment_id).physical_slot, PhysicalSlotId(41));
	const BTreePage rootPage = store.read(snapshot, root.root_page_id);
	const PageLocation firstLeaf = metadata->findPageLocation(snapshot, rootPage.header().leftmost_child);
	EXPECT_NE(rootLocation.segment_id, firstLeaf.segment_id);
	EXPECT_EQ(metadata->findCatalog(snapshot, firstLeaf.segment_id).physical_slot, PhysicalSlotId(40));
	BTreeIndex index(INDEX_ID, store, *metadata);
	for (const size_t value: {1U, 900U, 1800U}) {
		EXPECT_EQ(index.find(snapshot, expected[value - 1U].key), std::optional<uint64_t>(value));
	}
	EXPECT_FALSE(index.find(snapshot, poolSizedEntries(1801U).back().key).has_value());
	EXPECT_EQ(index.scan(snapshot, std::nullopt, expected.size()), expected);
	snapshot.release();

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPages, *reopenedMetadata);
	BTreeIndex reopenedIndex(INDEX_ID, reopenedStore, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	EXPECT_EQ(reopenedIndex.find(reopened, expected.back().key), std::optional<uint64_t>(1800U));
	EXPECT_EQ(reopenedIndex.scan(reopened, expected[1797U].key, 3U),
			  (std::vector<BTreePageEntry>{expected[1797U], expected[1798U], expected[1799U]}));
	reopened.release();
}

TEST_F(RelocatableBTreePageStoreTest, BulkBuildsAnEmptyTreeWithOneRootLeaf) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);
	const BulkBTreeBuildResult result =
			BulkBTreeBuilder::build(store, *metadata, INDEX_ID, sourceFor({}), options(1U, 1U), LogSequenceNumber(2));
	EXPECT_EQ(result.entry_count, 0U);
	EXPECT_EQ(result.page_count, 1U);
	EXPECT_EQ(result.root_level, 0U);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, INDEX_ID);
	const BTreePage rootPage = store.read(snapshot, root.root_page_id);
	EXPECT_EQ(rootPage.header().kind, BTreeNodeKind::BPN_LEAF);
	EXPECT_FALSE(rootPage.header().left_sibling.isValid());
	EXPECT_FALSE(rootPage.header().right_sibling.isValid());
	EXPECT_TRUE(BTreeIndex(INDEX_ID, store, *metadata).scan(snapshot, std::nullopt, 1U).empty());
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, BulkBuildsAndReopensKeyOnlyLeaves) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);
	std::vector<BTreePageEntry> expected = entries(200U);
	for (BTreePageEntry &entry: expected) {
		entry.value_or_child = 0U;
	}
	BulkBTreeBuildOptions keyOnlyOptions = options(200U, 32U);
	keyOnlyOptions.leaf_value_layout = BTreeLeafValueLayout::BLVL_EMPTY;
	const BulkBTreeBuildResult result =
			BulkBTreeBuilder::build(store, *metadata, INDEX_ID, sourceFor(expected), keyOnlyOptions, LogSequenceNumber(2));
	EXPECT_EQ(result.entry_count, expected.size());

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	const IndexRootLocation root = metadata->findIndexRoot(snapshot, INDEX_ID);
	const BTreePage rootPage = store.read(snapshot, root.root_page_id);
	const PageId firstLeafId =
			rootPage.header().kind == BTreeNodeKind::BPN_LEAF ? root.root_page_id : rootPage.header().leftmost_child;
	EXPECT_EQ(store.read(snapshot, firstLeafId).header().leaf_value_layout, BTreeLeafValueLayout::BLVL_EMPTY);
	BTreeIndex index(INDEX_ID, store, *metadata, BTreeLeafValueLayout::BLVL_EMPTY);
	EXPECT_EQ(index.scan(snapshot, std::nullopt, expected.size()), expected);
	snapshot.release();

	metadata.reset();
	auto reopenedMetadata = RelocatableMetadataStore::open(io);
	SlottedPagePool reopenedPages(io, *reopenedMetadata);
	BTreePageStore reopenedStore(reopenedPages, *reopenedMetadata);
	MetadataSnapshotPin reopened = reopenedMetadata->openSnapshot();
	BTreeIndex reopenedIndex(INDEX_ID, reopenedStore, *reopenedMetadata, BTreeLeafValueLayout::BLVL_EMPTY);
	EXPECT_EQ(reopenedIndex.scan(reopened, expected[197U].key, 3U),
			  (std::vector<BTreePageEntry>{expected[197U], expected[198U], expected[199U]}));
	reopened.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInvalidOrChangedBulkSourcesBeforePublishingARoot) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);

	expectFormatError(
			[&] {
				static_cast<void>(BulkBTreeBuilder::build(store, *metadata, INDEX_ID, {}, options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	std::vector<BTreePageEntry> unordered = entries(2U);
	std::swap(unordered[0], unordered[1]);
	expectFormatError(
			[&] {
				static_cast<void>(
						BulkBTreeBuilder::build(store, *metadata, INDEX_ID, sourceFor(unordered), options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(BulkBTreeBuilder::build(store, *metadata, INDEX_ID, sourceFor(entries(3U)), options(2U),
																 LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);

	const auto pass = std::make_shared<size_t>(0U);
	const OrderedBTreeEntrySource changedSecondPass = [pass] {
		const size_t currentPass = (*pass)++;
		auto values = std::make_shared<std::vector<BTreePageEntry>>(entries(3U));
		if (currentPass != 0U) {
			(*values)[2].value_or_child = 99U;
		}
		auto position = std::make_shared<size_t>(0U);
		return [values, position]() -> std::optional<BTreePageEntry> {
			if (*position == values->size()) {
				return std::nullopt;
			}
			return (*values)[(*position)++];
		};
	};
	expectFormatError(
			[&] {
				static_cast<void>(
						BulkBTreeBuilder::build(store, *metadata, INDEX_ID, changedSecondPass, options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin afterFailure = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(afterFailure, INDEX_ID).has_value());
	afterFailure.release();
}

TEST_F(RelocatableBTreePageStoreTest, FailsClosedForBoundedAndUnstableBulkBuildInputs) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);
	const OrderedBTreeEntrySource noCursor = [] { return OrderedBTreeEntryCursor{}; };
	expectFormatError(
			[&] {
				static_cast<void>(BulkBTreeBuilder::build(store, *metadata, INDEX_ID, noCursor, options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(BulkBTreeBuilder::build(store, *metadata, INDEX_ID, sourceFor(entries(1800U)),
																 options(2000U, 1U), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] {
				static_cast<void>(BulkBTreeBuilder::build(
						store, *metadata, INDEX_ID, sourceFor({{{}, 1U}}), options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(BulkBTreeBuilder::build(
						store, *metadata, INDEX_ID, sourceFor({{smallKey(1U), 0U}}), options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_HEADER);
	std::vector<std::byte> oversizedKey(4000U, std::byte{'x'});
	expectFormatError(
			[&] {
				static_cast<void>(BulkBTreeBuilder::build(store, *metadata, INDEX_ID,
																 sourceFor({{std::move(oversizedKey), 1U}}), options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);

	const auto pass = std::make_shared<size_t>(0U);
	const OrderedBTreeEntrySource shortSecondPass = [pass] {
		auto values = std::make_shared<std::vector<BTreePageEntry>>(entries(*pass == 0U ? 3U : 2U));
		++*pass;
		auto position = std::make_shared<size_t>(0U);
		return [values, position]() -> std::optional<BTreePageEntry> {
			if (*position == values->size()) {
				return std::nullopt;
			}
			return (*values)[(*position)++];
		};
	};
	expectFormatError(
			[&] {
				static_cast<void>(
						BulkBTreeBuilder::build(store, *metadata, INDEX_ID, shortSecondPass, options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);

	const auto boundaryPass = std::make_shared<size_t>(0U);
	const OrderedBTreeEntrySource changedBoundary = [boundaryPass] {
		auto values = std::make_shared<std::vector<BTreePageEntry>>(entries(3U));
		if ((*boundaryPass)++ != 0U) {
			(*values)[0].key = smallKey(0U);
		}
		auto position = std::make_shared<size_t>(0U);
		return [values, position]() -> std::optional<BTreePageEntry> {
			if (*position == values->size()) {
				return std::nullopt;
			}
			return (*values)[(*position)++];
		};
	};
	expectFormatError(
			[&] {
				static_cast<void>(
						BulkBTreeBuilder::build(store, *metadata, INDEX_ID, changedBoundary, options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, BulkBuildsParentLevelsBySplittingWideSeparators) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);
	BulkBTreeBuildOptions wideOptions = options(200U, 128U);
	wideOptions.last_slot = PhysicalSlotId(45);
	const std::vector<BTreePageEntry> expected = wideEntries(100U);
	const BulkBTreeBuildResult result = BulkBTreeBuilder::build(
			store, *metadata, INDEX_ID, sourceFor(expected), wideOptions, LogSequenceNumber(2));
	EXPECT_GT(result.root_level, 1U);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	BTreeIndex index(INDEX_ID, store, *metadata);
	EXPECT_EQ(index.find(snapshot, expected.front().key), std::optional<uint64_t>(1U));
	EXPECT_EQ(index.find(snapshot, expected.back().key), std::optional<uint64_t>(100U));
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsSecondPassOrderValueAndLengthChanges) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);

	const auto zeroValuePass = std::make_shared<size_t>(0U);
	const OrderedBTreeEntrySource zeroValueSecondPass = [zeroValuePass] {
		auto values = std::make_shared<std::vector<BTreePageEntry>>(entries(3U));
		if ((*zeroValuePass)++ != 0U) {
			(*values)[1].value_or_child = 0U;
		}
		auto position = std::make_shared<size_t>(0U);
		return [values, position]() -> std::optional<BTreePageEntry> {
			if (*position == values->size()) {
				return std::nullopt;
			}
			return (*values)[(*position)++];
		};
	};
	expectFormatError(
			[&] {
				static_cast<void>(
						BulkBTreeBuilder::build(store, *metadata, INDEX_ID, zeroValueSecondPass, options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);

	const auto unorderedPass = std::make_shared<size_t>(0U);
	const OrderedBTreeEntrySource unorderedSecondPass = [unorderedPass] {
		auto values = std::make_shared<std::vector<BTreePageEntry>>(entries(3U));
		if ((*unorderedPass)++ != 0U) {
			std::swap((*values)[1], (*values)[2]);
		}
		auto position = std::make_shared<size_t>(0U);
		return [values, position]() -> std::optional<BTreePageEntry> {
			if (*position == values->size()) {
				return std::nullopt;
			}
			return (*values)[(*position)++];
		};
	};
	expectFormatError(
			[&] {
				static_cast<void>(
						BulkBTreeBuilder::build(store, *metadata, INDEX_ID, unorderedSecondPass, options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);

	const auto extraPass = std::make_shared<size_t>(0U);
	const OrderedBTreeEntrySource extraSecondPass = [extraPass] {
		auto values = std::make_shared<std::vector<BTreePageEntry>>(entries(*extraPass == 0U ? 2U : 3U));
		++*extraPass;
		auto position = std::make_shared<size_t>(0U);
		return [values, position]() -> std::optional<BTreePageEntry> {
			if (*position == values->size()) {
				return std::nullopt;
			}
			return (*values)[(*position)++];
		};
	};
	expectFormatError(
			[&] {
				static_cast<void>(
						BulkBTreeBuilder::build(store, *metadata, INDEX_ID, extraSecondPass, options(), LogSequenceNumber(2)));
			},
			FormatError::FERR_INVALID_STATE);
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_FALSE(metadata->findIndexRootOptional(snapshot, INDEX_ID).has_value());
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInvalidNativePlansAndTamperedPreparationBeforePublication) {
	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);

	MetadataSnapshotPin released = metadata->openSnapshot();
	released.release();
	expectFormatError(
			[&] {
				static_cast<void>(BulkBTreeBuilder::planNativeTree(
						store, *metadata, released, INDEX_ID, sourceFor(entries(1U)), options()));
			},
			FormatError::FERR_INVALID_STATE);

	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	BulkBTreeBuildOptions invalidLayout = options();
	invalidLayout.leaf_value_layout = static_cast<BTreeLeafValueLayout>(0xffU);
	expectFormatError(
			[&] {
				static_cast<void>(BulkBTreeBuilder::planNativeTree(
						store, *metadata, snapshot, INDEX_ID, sourceFor(entries(1U)), invalidLayout));
			},
			FormatError::FERR_INVALID_HEADER);

	const BTreeInitialTreeMutationPlan plan =
			BulkBTreeBuilder::planNativeTree(store, *metadata, snapshot, INDEX_ID, sourceFor(entries(2U)), options());
	ASSERT_TRUE(plan.preparer);
	expectFormatError(
			[&] {
				std::vector<IndexTreeMutationPoolBatch> noBatches;
				plan.preparer(noBatches, LogSequenceNumber(2U));
			},
			FormatError::FERR_INVALID_STATE);
	std::vector<IndexTreeMutationPoolBatch> expectedBatchCount(plan.new_pool_mutations.size());
	expectFormatError(
			[&] { plan.preparer(expectedBatchCount, LogSequenceNumber{}); }, FormatError::FERR_INVALID_STATE);
	expectFormatError(
			[&] { plan.preparer(expectedBatchCount, LogSequenceNumber(2U)); }, FormatError::FERR_INVALID_STATE);
	snapshot.release();

	static_cast<void>(
			BulkBTreeBuilder::build(store, *metadata, INDEX_ID, sourceFor(entries(1U)), options(), LogSequenceNumber(2U)));
	MetadataSnapshotPin published = metadata->openSnapshot();
	expectFormatError(
			[&] {
				static_cast<void>(BulkBTreeBuilder::planNativeTree(
						store, *metadata, published, INDEX_ID, sourceFor(entries(1U)), options()));
			},
			FormatError::FERR_INVALID_STATE);
	published.release();
}
