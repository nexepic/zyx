/**
 * @file test_OfflineVectorAdjacencyQuality.cpp
 * @brief Tests for bounded offline adjacency-neighbor quality measurement.
 */

#include <algorithm>
#include <bit>
#include <functional>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/maintenance/OfflineVectorAdjacencyQuality.hpp"
#include "graph/storage/relocatable/maintenance/OfflineVectorAdjacencyRebuild.hpp"

using namespace graph::storage;

namespace {

	std::vector<std::byte> bfloat16(std::initializer_list<float> values) {
		std::vector<std::byte> bytes(values.size() * sizeof(uint16_t));
		size_t ordinal = 0U;
		for (const float value: values) {
			LittleEndianCodec::writeU16(bytes, ordinal * sizeof(uint16_t),
									 static_cast<uint16_t>(std::bit_cast<uint32_t>(value) >> 16U));
			++ordinal;
		}
		return bytes;
	}

	std::vector<CanonicalVectorRecord> records() {
		return {{1U, bfloat16({0.0F, 0.0F})}, {2U, bfloat16({0.0F, 1.0F})},
				{3U, bfloat16({10.0F, 0.0F})}, {4U, bfloat16({10.0F, 1.0F})},
				{5U, bfloat16({5.0F, 5.0F})}};
	}

	std::vector<CanonicalVectorRecord> candidateRecords(size_t count) {
		std::vector<CanonicalVectorRecord> result;
		result.reserve(count);
		for (size_t ordinal = 0U; ordinal < count; ++ordinal) {
			const float value = static_cast<float>(ordinal + 1U);
			result.push_back({static_cast<uint64_t>(ordinal + 1U),
						  bfloat16({value, static_cast<float>((ordinal * 7U) % 29U + 1U),
									static_cast<float>((ordinal * 13U) % 31U + 1U),
									static_cast<float>((ordinal * 17U) % 37U + 1U)})});
		}
		return result;
	}

	OfflineVectorAdjacencyRebuildOptions candidateOptions(size_t maximumRecords, size_t searchBudget,
												 uint16_t maximumDegree) {
		OfflineVectorAdjacencyRebuildOptions result;
		result.build_mode = VectorAdjacencyBuildMode::VABM_CANDIDATE_PRUNED;
		result.maximum_candidate_records = maximumRecords;
		result.candidate_search_budget = searchBudget;
		result.pruning_alpha = 1.2F;
		result.maximum_degree = maximumDegree;
		return result;
	}

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

} // namespace

TEST(OfflineVectorAdjacencyQualityTest, MeasuresExactNeighborRetentionWithoutClaimingQueryRecall) {
	const std::vector<VectorAdjacencyRecord> adjacency = {
			{1U, {2U, 5U}}, {2U, {1U, 5U}}, {3U, {4U, 5U}}, {4U, {3U, 5U}}, {5U, {2U, 4U}}};
	const VectorAdjacencyQualityReport result = OfflineVectorAdjacencyQuality::evaluate(
			records(), adjacency, {8U, 2U});
	EXPECT_EQ(result.vector_count, 5U);
	EXPECT_EQ(result.expected_exact_neighbors, 10U);
	EXPECT_EQ(result.retained_exact_neighbors, 10U);
	EXPECT_DOUBLE_EQ(result.adjacency_neighbor_recall, 1.0);
}

TEST(OfflineVectorAdjacencyQualityTest, MeasuresAValidButLowQualityCandidateGraph) {
	const std::vector<VectorAdjacencyRecord> adjacency = {
			{1U, {3U}}, {2U, {4U}}, {3U, {1U}}, {4U, {2U}}, {5U, {1U}}};
	const VectorAdjacencyQualityReport result = OfflineVectorAdjacencyQuality::evaluate(
			records(), adjacency, {8U, 1U});
	EXPECT_EQ(result.expected_exact_neighbors, 5U);
	EXPECT_EQ(result.retained_exact_neighbors, 0U);
	EXPECT_DOUBLE_EQ(result.adjacency_neighbor_recall, 0.0);
}

