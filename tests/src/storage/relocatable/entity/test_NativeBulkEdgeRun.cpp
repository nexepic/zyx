/**
 * @file test_NativeBulkEdgeRun.cpp
 * @brief Bounded private native bulk Edge run codec tests.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include <algorithm>
#include <functional>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/NativeBulkEdgeRun.hpp"
#include "graph/storage/relocatable/format/Format.hpp"

using namespace graph;
using namespace graph::storage;

namespace {

constexpr size_t RECORD_COUNT_OFFSET = 12U;
constexpr size_t HEADER_CRC_OFFSET = 32U;

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

} // namespace

TEST(NativeBulkEdgeRunCodecTest, RoundTripsAnOrderedMultiRecordRunWithExactSizing) {
	const NativeBulkEdgeRun expected = run();
	const std::vector<std::byte> encoded = NativeBulkEdgeRunCodec::encode(expected);
	EXPECT_EQ(NativeBulkEdgeRunCodec::encodedSize(expected), encoded.size());
	EXPECT_EQ(NativeBulkEdgeRunCodec::decode(encoded), expected);
}

TEST(NativeBulkEdgeRunCodecTest, RejectsEmptyAndNoncanonicalInputBeforePrivateStaging) {
	expectFormatError([&] { static_cast<void>(NativeBulkEdgeRunCodec::encode({})); }, FormatError::FERR_INVALID_HEADER);
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
