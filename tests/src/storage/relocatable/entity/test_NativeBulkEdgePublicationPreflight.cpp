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

TEST(NativeBulkEdgeEndpointCacheTest, DoesNotCacheRejectedEndpointsAndHandlesValidatorReentry) {
	NativeBulkEdgeEndpointCache cache;
	size_t rejectedCalls = 0U;
	EXPECT_FALSE(cache.validate(17U, [&rejectedCalls](uint64_t) {
		++rejectedCalls;
		return false;
	}));
	EXPECT_FALSE(cache.validate(17U, [&rejectedCalls](uint64_t) {
		++rejectedCalls;
		return false;
	}));
	EXPECT_EQ(rejectedCalls, 2U);

	bool reentered = false;
	EXPECT_TRUE(cache.validate(23U, [&cache, &reentered](uint64_t nodeId) {
		reentered = true;
		return cache.validate(nodeId, [](uint64_t) { return true; });
	}));
	EXPECT_TRUE(reentered);
	EXPECT_TRUE(cache.validate(23U, [](uint64_t) {
		ADD_FAILURE() << "a reentrantly validated endpoint must remain cached";
		return false;
	}));
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
						 source({record(0U, 1U, 2U)}), lease, options(), [](uint64_t) { return true; })),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
						 source({record(100U, 0U, 2U)}), lease, options(), [](uint64_t) { return true; })),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
						 source({record(100U, 1U, 0U)}), lease, options(), [](uint64_t) { return true; })),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
						 source({record(100U, 1U, 2U), record(100U, 2U, 3U)}), lease, options(),
								 [](uint64_t) { return true; })),
				 FormatException);
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
						 source({record(100U, 9U, 2U)}), lease, options(), [](uint64_t id) { return id != 9U; })),
				 FormatException);
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
	NativeBulkEdgePublicationPreflightOptions tinyRun = options();
	tinyRun.sort_run_bytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + 1U;
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
						 source({record(100U, 1U, 2U)}), lease, tinyRun, [](uint64_t) { return true; })),
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

TEST(NativeBulkEdgePublicationPreflightTest, RejectsAReplayThatChangesItsSealedRecordCountOrEdgeIdSpan) {
	const auto expectRejected = [](std::vector<NativeBulkEdgeRecord> initial,
									  std::vector<NativeBulkEdgeRecord> replay,
									  std::vector<NativeGraphLogicalIdLeaseRange> lease) {
		const auto sourcePass = std::make_shared<size_t>(0U);
		const NativeBulkEdgeRecordSource unstableSource = [sourcePass, initial = std::move(initial),
														 replay = std::move(replay)] {
			const std::vector<NativeBulkEdgeRecord> &records = (*sourcePass)++ == 0U ? initial : replay;
			return [records, ordinal = size_t{0U}]() mutable -> std::optional<NativeBulkEdgeRecord> {
				return ordinal == records.size() ? std::nullopt
														: std::optional<NativeBulkEdgeRecord>(records[ordinal++]);
			};
		};
		EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
						 unstableSource, lease, options(), [](uint64_t id) { return id == 1U || id == 2U; })),
					 FormatException);
	};

	expectRejected({record(100U, 1U, 2U)}, {record(100U, 1U, 2U), record(101U, 2U, 1U)}, {{100U, 2U}});
	expectRejected({record(100U, 1U, 2U)}, {record(101U, 1U, 2U)}, {{100U, 2U}});
	expectRejected({record(100U, 1U, 2U), record(102U, 2U, 1U)},
				   {record(100U, 1U, 2U), record(101U, 2U, 1U)}, {{100U, 3U}});
}

TEST(NativeBulkEdgePublicationPreflightTest, RejectsInvalidBoundsAndLeaseShapesBeforeReadingTheSource) {
	const std::vector<NativeGraphLogicalIdLeaseRange> validLease{{100U, 1U}};
	const NativeBulkEdgeRecordSource validSource = source({record(100U, 1U, 2U)});
	const NativeBulkEdgePersistedNodeValidator validEndpoint = [](uint64_t) { return true; };
	const auto expectsFailure = [&](const NativeBulkEdgeRecordSource &candidateSource,
								 const std::vector<NativeGraphLogicalIdLeaseRange> &lease,
								 const NativeBulkEdgePublicationPreflightOptions &candidateOptions,
								 const NativeBulkEdgePersistedNodeValidator &endpoint) {
		EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
						 candidateSource, lease, candidateOptions, endpoint)),
					 FormatException);
	};

	expectsFailure({}, validLease, options(), validEndpoint);
	expectsFailure(validSource, validLease, options(), {});
	NativeBulkEdgePublicationPreflightOptions invalidOptions = options();
	invalidOptions.maximum_records = 0U;
	expectsFailure(validSource, validLease, invalidOptions, validEndpoint);
	invalidOptions = options();
	invalidOptions.sort_run_bytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES;
	expectsFailure(validSource, validLease, invalidOptions, validEndpoint);
	invalidOptions = options();
	invalidOptions.maximum_sort_runs = 1U;
	expectsFailure(validSource, validLease, invalidOptions, validEndpoint);
	invalidOptions = options();
	invalidOptions.maximum_candidate_fragments = 0U;
	expectsFailure(validSource, validLease, invalidOptions, validEndpoint);

	expectsFailure(validSource, {}, options(), validEndpoint);
	expectsFailure(validSource, {{0U, 1U}}, options(), validEndpoint);
	expectsFailure(validSource, {{100U, 0U}}, options(), validEndpoint);
	expectsFailure(validSource, {{100U, 2U}, {101U, 1U}}, options(), validEndpoint);
	expectsFailure(validSource, {{std::numeric_limits<uint64_t>::max(), 2U}}, options(), validEndpoint);
}