TEST(OfflineVectorAdjacencyQualityTest, RejectsUnboundedOrMalformedInputs) {
	const std::vector<CanonicalVectorRecord> source = records();
	const std::vector<VectorAdjacencyRecord> valid = {
			{1U, {2U}}, {2U, {1U}}, {3U, {4U}}, {4U, {3U}}, {5U, {2U}}};
	expectFormatError(
			[&] { static_cast<void>(OfflineVectorAdjacencyQuality::evaluate(source, valid, {0U, 1U})); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(OfflineVectorAdjacencyQuality::evaluate(source, valid, {8U, 5U})); },
			FormatError::FERR_INVALID_HEADER);
	std::vector<VectorAdjacencyRecord> malformed = valid;
	malformed[0].neighbors = {1U};
	expectFormatError(
			[&] { static_cast<void>(OfflineVectorAdjacencyQuality::evaluate(source, malformed, {8U, 1U})); },
			FormatError::FERR_INVALID_HEADER);
	malformed = valid;
	malformed[2].neighbors = {99U};
	expectFormatError(
			[&] { static_cast<void>(OfflineVectorAdjacencyQuality::evaluate(source, malformed, {8U, 1U})); },
		FormatError::FERR_INVALID_HEADER);
}

TEST(OfflineVectorAdjacencyQualityTest, RejectsEveryCanonicalRecordAndAdjacencyBoundary) {
	const std::vector<CanonicalVectorRecord> source = records();
	const std::vector<VectorAdjacencyRecord> valid = {
			{1U, {2U}}, {2U, {1U}}, {3U, {4U}}, {4U, {3U}}, {5U, {2U}}};
	const auto expectInvalid = [](const std::vector<CanonicalVectorRecord> &input,
									  const std::vector<VectorAdjacencyRecord> &adjacency,
									  VectorAdjacencyQualityOptions options = {8U, 1U}) {
		expectFormatError([&] { static_cast<void>(OfflineVectorAdjacencyQuality::evaluate(input, adjacency, options)); },
					  FormatError::FERR_INVALID_HEADER);
	};

	expectInvalid({}, {});
	std::vector<CanonicalVectorRecord> malformed = source;
	malformed.front().raw_bfloat16.clear();
	expectInvalid(malformed, valid);
	malformed = source;
	malformed.front().raw_bfloat16.push_back(std::byte{0});
	expectInvalid(malformed, valid);
	malformed = source;
	malformed[1].vector_id = 0U;
	expectInvalid(malformed, valid);
	malformed = source;
	malformed[1].vector_id = malformed.front().vector_id;
	expectInvalid(malformed, valid);
	malformed = source;
	malformed.back().raw_bfloat16.pop_back();
	expectInvalid(malformed, valid);
	malformed = source;
	malformed.front().raw_bfloat16 = bfloat16({std::numeric_limits<float>::infinity(), 0.0F});
	expectInvalid(malformed, valid);

	expectInvalid(source, std::vector<VectorAdjacencyRecord>(valid.begin(), valid.end() - 1U));
	std::vector<VectorAdjacencyRecord> malformedAdjacency = valid;
	malformedAdjacency.front().vector_id = 2U;
	expectInvalid(source, malformedAdjacency);
	malformedAdjacency = valid;
	malformedAdjacency.front().neighbors = {5U, 2U};
	expectInvalid(source, malformedAdjacency);
	malformedAdjacency = valid;
	malformedAdjacency.front().neighbors = {2U, 2U};
	expectInvalid(source, malformedAdjacency);

	expectInvalid(source, valid, {4U, 1U});
	expectInvalid(std::vector<CanonicalVectorRecord>{source.front()}, std::vector<VectorAdjacencyRecord>{{1U, {}}});
	expectInvalid(source, valid, {8U, 0U});
}

