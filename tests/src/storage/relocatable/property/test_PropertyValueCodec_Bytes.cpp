/**
 * @file test_PropertyValueCodec_Bytes.cpp
 * @brief Canonical native PropertyValue binary wire tests.
 */

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"

using graph::PropertyValue;
using graph::TemporalDate;
using graph::TemporalDateTime;
using graph::TemporalDuration;
using graph::storage::CanonicalPropertyValueCodec;
using graph::storage::FormatError;
using graph::storage::FormatException;

namespace {

void expectFormatError(const std::function<void()> &operation, FormatError expected) {
	try {
		operation();
		FAIL() << "Expected FormatException";
	} catch (const FormatException &error) {
		EXPECT_EQ(error.error(), expected);
	}
}

PropertyValue::Bytes bytes() {
	return {std::byte{0x00}, std::byte{0x7F}, std::byte{0x80}, std::byte{0xFF}};
}

void appendU32(std::vector<std::byte> &bytes, uint32_t value) {
	for (size_t shift = 0U; shift < sizeof(value); ++shift) {
		bytes.push_back(static_cast<std::byte>(value >> (shift * 8U)));
	}
}

void writeU32(std::vector<std::byte> &bytes, size_t offset, uint32_t value) {
	ASSERT_LE(offset + sizeof(value), bytes.size());
	for (size_t shift = 0U; shift < sizeof(value); ++shift) {
		bytes[offset + shift] = static_cast<std::byte>(value >> (shift * 8U));
	}
}

std::vector<std::byte> wrapAsSingletonList(std::span<const std::byte> child) {
	std::vector<std::byte> payload;
	appendU32(payload, 1U);
	payload.insert(payload.end(), child.begin(), child.end());

	std::vector<std::byte> result;
	result.push_back(std::byte{6U});
	result.push_back(std::byte{0U});
	appendU32(result, static_cast<uint32_t>(payload.size()));
	result.insert(result.end(), payload.begin(), payload.end());
	return result;
}

} // namespace

TEST(RelocatablePropertyValueCodecBytesTest, RoundTripsEmptyAndBinaryPayloads) {
	for (const PropertyValue &value : {PropertyValue(PropertyValue::Bytes{}), PropertyValue(bytes())}) {
		std::vector<std::byte> encoded;
		CanonicalPropertyValueCodec::append(encoded, value);
		size_t offset = 0U;
		EXPECT_EQ(CanonicalPropertyValueCodec::decodeNext(encoded, offset), value);
		EXPECT_EQ(offset, encoded.size());
	}
}

TEST(RelocatablePropertyValueCodecBytesTest, RoundTripsNestedBinaryValuesWithCanonicalMapOrder) {
	PropertyValue::MapType map;
	map.emplace("z", PropertyValue(bytes()));
	map.emplace("a", PropertyValue(PropertyValue::Bytes{std::byte{0x01}}));
	const PropertyValue expected(std::vector<PropertyValue>{PropertyValue(std::move(map)), PropertyValue(bytes())});

	std::vector<std::byte> encoded;
	CanonicalPropertyValueCodec::append(encoded, expected);
	size_t offset = 0U;
	const PropertyValue decoded = CanonicalPropertyValueCodec::decodeNext(encoded, offset);

	EXPECT_EQ(decoded, expected);
	EXPECT_EQ(offset, encoded.size());
}

TEST(RelocatablePropertyValueCodecBytesTest, RejectsTruncationFlagsAndTrailingPayload) {
	std::vector<std::byte> encoded;
	CanonicalPropertyValueCodec::append(encoded, PropertyValue(bytes()));

	for (size_t size = 0U; size < encoded.size(); ++size) {
		size_t offset = 0U;
		expectFormatError([&] { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(
				std::span<const std::byte>(encoded.data(), size), offset)); },
				 size < 2U ? FormatError::FERR_BUFFER_TOO_SMALL : FormatError::FERR_BUFFER_TOO_SMALL);
	}

	auto corrupt = encoded;
	corrupt[1] = std::byte{1};
	size_t offset = 0U;
	expectFormatError([&] { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(corrupt, offset)); },
				 FormatError::FERR_RESERVED_BITS);

	auto trailing = encoded;
	trailing.push_back(std::byte{0x42});
	offset = 0U;
	const PropertyValue decoded = CanonicalPropertyValueCodec::decodeNext(trailing, offset);
	EXPECT_EQ(decoded, PropertyValue(bytes()));
	EXPECT_EQ(offset + 1U, trailing.size());
}

