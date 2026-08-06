/**
 * @file test_OfflineVectorAdjacencyQuality.cpp
 * @brief Tests for bounded offline adjacency-neighbor quality measurement.
 */

#include <bit>
#include <functional>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/maintenance/OfflineVectorAdjacencyQuality.hpp"

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
