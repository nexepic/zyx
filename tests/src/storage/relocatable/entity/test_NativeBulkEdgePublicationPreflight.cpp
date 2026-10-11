/**
 * @file test_NativeBulkEdgePublicationPreflight.cpp
 * @brief Bounded streamed Edge publication preflight tests.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <optional>
#include <memory>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgePublicationPreflight.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeEndpointCache.hpp"

using namespace graph::storage;

namespace {

	NativeBulkEdgeRecord record(uint64_t id, uint64_t sourceId, uint64_t targetId, std::string type = "REL") {
		NativeBulkEdgeRecord value;
		value.edge_id = id;
		value.source_node_id = sourceId;
		value.target_node_id = targetId;
		value.type = std::move(type);
		value.properties = {{"rank", graph::PropertyValue(int64_t(id))}};
		return value;
	}

	NativeBulkEdgeRecordSource source(std::vector<NativeBulkEdgeRecord> records) {
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

	NativeBulkEdgePublicationPreflightOptions options() { return {3U, RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES, 4U, 8U}; }

} // namespace

TEST(NativeBulkEdgePublicationPreflightTest, ValidatesSealedLeasedEndpointsAndDerivesAllRunCounts) {
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 3U}};
	const NativeBulkEdgePublicationPreflightResult result = NativeBulkEdgePublicationPreflight::validate(
			source({record(100U, 1U, 2U, "REL_A"), record(101U, 2U, 3U, "REL_B"), record(102U, 1U, 3U, "REL_A")}),
			lease, options(), [](uint64_t id) { return id >= 1U && id <= 3U; });
	EXPECT_EQ(result.record_count, 3U);
	EXPECT_EQ(result.first_edge_id, 100U);
	EXPECT_EQ(result.last_edge_id, 102U);
	EXPECT_EQ(result.property_root_count, 3U);
	EXPECT_EQ(result.large_value_count, 0U);
	EXPECT_EQ(result.token_entry_count, 6U);
	EXPECT_EQ(result.source_run_count, 1U);
	EXPECT_EQ(result.token_fragments, 1U);
	EXPECT_EQ(result.outgoing_adjacency_fragments, 1U);
	EXPECT_EQ(result.incoming_adjacency_fragments, 1U);
	EXPECT_EQ(result.derivedCandidateFragments(), 3U);
}

TEST(NativeBulkEdgePublicationPreflightTest, ValidatesRepeatedEndpointsOnlyOnceWithinItsBoundedCache) {
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 3U}};
	size_t validationCalls = 0U;
	const NativeBulkEdgePublicationPreflightResult result = NativeBulkEdgePublicationPreflight::validate(
			source({record(100U, 1U, 2U), record(101U, 2U, 1U), record(102U, 1U, 2U)}), lease, options(),
			[&validationCalls](uint64_t id) {
				++validationCalls;
				return id == 1U || id == 2U;
			});

	EXPECT_EQ(result.record_count, 3U);
	EXPECT_EQ(validationCalls, 2U);
}

TEST(NativeBulkEdgeEndpointCacheTest, EvictsTheOldestEndpointAfterItsFixedCapacity) {
	NativeBulkEdgeEndpointCache cache;
	size_t validationCalls = 0U;
	const auto validate = [&validationCalls](uint64_t) {
		++validationCalls;
		return true;
	};
	for (uint64_t nodeId = 1U; nodeId <= 4'096U; ++nodeId) {
		ASSERT_TRUE(cache.validate(nodeId, validate));
	}
	EXPECT_EQ(validationCalls, 4'096U);
	EXPECT_TRUE(cache.validate(1U, validate));
	EXPECT_EQ(validationCalls, 4'096U);
	ASSERT_TRUE(cache.validate(4'097U, validate));
	EXPECT_EQ(validationCalls, 4'097U);
	EXPECT_TRUE(cache.validate(1U, validate));
	EXPECT_EQ(validationCalls, 4'098U);
}

TEST(NativeBulkEdgePublicationPreflightTest, PreservesExactSourceSpanAcrossSparseLeaseRanges) {
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 1U}, {102U, 1U}};
	const NativeBulkEdgePublicationPreflightResult result = NativeBulkEdgePublicationPreflight::validate(
			source({record(100U, 1U, 2U), record(102U, 2U, 1U)}), lease, options(),
			[](uint64_t id) { return id == 1U || id == 2U; });
	EXPECT_EQ(result.record_count, 2U);
	EXPECT_EQ(result.first_edge_id, 100U);
	EXPECT_EQ(result.last_edge_id, 102U);
}

TEST(NativeBulkEdgePublicationPreflightTest, CountsPropertyRootsThatRequireLargeValueStorageBeforeTokenAllocation) {
	NativeBulkEdgeRecord oversized = record(100U, 1U, 2U);
	oversized.properties = {{"payload", graph::PropertyValue(graph::PropertyValue::Bytes(
												RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES, std::byte{0x2A}))}};
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 1U}};
	const NativeBulkEdgePublicationPreflightResult result = NativeBulkEdgePublicationPreflight::validate(
			source({std::move(oversized)}), lease, options(), [](uint64_t id) { return id == 1U || id == 2U; });
	EXPECT_EQ(result.record_count, 1U);
	EXPECT_EQ(result.property_root_count, 1U);
	EXPECT_EQ(result.large_value_count, 1U);
}

TEST(NativeBulkEdgePublicationPreflightTest, RejectsMissingEndpointsForeignLeasesAndCombinedCandidateOverflow) {
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 2U}};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
						 source({record(100U, 1U, 9U)}), lease, options(), [](uint64_t id) { return id < 9U; })),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
						 source({record(102U, 1U, 2U)}), lease, options(), [](uint64_t) { return true; })),
				 FormatException);
	NativeBulkEdgePublicationPreflightOptions tiny = options();
	tiny.maximum_candidate_fragments = 2U;
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(source({record(100U, 1U, 2U)}), lease,
																				tiny, [](uint64_t) { return true; })),
				 FormatException);
}

TEST(NativeBulkEdgePublicationPreflightTest, RejectsAReplayThatChangesTheSealedRecordPayload) {
	const auto sourcePass = std::make_shared<size_t>(0U);
	const NativeBulkEdgeRecordSource unstableSource = [sourcePass] {
		const bool changed = (*sourcePass)++ != 0U;
		return [changed, emitted = false]() mutable -> std::optional<NativeBulkEdgeRecord> {
			if (emitted) {
				return std::nullopt;
			}
			emitted = true;
			return record(100U, 1U, 2U, changed ? "CHANGED" : "REL");
		};
	};
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 1U}};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
				 unstableSource, lease, options(), [](uint64_t id) { return id == 1U || id == 2U; })),
			 FormatException);
}
