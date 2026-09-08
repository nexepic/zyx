/**
 * @file test_ExternalBTreeEntrySorter.cpp
 * @brief Anonymous-run external B+Tree entry sorting tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/index/BTreeIndex.hpp"
#include "graph/storage/relocatable/index/BulkBTreeBuilder.hpp"
#include "graph/storage/relocatable/index/ExternalBTreeEntrySorter.hpp"
#include "tests/src/storage/relocatable/index/RelocatableBTreePageStoreTestUtils.hpp"

using namespace graph::storage;
using namespace relocatable_btree_test;

namespace {

	constexpr uint64_t INDEX_ID = 61;

	[[nodiscard]] ExternalBTreeEntrySorterOptions sorterOptions() { return {64U, 128U, 2U}; }

	[[nodiscard]] BulkBTreeBuildOptions buildOptions() { return {PhysicalSlotId(40), PhysicalSlotId(43), INDEX_ID, 64U, 64U}; }

	[[nodiscard]] std::vector<BTreePageEntry> entries(size_t count) {
		std::vector<BTreePageEntry> result;
		result.reserve(count);
		for (size_t index = 1; index <= count; ++index) {
			result.push_back({smallKey(static_cast<uint16_t>(index)), index});
		}
		return result;
	}

	[[nodiscard]] std::vector<BTreePageEntry> readAll(OrderedBTreeEntryCursor cursor) {
		std::vector<BTreePageEntry> result;
		for (std::optional<BTreePageEntry> next = cursor(); next.has_value(); next = cursor()) {
			result.push_back(std::move(*next));
		}
		return result;
	}

} // namespace

TEST_F(RelocatableBTreePageStoreTest, SortsMultipleAnonymousRunsAndFeedsTwoPassBulkMaterialization) {
	ExternalBTreeEntrySorter sorter(sorterOptions());
	const std::vector<BTreePageEntry> expected = entries(9U);
	for (auto entry = expected.rbegin(); entry != expected.rend(); ++entry) {
		sorter.add(*entry);
	}
	const OrderedBTreeEntrySource source = sorter.finish();
	{
		EXPECT_EQ(readAll(source()), expected);
	}
	{
		EXPECT_EQ(readAll(source()), expected);
	}

	auto metadata = RelocatableMetadataStore::initializeNew(io, superblock());
	SlottedPagePool pages(io, *metadata);
	BTreePageStore store(pages, *metadata);
	const BulkBTreeBuildResult result =
			BulkBTreeBuilder::build(store, *metadata, INDEX_ID, source, buildOptions(), LogSequenceNumber(2));
	EXPECT_EQ(result.entry_count, expected.size());
	MetadataSnapshotPin snapshot = metadata->openSnapshot();
	EXPECT_EQ(BTreeIndex(INDEX_ID, store, *metadata).scan(snapshot, std::nullopt, 16U), expected);
	snapshot.release();
}

TEST_F(RelocatableBTreePageStoreTest, RejectsInvalidBoundsAndConcurrentOrPostFinishMutation) {
	expectFormatError([&] { static_cast<void>(ExternalBTreeEntrySorter({})); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(ExternalBTreeEntrySorter({1U, 64U, 1U})); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(ExternalBTreeEntrySorter({1U, RSS_BTREE_ENTRY_HEADER_BYTES - 1U, 2U})); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(ExternalBTreeEntrySorter({1U, 64U, 65U})); },
			FormatError::FERR_INVALID_HEADER);

	ExternalBTreeEntrySorter sorter({2U, 128U, 2U});
	sorter.add({smallKey(1U), 1U});
	sorter.add({smallKey(2U), 2U});
	expectFormatError([&] { sorter.add({smallKey(3U), 3U}); }, FormatError::FERR_INVALID_STATE);
	const OrderedBTreeEntrySource source = sorter.finish();
	expectFormatError([&] { sorter.add({smallKey(4U), 4U}); }, FormatError::FERR_INVALID_STATE);
	OrderedBTreeEntryCursor cursor = source();
	expectFormatError([&] { static_cast<void>(source()); }, FormatError::FERR_INVALID_STATE);
	static_cast<void>(cursor());
	cursor = {};
	EXPECT_EQ(readAll(source()), (std::vector<BTreePageEntry>{{smallKey(1U), 1U}, {smallKey(2U), 2U}}));

	ExternalBTreeEntrySorter bounded({2U, 128U, 2U});
	std::vector<std::byte> oversized(100U, std::byte{'x'});
	expectFormatError([&] { bounded.add({std::move(oversized), 1U}); }, FormatError::FERR_INVALID_STATE);
	ExternalBTreeEntrySorter maximumKey({2U, 5000U, 2U});
	std::vector<std::byte> overIndexKeyBound(4097U, std::byte{'x'});
	expectFormatError([&] { maximumKey.add({std::move(overIndexKeyBound), 1U}); }, FormatError::FERR_INVALID_STATE);
}

TEST_F(RelocatableBTreePageStoreTest, SupportsEmptyRepeatedAndMovedSorterSources) {
	ExternalBTreeEntrySorter empty(sorterOptions());
	const OrderedBTreeEntrySource emptySource = empty.finish();
	EXPECT_TRUE(readAll(emptySource()).empty());
	static_cast<void>(empty.finish());

	ExternalBTreeEntrySorter original({8U, 64U, 2U});
	original.add({smallKey(1U), 1U});
	ExternalBTreeEntrySorter moved(std::move(original));
	expectFormatError([&] { static_cast<void>(original.finish()); }, FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { original.add({smallKey(2U), 2U}); }, FormatError::FERR_INVALID_STATE);
	ExternalBTreeEntrySorter assigned(sorterOptions());
	assigned = std::move(moved);
	EXPECT_EQ(readAll(assigned.finish()()), (std::vector<BTreePageEntry>{{smallKey(1U), 1U}}));

	ExternalBTreeEntrySorter duplicates({8U, 64U, 2U});
	duplicates.add({smallKey(1U), 1U});
	duplicates.add({smallKey(1U), 2U});
	EXPECT_EQ(readAll(duplicates.finish()()),
			  (std::vector<BTreePageEntry>{{smallKey(1U), 1U}, {smallKey(1U), 2U}}));
}

TEST_F(RelocatableBTreePageStoreTest, PreservesBoundarySizedRunsAndMaximumAllowedMergeFanIn) {
	// `smallKey` has a 32-byte key and one B+Tree entry adds its fixed 32-byte
	// framing. A 64-byte run is therefore a useful exact-fit boundary: each
	// entry must form one complete private run, never an oversized buffer.
	ExternalBTreeEntrySorter sorter({64U, 64U, 64U});
	const std::vector<BTreePageEntry> expected = entries(64U);
	for (auto entry = expected.rbegin(); entry != expected.rend(); ++entry) {
		sorter.add(*entry);
	}

	const OrderedBTreeEntrySource source = sorter.finish();
	EXPECT_EQ(readAll(source()), expected);
	EXPECT_EQ(readAll(source()), expected);
}

TEST_F(RelocatableBTreePageStoreTest, PreservesDuplicateKeyOrderAcrossReducedAnonymousRuns) {
	// Each 64-byte run holds exactly one small entry. Five equal keys therefore
	// force repeated two-way reductions before the final cursor is opened.
	ExternalBTreeEntrySorter sorter({8U, 64U, 2U});
	const std::vector<BTreePageEntry> expected = {
			{smallKey(7U), 1U}, {smallKey(7U), 2U}, {smallKey(7U), 3U},
			{smallKey(7U), 4U}, {smallKey(7U), 5U},
	};
	for (const BTreePageEntry &entry: expected) {
		sorter.add(entry);
	}

	const OrderedBTreeEntrySource source = sorter.finish();
	EXPECT_EQ(readAll(source()), expected);
	EXPECT_EQ(readAll(source()), expected);

	// Reduction must retain source order within each equal-key group while still
	// ordering distinct groups globally. This mixes the two comparator paths.
	ExternalBTreeEntrySorter mixed({8U, 64U, 2U});
	for (const BTreePageEntry &entry: std::vector<BTreePageEntry>{{smallKey(2U), 1U},
																  {smallKey(1U), 10U},
																  {smallKey(2U), 2U},
																  {smallKey(1U), 11U},
																  {smallKey(2U), 3U}}) {
		mixed.add(entry);
	}
	EXPECT_EQ(readAll(mixed.finish()()), (std::vector<BTreePageEntry>{{smallKey(1U), 10U},
																	  {smallKey(1U), 11U},
																	  {smallKey(2U), 1U},
																	  {smallKey(2U), 2U},
																	  {smallKey(2U), 3U}}));
}
