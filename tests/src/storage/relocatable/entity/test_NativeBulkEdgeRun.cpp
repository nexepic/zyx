/**
 * @file test_NativeBulkEdgeRun.cpp
 * @brief Bounded private native bulk Edge run codec tests.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <functional>
#include <limits>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgeRun.hpp"
#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph;
using namespace graph::storage;

namespace {

constexpr size_t RECORD_COUNT_OFFSET = 12U;
constexpr size_t RECORD_BYTES_OFFSET = 16U;
constexpr size_t RECORDS_CRC_OFFSET = 20U;
constexpr size_t LOGICAL_BYTES_OFFSET = 24U;
constexpr size_t HEADER_CRC_OFFSET = 32U;
constexpr size_t RECORDS_OFFSET = RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES;
constexpr size_t RECORD_LENGTH_OFFSET = RECORDS_OFFSET;
constexpr size_t RECORD_CRC_OFFSET = RECORDS_OFFSET + sizeof(uint32_t);

void refreshHeaderChecksum(std::vector<std::byte> &bytes) {
	LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0U);
	const uint32_t checksum = graph::utils::calculateCrc(bytes.data(), RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES);
	LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, checksum);
}

void refreshAggregateChecksum(std::vector<std::byte> &bytes) {
	const std::span<const std::byte> records =
			std::span<const std::byte>(bytes).subspan(RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES);
	LittleEndianCodec::writeU32(bytes, RECORDS_CRC_OFFSET,
								graph::utils::calculateCrc(records.data(), records.size()));
	}

void expectFormatError(const std::function<void()> &operation, FormatError expected) {
	try {
		operation();
		FAIL() << "Expected FormatException";
	} catch (const FormatException &error) {
		EXPECT_EQ(error.error(), expected) << error.what();
	}
}

[[nodiscard]] NativeBulkEdgeRecord edge(uint64_t id) {
	NativeBulkEdgeRecord record;
	record.edge_id = id;
	record.source_node_id = id + 10U;
	record.target_node_id = id + 20U;
	record.type = "T";
	record.properties = {{"value", PropertyValue(static_cast<int64_t>(id))}};
	return record;
}

[[nodiscard]] NativeBulkEdgeRun run() {
	return {{edge(7U), edge(11U), edge(19U)}};
}

[[nodiscard]] NativeBulkEdgeRun uniformRun() {
	return {{edge(100U), edge(101U), edge(102U)}};
}

} // namespace

TEST(NativeBulkEdgeRunCodecTest, RoundTripsAnOrderedMultiRecordRunWithExactSizing) {
	const NativeBulkEdgeRun expected = run();
	const std::vector<std::byte> encoded = NativeBulkEdgeRunCodec::encode(expected);
	EXPECT_EQ(NativeBulkEdgeRunCodec::encodedSize(expected), encoded.size());
	EXPECT_EQ(NativeBulkEdgeRunCodec::decode(encoded), expected);
}

TEST(NativeBulkEdgeRunCodecTest, RejectsEmptyAndNoncanonicalInputBeforePrivateStaging) {
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::encode({})); }, FormatError::FERR_INVALID_HEADER);
	NativeBulkEdgeRun zeroId = run();
	zeroId.records.front().edge_id = 0U;
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::encode(zeroId)); },
				  FormatError::FERR_INVALID_HEADER);
	NativeBulkEdgeRun unordered = run();
	std::swap(unordered.records[0U], unordered.records[1U]);
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::encodedSize(unordered)); },
					  FormatError::FERR_INVALID_HEADER);
	NativeBulkEdgeRun duplicate = run();
	duplicate.records[1U].edge_id = duplicate.records[0U].edge_id;
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::encode(duplicate)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkEdgeRunCodecTest, RejectsHeaderAggregateRecordAndDimensionCorruption) {
	const std::vector<std::byte> encoded = NativeBulkEdgeRunCodec::encode(run());
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::decode({})); }, FormatError::FERR_INVALID_HEADER);

	auto corruptHeader = encoded;
	corruptHeader[HEADER_CRC_OFFSET] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::decode(corruptHeader)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	auto corruptAggregate = encoded;
	corruptAggregate.back() ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::decode(corruptAggregate)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	auto corruptCount = encoded;
	LittleEndianCodec::writeU32(corruptCount, RECORD_COUNT_OFFSET, 4U);
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::decode(corruptCount)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	std::vector<std::byte> truncated(encoded.begin(), encoded.end() - 1U);
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::decode(truncated)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkEdgeRunCodecTest, RejectsEverySingleBitWireCorruption) {
	const std::vector<std::byte> encoded = NativeBulkEdgeRunCodec::encode(run());
	for (size_t offset = 0U; offset < encoded.size(); ++offset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto corrupted = encoded;
			corrupted[offset] ^= static_cast<std::byte>(uint8_t{1U} << bit);
			EXPECT_THROW(static_cast<void>(NativeBulkEdgeRunCodec::decode(corrupted)), FormatException)
					<< "wire offset " << offset << ", bit " << static_cast<unsigned int>(bit);
		}
	}
}

TEST(NativeBulkEdgeRunCodecTest, RejectsMalformedRecordFramingAfterHeaderValidation) {
	const std::vector<std::byte> encoded = NativeBulkEdgeRunCodec::encode(run());

	// Four declared records leave the dimensions check valid, but the fourth
	// frame is absent and must fail at the record-framing boundary.
	auto missingFrame = encoded;
	LittleEndianCodec::writeU32(missingFrame, RECORD_COUNT_OFFSET, 4U);
	refreshHeaderChecksum(missingFrame);
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::decode(missingFrame)); },
					  FormatError::FERR_BUFFER_TOO_SMALL);

	auto zeroLength = encoded;
	LittleEndianCodec::writeU32(zeroLength, RECORD_LENGTH_OFFSET, 0U);
	refreshAggregateChecksum(zeroLength);
	refreshHeaderChecksum(zeroLength);
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::decode(zeroLength)); },
					  FormatError::FERR_INVALID_HEADER);

	auto oversizedLength = encoded;
	LittleEndianCodec::writeU32(oversizedLength, RECORD_LENGTH_OFFSET, std::numeric_limits<uint32_t>::max());
	refreshAggregateChecksum(oversizedLength);
	refreshHeaderChecksum(oversizedLength);
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::decode(oversizedLength)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkEdgeRunCodecTest, RejectsRecordChecksumAndLogicalByteMismatches) {
	const std::vector<std::byte> encoded = NativeBulkEdgeRunCodec::encode(run());

	auto corruptRecordChecksum = encoded;
	corruptRecordChecksum[RECORD_CRC_OFFSET] ^= std::byte{0x01};
	refreshAggregateChecksum(corruptRecordChecksum);
	refreshHeaderChecksum(corruptRecordChecksum);
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::decode(corruptRecordChecksum)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	auto inconsistentLogicalBytes = encoded;
	LittleEndianCodec::writeU64(inconsistentLogicalBytes, LOGICAL_BYTES_OFFSET, 0U);
	refreshHeaderChecksum(inconsistentLogicalBytes);
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::decode(inconsistentLogicalBytes)); },
					  FormatError::FERR_INVALID_HEADER);

	// Keep the record aggregate valid while declaring one extra byte in the
	// framing field; decoding must reject the resulting trailing bytes.
	auto inconsistentRecordBytes = encoded;
	LittleEndianCodec::writeU32(inconsistentRecordBytes, RECORD_BYTES_OFFSET,
								LittleEndianCodec::readU32(encoded, RECORD_BYTES_OFFSET) + 1U);
	refreshHeaderChecksum(inconsistentRecordBytes);
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::decode(inconsistentRecordBytes)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(NativeBulkEdgeRunCodecTest, RejectsAValidlyFramedButNonIncreasingDecodedRecord) {
	const std::vector<std::byte> encoded = NativeBulkEdgeRunCodec::encode(uniformRun());
	auto duplicate = encoded;
	const uint32_t firstLength = LittleEndianCodec::readU32(encoded, RECORD_LENGTH_OFFSET);
	const size_t secondFrame = RECORDS_OFFSET + sizeof(uint32_t) + sizeof(uint32_t) + firstLength;
	const uint32_t secondLength = LittleEndianCodec::readU32(encoded, secondFrame);
	ASSERT_EQ(firstLength, secondLength);
	const size_t firstPayload = RECORDS_OFFSET + sizeof(uint32_t) + sizeof(uint32_t);
	const size_t secondPayload = secondFrame + sizeof(uint32_t) + sizeof(uint32_t);
	std::copy_n(encoded.begin() + static_cast<std::ptrdiff_t>(firstPayload), firstLength,
				duplicate.begin() + static_cast<std::ptrdiff_t>(secondPayload));
	LittleEndianCodec::writeU32(duplicate, secondFrame + sizeof(uint32_t),
								graph::utils::calculateCrc(duplicate.data() + secondPayload, secondLength));
	refreshAggregateChecksum(duplicate);
	refreshHeaderChecksum(duplicate);
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::decode(duplicate)); },
					  FormatError::FERR_INVALID_HEADER);
}
