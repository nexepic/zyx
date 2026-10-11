/**
 * @file test_VectorSemanticCodec.cpp
 * @brief Canonical property-to-raw-vector codec tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <stdexcept>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/vector/VectorSemanticCodec.hpp"
#include "graph/common/VectorMath.hpp"

using namespace graph;
using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	IndexDefinition vectorDefinition(std::string metric) {
		return {17U,
				"embedding",
				IndexDefinitionEntityKind::IDEK_NODE,
				IndexDefinitionKind::IDK_VECTOR,
				7U,
				{"embedding"},
				2U,
				std::move(metric)};
	}

} // namespace

TEST(RelocatableVectorSemanticCodecTest, EncodesCanonicalBFloat16RawRecordsAndOrderedNodeKeys) {
	const PropertyValue value(
			std::vector<PropertyValue>{PropertyValue(int64_t(-3)), PropertyValue(2.5)});
	const std::vector<std::byte> encoded = VectorSemanticCodec::encodeBFloat16(value, 2U);
	ASSERT_EQ(encoded.size(), 2U * sizeof(uint16_t));
	EXPECT_EQ(LittleEndianCodec::readU16(encoded, 0U), common::BFloat16(-3.0F).data);
	EXPECT_EQ(LittleEndianCodec::readU16(encoded, sizeof(uint16_t)), common::BFloat16(2.5F).data);

	const std::vector<std::byte> record = VectorSemanticCodec::rawRecord(99U, encoded);
	ASSERT_EQ(record.size(), sizeof(uint64_t) + encoded.size());
	EXPECT_EQ(LittleEndianCodec::readU64(record, 0U), 99U);
	EXPECT_TRUE(std::equal(encoded.begin(), encoded.end(), record.begin() + sizeof(uint64_t)));

	const std::vector<std::byte> lower = VectorSemanticCodec::nodeMappingKey(255U);
	const std::vector<std::byte> higher = VectorSemanticCodec::nodeMappingKey(256U);
	EXPECT_LT(lower, higher);
	EXPECT_EQ(higher, (std::vector<std::byte>{std::byte{0}, std::byte{0}, std::byte{0}, std::byte{0},
																				 std::byte{0}, std::byte{0}, std::byte{1}, std::byte{0}}));
}

TEST(RelocatableVectorSemanticCodecTest, RejectsMalformedCoordinatesAndRawRecordInputs) {
	const PropertyValue wrongShape(std::vector<PropertyValue>{PropertyValue(1.0)});
	const PropertyValue nonList(1.0);
	const PropertyValue nonNumeric(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue("bad")});
	const PropertyValue nonFinite(
			std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(std::numeric_limits<double>::infinity())});
	const PropertyValue aboveFloatRange(
			std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(std::numeric_limits<double>::max())});
	const PropertyValue belowFloatRange(
			std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(-std::numeric_limits<double>::max())});
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::encodeBFloat16(wrongShape, 0U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(VectorSemanticCodec::encodeBFloat16(
						wrongShape, RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS + 1U));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::encodeBFloat16(nonList, 1U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::encodeBFloat16(wrongShape, 2U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::encodeBFloat16(nonNumeric, 2U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::encodeBFloat16(nonFinite, 2U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::encodeBFloat16(aboveFloatRange, 2U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::encodeBFloat16(belowFloatRange, 2U)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(VectorSemanticCodec::rawRecord(1U, std::span<const std::byte>{})); },
			FormatError::FERR_INVALID_HEADER);
	const std::array<std::byte, 1U> oddBfloat16 = {std::byte{0}};
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::rawRecord(0U, oddBfloat16)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::rawRecord(1U, oddBfloat16)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::nodeMappingKey(0U)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableVectorSemanticCodecTest, ReportsLogicalWriteInputErrorsWithoutWeakeningFormatValidation) {
	const PropertyValue wrongShape(std::vector<PropertyValue>{PropertyValue(1.0)});
	const PropertyValue nonNumeric(std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue("bad")});
	const PropertyValue nonFinite(
			std::vector<PropertyValue>{PropertyValue(1.0), PropertyValue(std::numeric_limits<double>::infinity())});

	EXPECT_THROW(static_cast<void>(VectorSemanticCodec::encodeBFloat16ForLogicalWrite(wrongShape, 2U)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(VectorSemanticCodec::encodeBFloat16ForLogicalWrite(nonNumeric, 2U)),
				 std::invalid_argument);
	EXPECT_THROW(static_cast<void>(VectorSemanticCodec::encodeBFloat16ForLogicalWrite(nonFinite, 2U)),
				 std::invalid_argument);
}

TEST(RelocatableVectorSemanticCodecTest, ValidatesCanonicalBfloat16CosineNorms) {
	const PropertyValue nonzero(
			std::vector<PropertyValue>{PropertyValue(3.0), PropertyValue(-4.0)});
	const PropertyValue zero(
			std::vector<PropertyValue>{PropertyValue(0.0), PropertyValue(0.0)});
	EXPECT_TRUE(VectorSemanticCodec::hasNonZeroFiniteL2Norm(VectorSemanticCodec::encodeBFloat16(nonzero, 2U)));
	EXPECT_FALSE(VectorSemanticCodec::hasNonZeroFiniteL2Norm(VectorSemanticCodec::encodeBFloat16(zero, 2U)));

	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::hasNonZeroFiniteL2Norm({})); },
				  FormatError::FERR_INVALID_HEADER);
	const std::array<std::byte, 1U> malformed = {std::byte{0}};
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::hasNonZeroFiniteL2Norm(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	std::array<std::byte, sizeof(uint16_t)> nonFinite{};
	LittleEndianCodec::writeU16(nonFinite, 0U, UINT16_C(0x7F80));
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::hasNonZeroFiniteL2Norm(nonFinite)); },
				  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableVectorSemanticCodecTest, MapsEverySupportedMetricAndRejectsWrongDefinitions) {
	EXPECT_EQ(VectorSemanticCodec::metricFor(vectorDefinition("l2")), VectorDistanceMetric::VDM_L2);
	EXPECT_EQ(VectorSemanticCodec::metricFor(vectorDefinition("ip")), VectorDistanceMetric::VDM_INNER_PRODUCT);
	EXPECT_EQ(VectorSemanticCodec::metricFor(vectorDefinition("cosine")), VectorDistanceMetric::VDM_COSINE);
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::metricFor(vectorDefinition("invalid"))); },
					  FormatError::FERR_INVALID_HEADER);
	IndexDefinition nonVector = vectorDefinition("l2");
	nonVector.kind = IndexDefinitionKind::IDK_PROPERTY;
	expectFormatError([&] { static_cast<void>(VectorSemanticCodec::metricFor(nonVector)); },
					  FormatError::FERR_INVALID_HEADER);
}
