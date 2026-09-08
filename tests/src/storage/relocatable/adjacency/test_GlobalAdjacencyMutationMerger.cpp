/**
 * @file test_GlobalAdjacencyMutationMerger.cpp
 * @brief Tests for final-state global adjacency source merging.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <cstddef>
#include <cstdint>
#include <optional>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/adjacency/GlobalAdjacencyMutationMerger.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] BTreePageEntry entry(uint8_t key, uint64_t value) {
			return {{std::byte{key}}, value};
		}

		[[nodiscard]] OrderedBTreeEntrySource source(std::vector<BTreePageEntry> entries) {
			return [entries = std::move(entries)] {
				return [entries, ordinal = size_t{0U}]() mutable -> std::optional<BTreePageEntry> {
					if (ordinal == entries.size()) {
						return std::nullopt;
					}
					return entries[ordinal++];
				};
			};
		}

		[[nodiscard]] std::vector<BTreePageEntry> collect(const OrderedBTreeEntrySource &input) {
			OrderedBTreeEntryCursor cursor = input();
			std::vector<BTreePageEntry> result;
			for (std::optional<BTreePageEntry> next = cursor(); next.has_value(); next = cursor()) {
				result.push_back(std::move(*next));
			}
			return result;
		}

	} // namespace

	TEST(GlobalAdjacencyMutationMergerTest, RewindablyAppliesTaggedUpdatesAndDeletes) {
		const OrderedBTreeEntrySource merged = GlobalAdjacencyMutationMerger::merge(
				source({entry(0x10U, 0U), entry(0x30U, 0U), entry(0x50U, 0U)}),
				source({entry(0x20U, 1U), entry(0x30U, 2U), entry(0x40U, 1U)}), 4U);
		const std::vector<BTreePageEntry> expected{
				entry(0x10U, 0U), entry(0x20U, 0U), entry(0x40U, 0U), entry(0x50U, 0U)};
		EXPECT_EQ(collect(merged), expected);
		EXPECT_EQ(collect(merged), expected);
	}

	TEST(GlobalAdjacencyMutationMergerTest, RejectsInvalidSourceShapesAndFinalBoundOverflow) {
		const OrderedBTreeEntrySource unorderedOld = GlobalAdjacencyMutationMerger::merge(
				source({entry(0x20U, 0U), entry(0x10U, 0U)}), source({entry(0x30U, 1U)}), 3U);
		EXPECT_THROW(static_cast<void>(collect(unorderedOld)), FormatException);

		const OrderedBTreeEntrySource invalidTag = GlobalAdjacencyMutationMerger::merge(
				source({entry(0x10U, 0U)}), source({entry(0x20U, 99U)}), 2U);
		EXPECT_THROW(static_cast<void>(collect(invalidTag)), FormatException);

		const OrderedBTreeEntrySource overbound = GlobalAdjacencyMutationMerger::merge(
				source({entry(0x10U, 0U), entry(0x20U, 0U)}), source({entry(0x30U, 1U)}), 2U);
		EXPECT_THROW(static_cast<void>(collect(overbound)), FormatException);
	}

	TEST(GlobalAdjacencyMutationMergerTest, RejectsMissingSourcesAndMalformedEntriesBeforeMerging) {
		const OrderedBTreeEntrySource valid = source({entry(0x10U, 0U)});
		EXPECT_THROW(static_cast<void>(GlobalAdjacencyMutationMerger::merge({}, valid, 1U)), FormatException);
		EXPECT_THROW(static_cast<void>(GlobalAdjacencyMutationMerger::merge(valid, {}, 1U)), FormatException);
		EXPECT_THROW(static_cast<void>(GlobalAdjacencyMutationMerger::merge(valid, valid, 0U)), FormatException);

		const OrderedBTreeEntrySource emptyCursor = [] { return OrderedBTreeEntryCursor{}; };
		EXPECT_THROW(static_cast<void>(collect(GlobalAdjacencyMutationMerger::merge(emptyCursor, valid, 1U))), FormatException);
		EXPECT_THROW(static_cast<void>(collect(GlobalAdjacencyMutationMerger::merge(valid, emptyCursor, 1U))), FormatException);

		const OrderedBTreeEntrySource emptyOld = source({entry(0x00U, 0U)});
		const OrderedBTreeEntrySource malformedOld = source({entry(0x10U, 1U)});
		EXPECT_THROW(static_cast<void>(collect(GlobalAdjacencyMutationMerger::merge(malformedOld, valid, 1U))), FormatException);
		const OrderedBTreeEntrySource malformedUpdate = source({entry(0x10U, 0U)});
		EXPECT_THROW(static_cast<void>(collect(GlobalAdjacencyMutationMerger::merge(emptyOld, malformedUpdate, 1U))), FormatException);
		const OrderedBTreeEntrySource emptyKey = source({{{}, 0U}});
		EXPECT_THROW(static_cast<void>(collect(GlobalAdjacencyMutationMerger::merge(emptyKey, valid, 1U))), FormatException);
		const OrderedBTreeEntrySource emptyUpdateKey = source({{{}, 1U}});
		EXPECT_THROW(static_cast<void>(collect(GlobalAdjacencyMutationMerger::merge(valid, emptyUpdateKey, 1U))), FormatException);
	}

} // namespace graph::storage