TEST(OfflineVectorAdjacencyQualityTest, RejectsAReportCapacityThatWouldOverflowRecallAccounting) {
	const std::vector<CanonicalVectorRecord> source = records();
	const std::vector<VectorAdjacencyRecord> valid = {
			{1U, {2U}}, {2U, {1U}}, {3U, {4U}}, {4U, {3U}}, {5U, {2U}}};
	const VectorAdjacencyQualityOptions options{std::numeric_limits<size_t>::max(), 2U};
	expectFormatError([&] { static_cast<void>(OfflineVectorAdjacencyQuality::evaluate(source, valid, options)); },
				  FormatError::FERR_ARITHMETIC_OVERFLOW);
}

TEST(OfflineVectorAdjacencyQualityTest, CandidatePrunedBuilderHasDeterministicBoundedWorkForEveryMetric) {
	constexpr size_t recordCount = 256U;
	constexpr size_t searchBudget = 8U;
	constexpr uint16_t maximumDegree = 4U;
	const std::vector<CanonicalVectorRecord> source = candidateRecords(recordCount);
	const OfflineVectorAdjacencyRebuildOptions options =
			candidateOptions(recordCount, searchBudget, maximumDegree);
	const uint64_t exactPairCount = static_cast<uint64_t>(recordCount) * (recordCount - 1U);
	const uint64_t maximumReverseLinks = static_cast<uint64_t>(recordCount) * maximumDegree;
	const uint64_t maximumCandidatePruneInput = static_cast<uint64_t>(recordCount) * searchBudget +
												 maximumReverseLinks * (maximumDegree + 1U);

	for (const VectorDistanceMetric metric:
			 {VectorDistanceMetric::VDM_L2, VectorDistanceMetric::VDM_COSINE, VectorDistanceMetric::VDM_INNER_PRODUCT}) {
		const OfflineVectorAdjacencyPreparedGraph first =
				OfflineVectorAdjacencyRebuild::buildGraph(metric, 4U, source, options);
		const OfflineVectorAdjacencyPreparedGraph second =
				OfflineVectorAdjacencyRebuild::buildGraph(metric, 4U, source, options);
		EXPECT_EQ(first, second);
		ASSERT_EQ(first.records.size(), recordCount);
		EXPECT_EQ(first.build_work.exact_pair_distance_evaluations, 0U);
		EXPECT_LE(first.build_work.candidate_frontier_pops,
				  static_cast<uint64_t>(recordCount) * searchBudget);
		EXPECT_LE(first.build_work.candidate_reverse_link_updates, maximumReverseLinks);
		EXPECT_LE(first.build_work.candidate_prune_calls, static_cast<uint64_t>(recordCount) + maximumReverseLinks);
		EXPECT_LE(first.build_work.candidate_prune_candidates, maximumCandidatePruneInput);
		EXPECT_LE(first.build_work.candidate_prune_pair_checks,
				  maximumCandidatePruneInput * maximumDegree);
		EXPECT_LT(first.build_work.candidate_prune_candidates, exactPairCount);

		for (const VectorAdjacencyMaterializationRecord &record: first.records) {
			EXPECT_LE(record.neighbors.size(), maximumDegree);
			EXPECT_TRUE(std::is_sorted(record.neighbors.begin(), record.neighbors.end()));
			EXPECT_EQ(std::adjacent_find(record.neighbors.begin(), record.neighbors.end()), record.neighbors.end());
			EXPECT_EQ(std::find(record.neighbors.begin(), record.neighbors.end(), record.vector_id), record.neighbors.end());
		}
	}
}