TEST(RelocatablePropertyValueCodecBytesTest, RoundTripsEveryCanonicalScalarAndContainerKind) {
	PropertyValue::MapType map;
	map.emplace("z", PropertyValue(int64_t(-7)));
	map.emplace("a", PropertyValue(false));
	const std::vector<PropertyValue> expected{
			PropertyValue(std::monostate{}),
			PropertyValue(false),
			PropertyValue(true),
			PropertyValue(int64_t(-123456789)),
			PropertyValue(42.5),
			PropertyValue("canonical text"),
			PropertyValue(std::vector<PropertyValue>{PropertyValue(int64_t(1)), PropertyValue("nested")}),
			PropertyValue(std::move(map)),
			PropertyValue(TemporalDate{-17}),
			PropertyValue(TemporalDateTime{123456789}),
			PropertyValue(TemporalDuration{-2, 3, -4}),
	};

	std::vector<std::byte> encoded;
	for (const PropertyValue &value: expected) {
		CanonicalPropertyValueCodec::append(encoded, value);
	}

	size_t offset = 0U;
	for (const PropertyValue &value: expected) {
		EXPECT_EQ(CanonicalPropertyValueCodec::decodeNext(encoded, offset), value);
	}
	EXPECT_EQ(offset, encoded.size());
}

TEST(RelocatablePropertyValueCodecBytesTest, RejectsInvalidWireValuesAndExcessiveNesting) {
	std::vector<std::byte> boolean;
	CanonicalPropertyValueCodec::append(boolean, PropertyValue(false));
	boolean[6U] = std::byte{2U};
	size_t offset = 0U;
	expectFormatError([&] { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(boolean, offset)); },
				  FormatError::FERR_INVALID_ENUM);

	std::vector<std::byte> unknownKind;
	CanonicalPropertyValueCodec::append(unknownKind, PropertyValue(std::monostate{}));
	unknownKind.front() = std::byte{0xFFU};
	offset = 0U;
	expectFormatError([&] { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(unknownKind, offset)); },
				  FormatError::FERR_INVALID_ENUM);

	std::vector<std::byte> list;
	CanonicalPropertyValueCodec::append(list, PropertyValue(std::vector<PropertyValue>{}));
	list[6U] = std::byte{1U};
	offset = 0U;
	expectFormatError([&] { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(list, offset)); },
				  FormatError::FERR_INVALID_HEADER);

	PropertyValue::MapType emptyMap;
	std::vector<std::byte> mapWithInvalidCount;
	CanonicalPropertyValueCodec::append(mapWithInvalidCount, PropertyValue(std::move(emptyMap)));
	mapWithInvalidCount[6U] = std::byte{1U};
	offset = 0U;
	expectFormatError(
			[&] { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(mapWithInvalidCount, offset)); },
			FormatError::FERR_INVALID_HEADER);

	PropertyValue::MapType map;
	map.emplace("a", PropertyValue(std::monostate{}));
	map.emplace("b", PropertyValue(std::monostate{}));
	std::vector<std::byte> nonCanonicalMap;
	CanonicalPropertyValueCodec::append(nonCanonicalMap, PropertyValue(std::move(map)));
	std::swap(nonCanonicalMap[14U], nonCanonicalMap[25U]);
	offset = 0U;
	expectFormatError([&] { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(nonCanonicalMap, offset)); },
				  FormatError::FERR_INVALID_HEADER);

	const std::array<std::byte, 0U> empty{};
	offset = 1U;
	expectFormatError([&] { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(empty, offset)); },
				  FormatError::FERR_INVALID_HEADER);

	PropertyValue nested(std::monostate{});
	for (size_t depth = 0U; depth <= 64U; ++depth) {
		nested = PropertyValue(std::vector<PropertyValue>{nested});
	}
	std::vector<std::byte> ignored;
	expectFormatError([&] { CanonicalPropertyValueCodec::append(ignored, nested); },
				  FormatError::FERR_INVALID_STATE);

	std::vector<std::byte> tooDeep;
	CanonicalPropertyValueCodec::append(tooDeep, PropertyValue(std::monostate{}));
	for (size_t depth = 0U; depth <= 64U; ++depth) {
		tooDeep = wrapAsSingletonList(tooDeep);
	}
	offset = 0U;
	expectFormatError([&] { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(tooDeep, offset)); },
				  FormatError::FERR_INVALID_STATE);
}

TEST(RelocatablePropertyValueCodecBytesTest, RejectsTruncatedFixedPayloadsAndTrailingInnerPayloads) {
	std::vector<std::byte> integer;
	CanonicalPropertyValueCodec::append(integer, PropertyValue(int64_t(9)));
	writeU32(integer, 2U, 7U);
	integer.resize(6U + 7U);
	size_t offset = 0U;
	expectFormatError([&] { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(integer, offset)); },
				  FormatError::FERR_BUFFER_TOO_SMALL);

	std::vector<std::byte> string;
	CanonicalPropertyValueCodec::append(string, PropertyValue("x"));
	writeU32(string, 6U, 2U);
	offset = 0U;
	expectFormatError([&] { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(string, offset)); },
				  FormatError::FERR_BUFFER_TOO_SMALL);

	std::vector<std::byte> nullValue;
	CanonicalPropertyValueCodec::append(nullValue, PropertyValue(std::monostate{}));
	writeU32(nullValue, 2U, 1U);
	nullValue.push_back(std::byte{0U});
	offset = 0U;
	expectFormatError([&] { static_cast<void>(CanonicalPropertyValueCodec::decodeNext(nullValue, offset)); },
				  FormatError::FERR_INVALID_HEADER);
}
