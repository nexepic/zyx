/**
 * @file test_StateRecord.cpp
 * @brief Explicit relocatable State record codec tests.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <functional>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/metadata/StateRecord.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t MAGIC_OFFSET = 0;
	constexpr size_t VERSION_OFFSET = 4;
	constexpr size_t HEADER_BYTES_OFFSET = 6;
	constexpr size_t STORAGE_KIND_OFFSET = 16;
	constexpr size_t KEY_BYTES_OFFSET = 20;
	constexpr size_t INLINE_BYTES_OFFSET = 24;
	constexpr size_t LARGE_VALUE_ID_OFFSET = 28;
	constexpr size_t PAYLOAD_CRC_OFFSET = 36;
	constexpr size_t HEADER_CRC_OFFSET = 40;
	constexpr size_t RESERVED_OFFSET = 44;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	StateRecord inlineRecord() {
		return {71, "system.state", StateRecordStorageKind::SRS_INLINE, LargeValueId{},
				{std::byte{0x00}, std::byte{0x41}, std::byte{0xFF}}};
	}

	void recalculateChecksums(std::vector<std::byte> &bytes) {
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET,
								graph::utils::calculateCrc(bytes.data() + RSS_STATE_RECORD_HEADER_BYTES,
														 bytes.size() - RSS_STATE_RECORD_HEADER_BYTES));
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET,
								graph::utils::calculateCrc(bytes.data(), RSS_STATE_RECORD_HEADER_BYTES));
	}

	void expectChecksumValidSingleBitMutationsToRejectOrRoundTrip(const std::vector<std::byte> &encoded) {
		for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
			for (uint8_t bit = 0U; bit < 8U; ++bit) {
				auto mutation = encoded;
				mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
				recalculateChecksums(mutation);
				try {
					const StateRecord decoded = StateRecordCodec::decode(mutation);
					if (StateRecordCodec::encode(decoded) != mutation) {
						ADD_FAILURE() << "State record mutation was normalized at byte " << byteOffset << ", bit "
								  << static_cast<unsigned>(bit);
						return;
					}
				} catch (const FormatException &) {
					// A checksum-valid mutation is safe only when the typed boundary rejects it.
				}
			}
		}
	}

} // namespace

TEST(RelocatableStateRecordTest, RoundTripsCanonicalInlineAndLargeValueRecords) {
	const StateRecord inline_record = inlineRecord();
	const std::vector<std::byte> encoded_inline = StateRecordCodec::encode(inline_record);
	EXPECT_EQ(StateRecordCodec::decode(encoded_inline), inline_record);
	EXPECT_EQ(StateRecordCodec::encode(StateRecordCodec::decode(encoded_inline)), encoded_inline);

	const StateRecord external{72, "vector.registry", StateRecordStorageKind::SRS_LARGE_VALUE, LargeValueId(51), {}};
	const std::vector<std::byte> encoded_external = StateRecordCodec::encode(external);
	EXPECT_EQ(StateRecordCodec::decode(encoded_external), external);
	EXPECT_EQ(StateRecordCodec::encode(StateRecordCodec::decode(encoded_external)), encoded_external);
}

TEST(RelocatableStateRecordTest, RejectsInvalidInMemoryRecordIdentitiesAndStorageCombinations) {
	for (const StateRecord &invalid: {
				 StateRecord{0, "key", StateRecordStorageKind::SRS_INLINE, LargeValueId{}, {}},
				 StateRecord{1, "", StateRecordStorageKind::SRS_INLINE, LargeValueId{}, {}},
				 StateRecord{1, "key", StateRecordStorageKind::SRS_INLINE, LargeValueId(9), {}},
				 StateRecord{1, "key", StateRecordStorageKind::SRS_LARGE_VALUE, LargeValueId{}, {}},
				 StateRecord{1, "key", StateRecordStorageKind::SRS_LARGE_VALUE, LargeValueId(9), {std::byte{0x01}}},
				 StateRecord{1, "key", static_cast<StateRecordStorageKind>(99), LargeValueId{}, {}},
		 }) {
		expectFormatError([&] { static_cast<void>(StateRecordCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	}
}

TEST(RelocatableStateRecordTest, FailsClosedForTruncatedChecksummedAndMalformedBytes) {
	const std::vector<std::byte> encoded = StateRecordCodec::encode(inlineRecord());
	expectFormatError(
			[&] { static_cast<void>(StateRecordCodec::decode(std::span(encoded).first(47))); },
			FormatError::FERR_BUFFER_TOO_SMALL);
	auto malformed = encoded;
	LittleEndianCodec::writeU32(malformed, HEADER_CRC_OFFSET, 0U);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_CHECKSUM_MISMATCH);
	malformed = encoded;
	malformed.back() ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_CHECKSUM_MISMATCH);
	malformed = encoded;
	LittleEndianCodec::writeU32(malformed, MAGIC_OFFSET, 0U);
	recalculateChecksums(malformed);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	LittleEndianCodec::writeU16(malformed, VERSION_OFFSET, 1U);
	recalculateChecksums(malformed);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	LittleEndianCodec::writeU16(malformed, HEADER_BYTES_OFFSET, 0U);
	recalculateChecksums(malformed);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	malformed[STORAGE_KIND_OFFSET] = std::byte{99};
	recalculateChecksums(malformed);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	malformed[STORAGE_KIND_OFFSET + 1U] = std::byte{1};
	recalculateChecksums(malformed);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	malformed[STORAGE_KIND_OFFSET + 2U] = std::byte{1};
	recalculateChecksums(malformed);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	malformed[STORAGE_KIND_OFFSET + 3U] = std::byte{1};
	recalculateChecksums(malformed);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	LittleEndianCodec::writeU32(malformed, RESERVED_OFFSET, 1U);
	recalculateChecksums(malformed);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableStateRecordTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	expectChecksumValidSingleBitMutationsToRejectOrRoundTrip(StateRecordCodec::encode(inlineRecord()));
	expectChecksumValidSingleBitMutationsToRejectOrRoundTrip(
			StateRecordCodec::encode({72, "vector.registry", StateRecordStorageKind::SRS_LARGE_VALUE, LargeValueId(51), {}}));
}

TEST(RelocatableStateRecordTest, RejectsMalformedPayloadLengthsModesAndIdentities) {
	const std::vector<std::byte> encoded = StateRecordCodec::encode(inlineRecord());
	auto malformed = encoded;
	LittleEndianCodec::writeU32(malformed, KEY_BYTES_OFFSET, 1000U);
	recalculateChecksums(malformed);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	LittleEndianCodec::writeU32(malformed, INLINE_BYTES_OFFSET, 0U);
	recalculateChecksums(malformed);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	malformed[STORAGE_KIND_OFFSET] = static_cast<std::byte>(StateRecordStorageKind::SRS_LARGE_VALUE);
	LittleEndianCodec::writeU64(malformed, LARGE_VALUE_ID_OFFSET, 9U);
	recalculateChecksums(malformed);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
	malformed = encoded;
	LittleEndianCodec::writeU64(malformed, 8, 0U);
	recalculateChecksums(malformed);
	expectFormatError([&] { static_cast<void>(StateRecordCodec::decode(malformed)); },
				  FormatError::FERR_INVALID_HEADER);
}
