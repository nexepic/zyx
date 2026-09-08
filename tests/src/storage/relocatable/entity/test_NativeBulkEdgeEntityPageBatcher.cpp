/**
 * @file test_NativeBulkEdgeEntityPageBatcher.cpp
 * @brief Tests for page-local streamed native Edge batching.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgeEntityPageBatcher.hpp"

namespace graph::storage {

	namespace {

		[[nodiscard]] NativeBulkEdgeRecord record(uint64_t edgeId) {
			NativeBulkEdgeRecord value;
			value.edge_id = edgeId;
			value.source_node_id = edgeId;
			value.target_node_id = edgeId + 1U;
			value.type = "REL";
			value.properties = {{"weight", static_cast<int64_t>(edgeId)}};
			return value;
		}

		[[nodiscard]] NativeBulkEdgeRecordSource source(std::vector<NativeBulkEdgeRecord> records) {
			return [records = std::move(records)] {
				return [records, ordinal = size_t{0U}]() mutable -> std::optional<NativeBulkEdgeRecord> {
					if (ordinal == records.size()) {
						return std::nullopt;
					}
					return records[ordinal++];
				};
			};
		}

		[[nodiscard]] NativeBulkEdgeMaterializerResolvers resolvers() {
			return {[](std::string_view name) -> std::optional<uint64_t> {
					return name == "REL" ? std::optional<uint64_t>(17U) : std::nullopt;
				},
					[](std::string_view name) -> std::optional<uint32_t> {
						return name == "weight" ? std::optional<uint32_t>(9U) : std::nullopt;
					}};
		}

	} // namespace

	TEST(NativeBulkEdgeEntityPageBatcherTest, EmitsCodecDerivedPageLocalBatchesInIdOrder) {
		const size_t capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_EDGE);
		ASSERT_GT(capacity, 0U);
		std::vector<NativeBulkEdgeRecord> records;
		records.reserve(capacity + 1U);
		for (size_t ordinal = 0U; ordinal < capacity + 1U; ++ordinal) {
			records.push_back(record(100U + ordinal));
		}
		const NativeGraphLogicalIdLeaseRange lease{100U, static_cast<uint64_t>(records.size())};
		std::vector<NativeBulkEdgeEntityPageBatch> batches;

		NativeBulkEdgeEntityPageBatcher::materializePages(
				source(std::move(records)), std::span(&lease, 1U), capacity + 1U, resolvers(),
				[&batches](NativeBulkEdgeEntityPageBatch &&batch) { batches.push_back(std::move(batch)); });

		ASSERT_EQ(batches.size(), 2U);
		ASSERT_EQ(batches[0].records.size(), capacity);
		ASSERT_EQ(batches[1].records.size(), 1U);
		for (size_t ordinal = 0U; ordinal < capacity; ++ordinal) {
			EXPECT_EQ(batches[0].records[ordinal].edge_id, 100U + ordinal);
		}
		EXPECT_EQ(batches[1].records.front().edge_id, 100U + capacity);
	}

TEST(NativeBulkEdgeEntityPageBatcherTest, RejectsMissingBatchVisitorBeforeReadingTheSource) {
		const NativeGraphLogicalIdLeaseRange lease{100U, 1U};
		EXPECT_THROW(NativeBulkEdgeEntityPageBatcher::materializePages(
						 source({record(100U)}), std::span(&lease, 1U), 1U, resolvers(), {}),
					 FormatException);
}

TEST(NativeBulkEdgeEntityPageBatcherTest, DoesNotEmitAnEmptyTailAfterAnExactlyFullPage) {
	const size_t capacity = EntityPage::recordCapacity(EntityPageRecordKind::EPRK_EDGE);
	ASSERT_GT(capacity, 0U);
	std::vector<NativeBulkEdgeRecord> records;
	records.reserve(capacity);
	for (size_t ordinal = 0U; ordinal < capacity; ++ordinal) {
		records.push_back(record(100U + ordinal));
	}
	const NativeGraphLogicalIdLeaseRange lease{100U, static_cast<uint64_t>(records.size())};
	size_t emittedBatches = 0U;
	NativeBulkEdgeEntityPageBatcher::materializePages(
			source(std::move(records)), std::span(&lease, 1U), capacity, resolvers(),
			[&emittedBatches, capacity](NativeBulkEdgeEntityPageBatch &&batch) {
				++emittedBatches;
				EXPECT_EQ(batch.records.size(), capacity);
			});
	EXPECT_EQ(emittedBatches, 1U);
}

} // namespace graph::storage
