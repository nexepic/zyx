/**
 * @file test_IndexKeyCodec.cpp
 * @brief Ordering and validation tests for relocatable B+Tree index keys.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <array>
#include <cmath>
#include <functional>
#include <limits>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/index/IndexKeyCodec.hpp"

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

	bool lessKey(const std::vector<std::byte> &left, const std::vector<std::byte> &right) {
		return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end());
	}

} // namespace

TEST(RelocatableIndexKeyCodecTest, PreservesScalarOrderingForAllSupportedTypes) {
	const std::vector<PropertyValue> integers = {PropertyValue(-42), PropertyValue(-1), PropertyValue(0), PropertyValue(9)};
	for (size_t index = 1; index < integers.size(); ++index) {
		EXPECT_TRUE(lessKey(IndexKeyCodec::encodeValue(integers[index - 1U]), IndexKeyCodec::encodeValue(integers[index])));
	}

	const std::vector<PropertyValue> doubles =
			{PropertyValue(-42.5), PropertyValue(-0.0), PropertyValue(0.0), PropertyValue(1.25)};
	EXPECT_EQ(IndexKeyCodec::encodeValue(doubles[1]), IndexKeyCodec::encodeValue(doubles[2]));
	EXPECT_TRUE(lessKey(IndexKeyCodec::encodeValue(doubles[0]), IndexKeyCodec::encodeValue(doubles[1])));
	EXPECT_TRUE(lessKey(IndexKeyCodec::encodeValue(doubles[2]), IndexKeyCodec::encodeValue(doubles[3])));

	const std::vector<PropertyValue> strings = {PropertyValue(""), PropertyValue(std::string("\0", 1)),
																 PropertyValue("a"), PropertyValue(std::string("a\0", 2)),
																 PropertyValue(std::string("\xFF", 1))};
	for (size_t index = 1; index < strings.size(); ++index) {
		EXPECT_TRUE(lessKey(IndexKeyCodec::encodeValue(strings[index - 1U]), IndexKeyCodec::encodeValue(strings[index])));
	}
	EXPECT_TRUE(lessKey(IndexKeyCodec::encodeValue(PropertyValue(false)), IndexKeyCodec::encodeValue(PropertyValue(true))));
	EXPECT_TRUE(lessKey(IndexKeyCodec::encodeValue(PropertyValue(true)), IndexKeyCodec::encodeValue(PropertyValue(-1))));
	EXPECT_TRUE(lessKey(IndexKeyCodec::encodeValue(PropertyValue(99)), IndexKeyCodec::encodeValue(PropertyValue(-1.0))));
	EXPECT_TRUE(lessKey(IndexKeyCodec::encodeValue(PropertyValue(99.0)), IndexKeyCodec::encodeValue(PropertyValue(""))));
}

TEST(RelocatableIndexKeyCodecTest, EncodesCompositeValuesAndStableEntityIdSuffixes) {
	const std::array first = {PropertyValue("alpha"), PropertyValue(7)};
	const std::array second = {PropertyValue("alpha"), PropertyValue(8)};
	EXPECT_TRUE(lessKey(IndexKeyCodec::encodeComposite(first), IndexKeyCodec::encodeComposite(second)));
	EXPECT_EQ(IndexKeyCodec::tryEncodeComposite(first), IndexKeyCodec::encodeComposite(first));
	EXPECT_EQ(IndexKeyCodec::tryEncodeCompositeEntryKey(first, 12U),
			  IndexKeyCodec::encodeCompositeEntryKey(first, 12U));
	const std::vector<std::byte> low = IndexKeyCodec::encodeEntryKey(PropertyValue("name"), 7);
	const std::vector<std::byte> high = IndexKeyCodec::encodeEntryKey(PropertyValue("name"), 9);
	EXPECT_TRUE(lessKey(low, high));
	EXPECT_EQ(IndexKeyCodec::decodeEntryEntityId(low), 7U);
	EXPECT_EQ(IndexKeyCodec::decodeEntryEntityId(IndexKeyCodec::encodeCompositeEntryKey(first, 12)), 12U);
	const std::vector<std::byte> lower = IndexKeyCodec::lowerBound(PropertyValue("name"));
	const std::vector<std::byte> upper = IndexKeyCodec::upperBound(PropertyValue("name"));
	EXPECT_TRUE(lessKey(lower, low));
	EXPECT_TRUE(lessKey(high, upper));
}

TEST(RelocatableIndexKeyCodecTest, RejectsUnsupportedOrNonCanonicalInputs) {
	expectFormatError([&] { static_cast<void>(IndexKeyCodec::encodeValue(PropertyValue{})); }, FormatError::FERR_INVALID_ENUM);
	expectFormatError(
			[&] { static_cast<void>(IndexKeyCodec::encodeValue(PropertyValue(std::vector<PropertyValue>{PropertyValue(1)}))); },
			FormatError::FERR_INVALID_ENUM);
	expectFormatError([&] { static_cast<void>(IndexKeyCodec::encodeValue(PropertyValue(PropertyValue::MapType{}))); },
				  FormatError::FERR_INVALID_ENUM);
	expectFormatError([&] { static_cast<void>(IndexKeyCodec::encodeValue(PropertyValue(std::numeric_limits<double>::quiet_NaN()))); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(IndexKeyCodec::encodeValue(PropertyValue(std::numeric_limits<double>::infinity()))); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(IndexKeyCodec::encodeEntryKey(PropertyValue(1), 0)); },
				  FormatError::FERR_INVALID_HEADER);
	const std::array<PropertyValue, 0> empty{};
	expectFormatError([&] { static_cast<void>(IndexKeyCodec::encodeComposite(empty)); }, FormatError::FERR_INVALID_HEADER);
	std::vector<PropertyValue> tooMany(RSS_INDEX_KEY_MAX_COMPONENTS + 1U, PropertyValue(1));
	expectFormatError([&] { static_cast<void>(IndexKeyCodec::encodeComposite(tooMany)); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(IndexKeyCodec::decodeEntryEntityId({})); }, FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> zeroSuffix(9U, std::byte{});
	expectFormatError([&] { static_cast<void>(IndexKeyCodec::decodeEntryEntityId(zeroSuffix)); },
				  FormatError::FERR_INVALID_HEADER);
	std::string oversized(RSS_INDEX_KEY_MAX_BYTES, 'x');
	expectFormatError([&] { static_cast<void>(IndexKeyCodec::encodeValue(PropertyValue(oversized))); },
				  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(IndexKeyCodec::encodeComposite(std::array{PropertyValue(oversized)})); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(IndexKeyCodec::encodeEntryKey(PropertyValue(oversized), 1U)); },
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(IndexKeyCodec::encodeCompositeEntryKey(std::array{PropertyValue(oversized)}, 1U));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(IndexKeyCodec::lowerBound(PropertyValue(oversized))); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] { static_cast<void>(IndexKeyCodec::upperBound(PropertyValue(oversized))); }, FormatError::FERR_INVALID_HEADER);
	EXPECT_FALSE(IndexKeyCodec::tryEncodeEntryKey(PropertyValue(oversized), 1U).has_value());
	EXPECT_FALSE(IndexKeyCodec::tryEncodeComposite(std::array{PropertyValue(oversized)}).has_value());
	EXPECT_FALSE(IndexKeyCodec::tryEncodeCompositeEntryKey(std::array{PropertyValue(oversized)}, 1U).has_value());
	expectFormatError(
			[&] { static_cast<void>(IndexKeyCodec::tryEncodeComposite(std::array<PropertyValue, 0>{})); },
			FormatError::FERR_INVALID_HEADER);
}