TEST(NativeBulkEdgePublicationPreflightTest, AcceptsRecordsWithoutTokensAndProtectsCandidateCountArithmetic) {
	NativeBulkEdgeRecord noTokens = record(100U, 1U, 2U, {});
	noTokens.properties.clear();
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 1U}};
	const NativeBulkEdgePublicationPreflightResult result = NativeBulkEdgePublicationPreflight::validate(
			source({std::move(noTokens)}), lease, options(), [](uint64_t) { return true; });
	EXPECT_EQ(result.token_entry_count, 0U);
	EXPECT_EQ(result.property_root_count, 0U);
	EXPECT_EQ(result.large_value_count, 0U);
	EXPECT_EQ(result.token_fragments, 0U);
	EXPECT_EQ(result.derivedCandidateFragments(),
			  result.outgoing_adjacency_fragments + result.incoming_adjacency_fragments);

	NativeBulkEdgePublicationPreflightResult overflow;
	overflow.token_fragments = std::numeric_limits<size_t>::max();
	overflow.outgoing_adjacency_fragments = 1U;
	EXPECT_THROW(static_cast<void>(overflow.derivedCandidateFragments()), FormatException);
	overflow.token_fragments = 1U;
	overflow.outgoing_adjacency_fragments = 1U;
	overflow.incoming_adjacency_fragments = std::numeric_limits<size_t>::max();
	EXPECT_THROW(static_cast<void>(overflow.derivedCandidateFragments()), FormatException);
}

TEST(NativeBulkEdgePublicationPreflightTest, RollsSourceRunsAtTheConfiguredBoundWithoutChangingTheRecordSet) {
	const NativeBulkEdgeRecord first = record(100U, 1U, 2U);
	const NativeBulkEdgeRecord second = record(101U, 2U, 1U);
	const size_t sourceRecordBytes = NativeBulkEdgeRecordCodec::encodedSize(first) +
			RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES;
	NativeBulkEdgePublicationPreflightOptions bounded = options();
	bounded.maximum_records = 2U;
	bounded.sort_run_bytes = RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES + sourceRecordBytes;
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 2U}};
	const NativeBulkEdgePublicationPreflightResult result = NativeBulkEdgePublicationPreflight::validate(
			source({first, second}), lease, bounded, [](uint64_t id) { return id == 1U || id == 2U; });
	EXPECT_EQ(result.record_count, 2U);
	EXPECT_EQ(result.source_run_count, 2U);
}

TEST(NativeBulkEdgePublicationPreflightTest, RejectsARecordThatCannotFitInOneConfiguredSortRun) {
	const NativeBulkEdgeRecord edge = record(100U, 1U, 2U);
	const size_t sourceRecordBytes = NativeBulkEdgeRecordCodec::encodedSize(edge) +
			RSS_NATIVE_BULK_RUN_RECORD_HEADER_BYTES;
	NativeBulkEdgePublicationPreflightOptions bounded = options();
	bounded.sort_run_bytes = RSS_NATIVE_BULK_RUN_HEADER_BYTES + sourceRecordBytes - 1U;
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 1U}};

	EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
				source({edge}), lease, bounded, [](uint64_t id) { return id == 1U || id == 2U; })),
			 FormatException);
}

TEST(NativeBulkEdgePublicationPreflightTest, RejectsARecordBeyondTheConfiguredMaximumBeforeReplay) {
	NativeBulkEdgePublicationPreflightOptions bounded = options();
	bounded.maximum_records = 1U;
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 2U}};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
				source({record(100U, 1U, 2U), record(101U, 2U, 1U)}), lease, bounded,
				[](uint64_t id) { return id == 1U || id == 2U; })),
			 FormatException);
}

TEST(NativeBulkEdgePublicationPreflightTest, RejectsAnEmptySealedSourceBeforeBuildingPrivateRuns) {
	const std::vector<NativeGraphLogicalIdLeaseRange> lease{{100U, 1U}};
	EXPECT_THROW(static_cast<void>(NativeBulkEdgePublicationPreflight::validate(
				 source({}), lease, options(), [](uint64_t) { return true; })),
			 FormatException);
}
