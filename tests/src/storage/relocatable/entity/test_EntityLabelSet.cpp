/**
 * @file test_EntityLabelSet.cpp
 * @brief Tests for canonical overflow Node label-set wire records.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <algorithm>
#include <stdexcept>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <numeric>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/entity/EntityLabelSet.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t MAGIC_OFFSET = 0U;
	constexpr size_t VERSION_OFFSET = 4U;
	constexpr size_t HEADER_BYTES_OFFSET = 6U;
	constexpr size_t LABEL_COUNT_OFFSET = 8U;
	constexpr size_t PAYLOAD_BYTES_OFFSET = 12U;
	constexpr size_t FLAGS_OFFSET = 16U;
	constexpr size_t CHECKSUM_OFFSET = 20U;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	void reseal(std::vector<std::byte> &bytes) {
		LittleEndianCodec::writeU32(bytes, CHECKSUM_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, CHECKSUM_OFFSET, graph::utils::calculateCrc(bytes.data(), bytes.size()));
	}

} // namespace

TEST(RelocatableEntityLabelSetTest, RoundTripsCanonicalNonzeroSortedLabels) {
	const std::array<uint64_t, 4U> labels{4U, 12U, 90U, 1234U};
	const std::vector<std::byte> bytes = EntityLabelSetCodec::encode(labels);
	EXPECT_EQ(bytes.size(), RSS_ENTITY_LABEL_SET_HEADER_BYTES + labels.size() * sizeof(uint64_t));
	EXPECT_EQ(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET), RSS_ENTITY_LABEL_SET_MAGIC);
	EXPECT_EQ(LittleEndianCodec::readU16(bytes, VERSION_OFFSET), RSS_ENTITY_LABEL_SET_VERSION);
	EXPECT_EQ(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET), RSS_ENTITY_LABEL_SET_HEADER_BYTES);
	EXPECT_EQ(LittleEndianCodec::readU32(bytes, LABEL_COUNT_OFFSET), labels.size());
	EXPECT_EQ(LittleEndianCodec::readU32(bytes, PAYLOAD_BYTES_OFFSET), labels.size() * sizeof(uint64_t));
	EXPECT_EQ(LittleEndianCodec::readU32(bytes, FLAGS_OFFSET), 0U);
	EXPECT_EQ(EntityLabelSetCodec::decode(bytes), std::vector<uint64_t>(labels.begin(), labels.end()));
	const auto alignRecordBytes = &alignEntityLabelSetRecordBytes;
	EXPECT_EQ(alignRecordBytes(bytes.size() - 1U), bytes.size());
}

TEST(RelocatableEntityLabelSetTest, RejectsEmptyZeroUnorderedAndDuplicateLabels) {
	const std::array<uint64_t, 0U> empty{};
	const std::array<uint64_t, 2U> zero{0U, 2U};
	const std::array<uint64_t, 2U> unordered{8U, 2U};
	const std::array<uint64_t, 2U> duplicate{8U, 8U};
	for (const auto labels: {std::span<const uint64_t>(empty), std::span<const uint64_t>(zero),
							std::span<const uint64_t>(unordered), std::span<const uint64_t>(duplicate)}) {
		expectFormatError([labels] { static_cast<void>(EntityLabelSetCodec::encode(labels)); },
						  FormatError::FERR_INVALID_HEADER);
	}
}

TEST(RelocatableEntityLabelSetTest, RejectsTruncationChecksumAndStaticWireCorruption) {
	const std::array<uint64_t, 2U> labels{7U, 9U};
	const std::vector<std::byte> clean = EntityLabelSetCodec::encode(labels);
	std::vector<std::byte> truncated(clean.begin(), clean.begin() + RSS_ENTITY_LABEL_SET_HEADER_BYTES - 1U);
	expectFormatError([&] { static_cast<void>(EntityLabelSetCodec::decode(truncated)); },
					  FormatError::FERR_BUFFER_TOO_SMALL);

	std::vector<std::byte> checksumCorrupt = clean;
	checksumCorrupt.back() ^= std::byte{0x80};
	expectFormatError([&] { static_cast<void>(EntityLabelSetCodec::decode(checksumCorrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	std::vector<std::byte> invalidMagic = clean;
	LittleEndianCodec::writeU32(invalidMagic, MAGIC_OFFSET, 0U);
	reseal(invalidMagic);
	expectFormatError([&] { static_cast<void>(EntityLabelSetCodec::decode(invalidMagic)); },
					  FormatError::FERR_INVALID_MAGIC);

	std::vector<std::byte> unsupportedVersion = clean;
	LittleEndianCodec::writeU16(unsupportedVersion, VERSION_OFFSET, RSS_ENTITY_LABEL_SET_VERSION + 1U);
	reseal(unsupportedVersion);
	expectFormatError([&] { static_cast<void>(EntityLabelSetCodec::decode(unsupportedVersion)); },
					  FormatError::FERR_UNSUPPORTED_VERSION);
}

TEST(RelocatableEntityLabelSetTest, RejectsBoundsReservedAndNoncanonicalDecodedLabels) {
	const std::array<uint64_t, 2U> labels{7U, 9U};
	const std::vector<std::byte> clean = EntityLabelSetCodec::encode(labels);

	std::vector<std::byte> invalidHeaderBytes = clean;
	LittleEndianCodec::writeU16(invalidHeaderBytes, HEADER_BYTES_OFFSET, 0U);
	reseal(invalidHeaderBytes);
	expectFormatError([&] { static_cast<void>(EntityLabelSetCodec::decode(invalidHeaderBytes)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> invalidCount = clean;
	LittleEndianCodec::writeU32(invalidCount, LABEL_COUNT_OFFSET, 3U);
	reseal(invalidCount);
	expectFormatError([&] { static_cast<void>(EntityLabelSetCodec::decode(invalidCount)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> invalidPayloadBytes = clean;
	LittleEndianCodec::writeU32(invalidPayloadBytes, PAYLOAD_BYTES_OFFSET, 0U);
	reseal(invalidPayloadBytes);
	expectFormatError([&] { static_cast<void>(EntityLabelSetCodec::decode(invalidPayloadBytes)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> invalidFlags = clean;
	LittleEndianCodec::writeU32(invalidFlags, FLAGS_OFFSET, 1U);
	reseal(invalidFlags);
	expectFormatError([&] { static_cast<void>(EntityLabelSetCodec::decode(invalidFlags)); },
					  FormatError::FERR_INVALID_HEADER);

	std::vector<std::byte> duplicate = clean;
	LittleEndianCodec::writeU64(duplicate, RSS_ENTITY_LABEL_SET_HEADER_BYTES + sizeof(uint64_t), 7U);
	reseal(duplicate);
	expectFormatError([&] { static_cast<void>(EntityLabelSetCodec::decode(duplicate)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableEntityLabelSetTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	const std::array<uint64_t, 4U> labels{7U, 19U, 41U, 89U};
	const std::vector<std::byte> encoded = EntityLabelSetCodec::encode(labels);
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
			reseal(mutation);
			try {
				const std::vector<uint64_t> decoded = EntityLabelSetCodec::decode(mutation);
				if (EntityLabelSetCodec::encode(decoded) != mutation) {
					ADD_FAILURE() << "Entity LabelSet mutation was silently normalized at byte " << byteOffset
								  << ", bit " << static_cast<unsigned>(bit);
					return;
				}
			} catch (const FormatException &) {
				// Every non-canonical mutation must fail at the typed LabelSet boundary.
			}
		}
	}
}

TEST(RelocatableEntityLabelSetTest, EnforcesDerivedSinglePageCapacityBeforeEncoding) {
	std::vector<uint64_t> maximum(RSS_ENTITY_LABEL_SET_MAX_OVERFLOW_LABELS);
	std::iota(maximum.begin(), maximum.end(), uint64_t{1U});
	const std::vector<std::byte> encoded = EntityLabelSetCodec::encode(maximum);
	EXPECT_EQ(encoded.size(), RSS_ENTITY_LABEL_SET_MAX_RECORD_BYTES);
	EXPECT_EQ(EntityLabelSetCodec::decode(encoded), maximum);

	maximum.push_back(static_cast<uint64_t>(maximum.size()) + 1U);
	expectFormatError([&] { static_cast<void>(EntityLabelSetCodec::encode(maximum)); },
					  FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableEntityLabelSetTest, RejectsOversizedDecodedRecordFromDerivedCapacity) {
	std::vector<uint64_t> maximum(RSS_ENTITY_LABEL_SET_MAX_OVERFLOW_LABELS);
	std::iota(maximum.begin(), maximum.end(), uint64_t{1U});
	std::vector<std::byte> oversized = EntityLabelSetCodec::encode(maximum);
	oversized.resize(oversized.size() + sizeof(uint64_t));
	LittleEndianCodec::writeU32(oversized, LABEL_COUNT_OFFSET,
							static_cast<uint32_t>(RSS_ENTITY_LABEL_SET_MAX_OVERFLOW_LABELS + 1U));
	LittleEndianCodec::writeU32(oversized, PAYLOAD_BYTES_OFFSET,
							static_cast<uint32_t>((RSS_ENTITY_LABEL_SET_MAX_OVERFLOW_LABELS + 1U) * sizeof(uint64_t)));
	LittleEndianCodec::writeU64(oversized, RSS_ENTITY_LABEL_SET_MAX_RECORD_BYTES,
							static_cast<uint64_t>(RSS_ENTITY_LABEL_SET_MAX_OVERFLOW_LABELS + 1U));
	reseal(oversized);
	expectFormatError([&] { static_cast<void>(EntityLabelSetCodec::decode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableEntityLabelSetTest, AdmitsChecksumAndResultBeforeAllocationAndRetries) {
	const std::array<uint64_t, 4U> labels{3U, 7U, 11U, 17U};
	const auto bytes = EntityLabelSetCodec::encode(labels);
	size_t requested = 0U;
	EXPECT_THROW(static_cast<void>(EntityLabelSetCodec::decode(bytes, [&](size_t count) {
		requested = count; throw std::length_error("checksum budget");
	})), std::length_error);
	EXPECT_EQ(requested, bytes.size());
	bool checksumReleased = false;
	EXPECT_THROW(static_cast<void>(EntityLabelSetCodec::decode(bytes, [&](size_t count) {
		if (count == 0U) checksumReleased = true;
		else if (checksumReleased) { requested = count; throw std::length_error("result budget"); }
	})), std::length_error);
	EXPECT_EQ(requested, labels.size() * sizeof(uint64_t));
	for (int retry = 0; retry < 2; ++retry) {
		size_t retained = 0U, peak = 0U;
		const auto result = EntityLabelSetCodec::decode(bytes, [&](size_t count) {
			retained = count; peak = std::max(peak, count);
		});
		EXPECT_EQ(result, (std::vector<uint64_t>(labels.begin(), labels.end())));
		EXPECT_EQ(retained, result.capacity() * sizeof(uint64_t));
		EXPECT_GE(peak, bytes.size());
	}
}
