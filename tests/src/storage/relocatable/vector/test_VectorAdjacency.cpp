/**
 * @file test_VectorAdjacency.cpp
 * @brief Checksummed relocatable DiskANN adjacency-record codec tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <functional>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/vector/VectorAdjacency.hpp"
#include "graph/utils/ChecksumUtils.hpp"

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

	VectorAdjacencyRecord record() { return {17, {3, 5, 9}}; }

	void recalculateChecksums(std::vector<std::byte> &bytes) {
		LittleEndianCodec::writeU32(bytes, 20, graph::utils::calculateCrc(bytes.data() + 32, bytes.size() - 32));
		LittleEndianCodec::writeU32(bytes, 24, 0);
		LittleEndianCodec::writeU32(bytes, 24, graph::utils::calculateCrc(bytes.data(), 32));
	}

} // namespace

TEST(RelocatableVectorAdjacencyTest, EncodesEmptyAndOrderedNeighborListsExplicitly) {
	const VectorAdjacencyRecord expected = record();
	const std::vector<std::byte> encoded = VectorAdjacencyCodec::encode(expected);
	EXPECT_EQ(encoded.size(), 32U + expected.neighbors.size() * 8U);
	EXPECT_EQ(LittleEndianCodec::readU32(encoded, 0), RSS_VECTOR_ADJACENCY_MAGIC);
	EXPECT_EQ(LittleEndianCodec::readU16(encoded, 4), RSS_VECTOR_ADJACENCY_VERSION);
	EXPECT_EQ(VectorAdjacencyCodec::decode(encoded), expected);

	VectorAdjacencyRecord empty{17, {}};
	EXPECT_EQ(VectorAdjacencyCodec::decode(VectorAdjacencyCodec::encode(empty)), empty);
}

TEST(RelocatableVectorAdjacencyTest, RejectsInvalidIdentityOrderingAndOversizedLists) {
	VectorAdjacencyRecord invalid = record();
	invalid.vector_id = 0;
	expectFormatError([&] { static_cast<void>(VectorAdjacencyCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = record();
	invalid.neighbors = {3, 3};
	expectFormatError([&] { static_cast<void>(VectorAdjacencyCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = record();
	invalid.neighbors = {3, invalid.vector_id};
	expectFormatError([&] { static_cast<void>(VectorAdjacencyCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = record();
	invalid.neighbors.assign(RSS_VECTOR_ADJACENCY_MAX_NEIGHBORS + 1U, 1U);
	expectFormatError([&] { static_cast<void>(VectorAdjacencyCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableVectorAdjacencyTest, FailsClosedForCorruptVariableLengthRecords) {
	const std::vector<std::byte> encoded = VectorAdjacencyCodec::encode(record());
	std::vector<std::byte> corrupt = encoded;
	corrupt.front() ^= std::byte{1};
	expectFormatError([&] { static_cast<void>(VectorAdjacencyCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	corrupt.back() ^= std::byte{1};
	expectFormatError([&] { static_cast<void>(VectorAdjacencyCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 8, 4);
	recalculateChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(VectorAdjacencyCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 10, 1);
	recalculateChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(VectorAdjacencyCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, 32 + 8, 3);
	recalculateChecksums(corrupt);
	expectFormatError([&] { static_cast<void>(VectorAdjacencyCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	expectFormatError([&] { static_cast<void>(VectorAdjacencyCodec::decode(std::span(encoded).first(31U))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
}
