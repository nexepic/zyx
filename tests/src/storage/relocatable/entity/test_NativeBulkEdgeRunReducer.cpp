/**
 * @file test_NativeBulkEdgeRunReducer.cpp
 * @brief Bounded native streamed run-reduction tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <optional>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgeRunReducer.hpp"

using namespace graph::storage;

namespace {

	BTreePageEntry entry(uint8_t key, uint64_t value) { return {{std::byte{key}}, value}; }

	OrderedBTreeEntrySource orderedSource(std::vector<BTreePageEntry> entries) {
		return [entries = std::move(entries)]() {
			size_t ordinal = 0U;
			return [entries, ordinal]() mutable -> std::optional<BTreePageEntry> {
				return ordinal == entries.size() ? std::nullopt
																: std::optional<BTreePageEntry>(entries[ordinal++]);
			};
		};
	}

	NativeBulkEdgeRunSource source(std::vector<NativeBulkRunFragment> fragments) {
		return [fragments = std::move(fragments)]() {
			size_t ordinal = 0U;
			return [fragments, ordinal]() mutable -> std::optional<OrderedBTreeEntrySource> {
				return ordinal == fragments.size() ? std::nullopt
																: std::optional<OrderedBTreeEntrySource>(
										orderedSource(std::move(fragments[ordinal++].entries)));
			};
		};
	}

} // namespace

TEST(NativeBulkEdgeRunReducerTest, ReducesOneLevelWithBoundedFanInAndPreservesGlobalOrder) {
	std::vector<NativeBulkRunFragment> reduced;
	NativeBulkEdgeRunReducer::reduceOneLevel(
			source({{{entry(0x10U, 1U)}}, {{entry(0x20U, 2U)}}, {{entry(0x30U, 3U)}}, {{entry(0x40U, 4U)}},
					{{entry(0x50U, 5U)}}}),
			2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 5U,
			[&](NativeBulkRunFragment &&fragment, bool) { reduced.push_back(std::move(fragment)); });
	ASSERT_EQ(reduced.size(), 3U);
	std::vector<uint8_t> keys;
	for (const NativeBulkRunFragment &fragment: reduced) {
		for (const BTreePageEntry &item: fragment.entries) {
			keys.push_back(std::to_integer<uint8_t>(item.key.front()));
		}
	}
	EXPECT_EQ(keys, (std::vector<uint8_t>{0x10U, 0x20U, 0x30U, 0x40U, 0x50U}));
}

TEST(NativeBulkEdgeRunReducerTest, RejectsDuplicateOrEmptyInputFragmentsBeforeOutput) {
	EXPECT_THROW(NativeBulkEdgeRunReducer::reduceOneLevel(
				source({{{entry(0x10U, 1U)}}, {{entry(0x10U, 2U)}}}), 2U,
				RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 2U, [](NativeBulkRunFragment &&, bool) {}),
			 FormatException);
	EXPECT_THROW(NativeBulkEdgeRunReducer::reduceOneLevel(
				source({{}}), 2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 1U,
				[](NativeBulkRunFragment &&, bool) {}),
			 FormatException);
}

TEST(NativeBulkEdgeRunReducerTest, CoalescesCrossRunTokenDuplicatesOnlyWhenRequested) {
	std::vector<NativeBulkRunFragment> reduced;
	NativeBulkEdgeRunReducer::reduceOneLevel(
			source({{{entry(0x10U, 0U)}}, {{entry(0x10U, 0U), entry(0x20U, 0U)}}}), 2U,
			RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 3U,
			[&](NativeBulkRunFragment &&fragment, bool) { reduced.push_back(std::move(fragment)); },
			NativeBulkEdgeRunDuplicatePolicy::NBERDP_KEEP_ONE);
	ASSERT_EQ(reduced.size(), 1U);
	ASSERT_EQ(reduced.front().entries.size(), 2U);
	EXPECT_EQ(std::to_integer<uint8_t>(reduced.front().entries[0].key.front()), 0x10U);
	EXPECT_EQ(std::to_integer<uint8_t>(reduced.front().entries[1].key.front()), 0x20U);
}

TEST(NativeBulkEdgeRunReducerTest, RepeatedLevelsGloballyOrderInterleavedInitialRuns) {
	std::vector<NativeBulkRunFragment> firstLevel;
	NativeBulkEdgeRunReducer::reduceOneLevel(
			source({{{entry(0x10U, 1U), entry(0x70U, 7U)}}, {{entry(0x20U, 2U), entry(0x80U, 8U)}},
					{{entry(0x30U, 3U), entry(0x90U, 9U)}}, {{entry(0x40U, 4U), entry(0xA0U, 10U)}}}),
			2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 8U,
			[&](NativeBulkRunFragment &&fragment, bool) { firstLevel.push_back(std::move(fragment)); });
	ASSERT_EQ(firstLevel.size(), 2U);
	EXPECT_EQ(std::to_integer<uint8_t>(firstLevel.front().entries.back().key.front()), 0x80U);
	EXPECT_EQ(std::to_integer<uint8_t>(firstLevel.back().entries.front().key.front()), 0x30U);

	std::vector<NativeBulkRunFragment> secondLevel;
	NativeBulkEdgeRunReducer::reduceOneLevel(
			source(std::move(firstLevel)), 2U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 8U,
			[&](NativeBulkRunFragment &&fragment, bool) { secondLevel.push_back(std::move(fragment)); });
	ASSERT_EQ(secondLevel.size(), 1U);
	std::vector<uint8_t> keys;
	for (const BTreePageEntry &item: secondLevel.front().entries) {
		keys.push_back(std::to_integer<uint8_t>(item.key.front()));
	}
	EXPECT_EQ(keys, (std::vector<uint8_t>{0x10U, 0x20U, 0x30U, 0x40U, 0x70U, 0x80U, 0x90U, 0xA0U}));
}

TEST(NativeBulkEdgeRunReducerTest, SplitsAGroupAtTheCandidateFragmentBoundaryWithoutLosingOrder) {
	const BTreePageEntry first = entry(0x10U, 1U);
	const BTreePageEntry second = entry(0x20U, 2U);
	const size_t oneEntryBytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES +
			BTreePageEntryCodec::encode(first).size();
	std::vector<NativeBulkRunFragment> reduced;
	std::vector<bool> closesRun;
	NativeBulkEdgeRunReducer::reduceOneLevel(
			source(std::vector<NativeBulkRunFragment>{{{first, second}}}), 2U, oneEntryBytes, 2U,
			[&](NativeBulkRunFragment &&fragment, bool closes) {
				reduced.push_back(std::move(fragment));
				closesRun.push_back(closes);
			});
	ASSERT_EQ(reduced.size(), 2U);
	ASSERT_EQ(reduced[0].entries.size(), 1U);
	ASSERT_EQ(reduced[1].entries.size(), 1U);
	EXPECT_EQ(reduced[0].entries.front(), first);
	EXPECT_EQ(reduced[1].entries.front(), second);
	EXPECT_EQ(closesRun, (std::vector<bool>{false, true}));
}