TEST(OfflineVectorAdjacencyQualityTest, BuilderRejectsEveryModeSpecificResourceLimitBeforeConstruction) {
	const std::vector<CanonicalVectorRecord> source = records();
	OfflineVectorAdjacencyRebuildOptions exact;
	exact.build_mode = VectorAdjacencyBuildMode::VABM_EXACT;
	exact.maximum_exact_records = source.size();
	exact.maximum_degree = 2U;
	const OfflineVectorAdjacencyRebuildOptions candidate = candidateOptions(source.size(), 2U, 2U);

	const auto expectInvalid = [&](VectorDistanceMetric metric, uint32_t dimensions,
									 const std::vector<CanonicalVectorRecord> &input,
									 const OfflineVectorAdjacencyRebuildOptions &options) {
		expectFormatError(
				[&] { static_cast<void>(OfflineVectorAdjacencyRebuild::buildGraph(metric, dimensions, input, options)); },
				FormatError::FERR_INVALID_HEADER);
	};

	expectInvalid(static_cast<VectorDistanceMetric>(99U), 2U, source, exact);
	expectInvalid(VectorDistanceMetric::VDM_L2, 0U, source, exact);
	expectInvalid(VectorDistanceMetric::VDM_L2, 2U, {}, exact);
	OfflineVectorAdjacencyRebuildOptions invalidMode = exact;
	invalidMode.build_mode = static_cast<VectorAdjacencyBuildMode>(99U);
	expectInvalid(VectorDistanceMetric::VDM_L2, 2U, source, invalidMode);
	OfflineVectorAdjacencyRebuildOptions exactWithoutCapacity = exact;
	exactWithoutCapacity.maximum_exact_records = 0U;
	expectInvalid(VectorDistanceMetric::VDM_L2, 2U, source, exactWithoutCapacity);
	OfflineVectorAdjacencyRebuildOptions exactTooSmall = exact;
	exactTooSmall.maximum_exact_records = source.size() - 1U;
	expectInvalid(VectorDistanceMetric::VDM_L2, 2U, source, exactTooSmall);

	OfflineVectorAdjacencyRebuildOptions candidateWithoutCapacity = candidate;
	candidateWithoutCapacity.maximum_candidate_records = 0U;
	expectInvalid(VectorDistanceMetric::VDM_L2, 2U, source, candidateWithoutCapacity);
	OfflineVectorAdjacencyRebuildOptions candidateTooSmall = candidate;
	candidateTooSmall.maximum_candidate_records = source.size() - 1U;
	expectInvalid(VectorDistanceMetric::VDM_L2, 2U, source, candidateTooSmall);
	OfflineVectorAdjacencyRebuildOptions candidateWithoutSearchBudget = candidate;
	candidateWithoutSearchBudget.candidate_search_budget = 0U;
	expectInvalid(VectorDistanceMetric::VDM_L2, 2U, source, candidateWithoutSearchBudget);
	OfflineVectorAdjacencyRebuildOptions candidateOversizedSearch = candidate;
	candidateOversizedSearch.candidate_search_budget = source.size() + 1U;
	expectInvalid(VectorDistanceMetric::VDM_L2, 2U, source, candidateOversizedSearch);
	OfflineVectorAdjacencyRebuildOptions candidateNanAlpha = candidate;
	candidateNanAlpha.pruning_alpha = std::numeric_limits<float>::quiet_NaN();
	expectInvalid(VectorDistanceMetric::VDM_L2, 2U, source, candidateNanAlpha);
	OfflineVectorAdjacencyRebuildOptions candidateContractingAlpha = candidate;
	candidateContractingAlpha.pruning_alpha = 0.9F;
	expectInvalid(VectorDistanceMetric::VDM_L2, 2U, source, candidateContractingAlpha);
	OfflineVectorAdjacencyRebuildOptions noDegree = exact;
	noDegree.maximum_degree = 0U;
	expectInvalid(VectorDistanceMetric::VDM_L2, 2U, source, noDegree);
	OfflineVectorAdjacencyRebuildOptions oversizedDegree = exact;
	oversizedDegree.maximum_degree = static_cast<uint16_t>(RSS_VECTOR_ADJACENCY_MAX_NEIGHBORS + 1U);
	expectInvalid(VectorDistanceMetric::VDM_L2, 2U, source, oversizedDegree);
}
