/**
 * @file test_NativeBulkEdgeMaterializer.cpp
 * @brief Bounded typed streamed Edge materialization tests.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "graph/core/Edge.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeMaterializer.hpp"

using namespace graph::storage;

namespace {

	[[nodiscard]] NativeBulkEdgeRecord record(uint64_t edgeId, uint64_t source, uint64_t target,
													std::string type = "KNOWS") {
		return {edgeId, source, target, std::move(type), {{"rank", graph::PropertyValue(int64_t(edgeId))}}};
	}

	[[nodiscard]] NativeBulkEdgeRecordSource source(std::vector<NativeBulkEdgeRecord> records) {
		return [records = std::move(records)]() mutable {
			size_t ordinal = 0U;
			return [records, ordinal]() mutable -> std::optional<NativeBulkEdgeRecord> {
				if (ordinal == records.size()) {
					return std::nullopt;
				}
				return records[ordinal++];
			};
		};
	}

	[[nodiscard]] NativeBulkEdgeMaterializerResolvers resolvers() {
		return {
				[](std::string_view name) -> std::optional<uint64_t> {
					return name == "KNOWS" ? std::optional<uint64_t>(17U) : std::nullopt;
				},
				[](std::string_view name) -> std::optional<uint32_t> {
					return name == "rank" ? std::optional<uint32_t>(9U) :
							name == "other" ? std::optional<uint32_t>(9U) : std::nullopt;
				},
		};
	}

	void expectFormatError(const std::function<void()> &operation) {
		EXPECT_THROW(operation(), FormatException);
	}

} // namespace

TEST(NativeBulkEdgeMaterializerTest, StreamsResolvedRecordsWithCanonicalPropertyState) {
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 3U}};
	std::vector<NativeBulkEdgeMaterializedRecord> received;
	NativeBulkEdgeMaterializer::materialize(source({record(100U, 4U, 8U), record(101U, 8U, 8U, "")}), lease,
															 3U, resolvers(),
															 [&received](NativeBulkEdgeMaterializedRecord &&value) {
																received.push_back(std::move(value));
															 });

	ASSERT_EQ(received.size(), 2U);
	EXPECT_EQ(received[0].edge_id, 100U);
	EXPECT_EQ(received[0].entity, (EdgeEntityRecord{4U, 8U, 17U}));
	EXPECT_EQ(received[0].properties.owner_entity_id, 100U);
	EXPECT_EQ(received[0].properties.owner_entity_type, graph::Edge::typeId);
	ASSERT_EQ(received[0].properties.entries.size(), 1U);
	EXPECT_EQ(received[0].properties.entries.front().key_token, 9U);
	EXPECT_EQ(received[1].entity.type_id, RSS_UNTYPED_RELATIONSHIP_TYPE_TOKEN);
}

TEST(NativeBulkEdgeMaterializerTest, RejectsUnleasedUnorderedOverboundAndUnresolvableRecords) {
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 2U}};
	expectFormatError([&] {
		NativeBulkEdgeMaterializer::materialize(source({record(102U, 1U, 2U)}), lease, 2U, resolvers(),
															 [](NativeBulkEdgeMaterializedRecord &&) {});
	});
	expectFormatError([&] {
		NativeBulkEdgeMaterializer::materialize(source({record(101U, 1U, 2U), record(100U, 1U, 2U)}), lease, 2U,
															 resolvers(), [](NativeBulkEdgeMaterializedRecord &&) {});
	});
	expectFormatError([&] {
		NativeBulkEdgeMaterializer::materialize(source({record(100U, 1U, 2U), record(101U, 1U, 2U)}), lease, 1U,
															 resolvers(), [](NativeBulkEdgeMaterializedRecord &&) {});
	});
	expectFormatError([&] {
		NativeBulkEdgeMaterializer::materialize(source({record(100U, 1U, 2U, "UNKNOWN")}), lease, 2U, resolvers(),
															 [](NativeBulkEdgeMaterializedRecord &&) {});
	});
}

TEST(NativeBulkEdgeMaterializerTest, RejectsDistinctPropertyNamesThatResolveToTheSameToken) {
	NativeBulkEdgeRecord duplicate = record(100U, 1U, 2U);
	duplicate.properties.push_back({"other", graph::PropertyValue(int64_t(2))});
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 1U}};
	expectFormatError([&] {
		NativeBulkEdgeMaterializer::materialize(source({std::move(duplicate)}), lease, 1U, resolvers(),
															 [](NativeBulkEdgeMaterializedRecord &&) {});
	});
}

TEST(NativeBulkEdgeMaterializerTest, RejectsMalformedRecordAndUnorderedLeaseBeforeEmission) {
	NativeBulkEdgeRecord malformed = record(100U, 0U, 2U);
	const std::vector<NativeGraphLogicalIdLeaseRange> validLease{{100U, 1U}};
	expectFormatError([&] {
		NativeBulkEdgeMaterializer::materialize(source({std::move(malformed)}), validLease, 1U, resolvers(),
															 [](NativeBulkEdgeMaterializedRecord &&) {});
	});
	const std::vector<NativeGraphLogicalIdLeaseRange> unorderedLease{{101U, 1U}, {100U, 1U}};
	expectFormatError([&] {
		NativeBulkEdgeMaterializer::materialize(source({record(100U, 1U, 2U)}), unorderedLease, 2U, resolvers(),
															 [](NativeBulkEdgeMaterializedRecord &&) {});
	});
}
