/**
 * @file test_NativeBulkEdgeRecord.cpp
 * @brief Canonical transaction-private native bulk Edge record tests.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <functional>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgeRecord.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph;
using namespace graph::storage;

namespace {

constexpr size_t HEADER_CRC_OFFSET = 52U;
constexpr size_t PROPERTY_COUNT_OFFSET = 40U;

void expectFormatError(const std::function<void()> &operation, FormatError expected) {
	try {
		operation();
		FAIL() << "Expected FormatException";
	} catch (const FormatException &error) {
		EXPECT_EQ(error.error(), expected) << error.what();
	}
}

[[nodiscard]] uint32_t crc(std::span<const std::byte> bytes) {
	const std::byte empty{};
	return utils::calculateCrc(bytes.empty() ? &empty : bytes.data(), bytes.size());
}

void rewriteHeaderCrc(std::vector<std::byte> &bytes) {
	ASSERT_GE(bytes.size(), RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES);
	LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0U);
	LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET,
							crc(std::span<const std::byte>(bytes).first(RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES)));
}

[[nodiscard]] NativeBulkEdgeRecord record() {
	NativeBulkEdgeRecord result;
	result.edge_id = 91U;
	result.source_node_id = 17U;
	result.target_node_id = 23U;
	result.type = "RELATES_TO";
	PropertyValue::MapType details;
	details.emplace("first", PropertyValue(int64_t(-7)));
	details.emplace("bytes", PropertyValue(PropertyValue::Bytes{std::byte{0x00}, std::byte{0x80}}));
	result.properties = {
			{"z", PropertyValue(TemporalDuration{-2, 3, -4})},
			{"details", PropertyValue(std::move(details))},
			{"a", PropertyValue(std::vector<PropertyValue>{PropertyValue(true), PropertyValue("nested")})},
	};
	return result;
}

} // namespace

TEST(NativeBulkEdgeRecordCodecTest, CanonicalizesPropertyOrderAndRoundTripsEveryPersistedValueFamily) {
	const NativeBulkEdgeRecord source = record();
	const std::vector<std::byte> encoded = NativeBulkEdgeRecordCodec::encode(source);

	EXPECT_EQ(NativeBulkEdgeRecordCodec::encodedSize(source), encoded.size());
	NativeBulkEdgeRecord expected = source;
	std::ranges::sort(expected.properties, [](const auto &left, const auto &right) { return left.key < right.key; });
	EXPECT_EQ(NativeBulkEdgeRecordCodec::decode(encoded), expected);

	NativeBulkEdgeRecord reordered = source;
	std::swap(reordered.properties.front(), reordered.properties.back());
	EXPECT_EQ(NativeBulkEdgeRecordCodec::encode(reordered), encoded);
	for (const auto &property: source.properties) {
		std::vector<std::byte> value;
		CanonicalPropertyValueCodec::append(value, property.value);
		EXPECT_EQ(CanonicalPropertyValueCodec::encodedSize(property.value), value.size());
	}
}

TEST(NativeBulkEdgeRecordCodecTest, PreservesAnUntypedLogicalRelationship) {
	NativeBulkEdgeRecord untyped = record();
	untyped.type.clear();
	untyped.properties.clear();

	const auto decoded = NativeBulkEdgeRecordCodec::decode(NativeBulkEdgeRecordCodec::encode(untyped));
	EXPECT_EQ(decoded, untyped);
}

TEST(NativeBulkEdgeRecordCodecTest, RejectsInvalidLogicalIdentitiesAndPropertyKeysBeforeScratchWrite) {
	NativeBulkEdgeRecord invalid = record();
	invalid.edge_id = 0U;
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRecordCodec::encodedSize(invalid)); },
					  FormatError::FERR_INVALID_HEADER);

	invalid = record();
	invalid.properties.push_back({"a", PropertyValue(int64_t(1))});
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRecordCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);

	invalid = record();
	invalid.properties.front().key.clear();
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRecordCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkEdgeRecordCodecTest, FailsClosedForHeaderPayloadAndCanonicalDimensionCorruption) {
	const std::vector<std::byte> encoded = NativeBulkEdgeRecordCodec::encode(record());

	auto corruptHeader = encoded;
	corruptHeader[0U] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRecordCodec::decode(corruptHeader)); },
					  FormatError::FERR_INVALID_HEADER);

	auto corruptHeaderChecksum = encoded;
	corruptHeaderChecksum[HEADER_CRC_OFFSET] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRecordCodec::decode(corruptHeaderChecksum)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	auto corruptPayload = encoded;
	corruptPayload.back() ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRecordCodec::decode(corruptPayload)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	auto wrongPropertyCount = encoded;
	LittleEndianCodec::writeU32(wrongPropertyCount, PROPERTY_COUNT_OFFSET, 0U);
	rewriteHeaderCrc(wrongPropertyCount);
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRecordCodec::decode(wrongPropertyCount)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> truncated(encoded.begin(), encoded.end() - 1U);
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRecordCodec::decode(truncated)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkEdgeRecordCodecTest, RejectsEverySingleBitCorruptionOfACanonicalWireImage) {
	const std::vector<std::byte> encoded = NativeBulkEdgeRecordCodec::encode(record());
	for (size_t byte = 0U; byte < encoded.size(); ++byte) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			SCOPED_TRACE(::testing::Message() << "byte=" << byte << ", bit=" << static_cast<unsigned>(bit));
			auto corrupt = encoded;
			corrupt[byte] ^= std::byte{static_cast<unsigned char>(1U << bit)};
			EXPECT_THROW(static_cast<void>(NativeBulkEdgeRecordCodec::decode(corrupt)), FormatException);
		}
	}
}
