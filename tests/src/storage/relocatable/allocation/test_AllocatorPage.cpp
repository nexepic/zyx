/**
 * @file test_AllocatorPage.cpp
 * @brief Golden-byte and corruption tests for relocatable free-space bitmap leaves.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/allocation/AllocatorPage.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t HEADER_CRC_OFFSET = 60;
	constexpr size_t PAYLOAD_CRC_OFFSET = 56;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	AllocatorLeafPage page(uint32_t slotCount = 10) {
		AllocatorLeafPage result;
		result.page_id = MetadataPageId(7);
		result.first_slot = PhysicalSlotId(101);
		result.slot_count = slotCount;
		result.generation = Generation(3);
		result.committed_lsn = LogSequenceNumber(9);
		result.allocated_slots[0] = std::byte{0b00000101};
		result.allocated_slots[1] = std::byte{0b00000010};
		return result;
	}

	void recalculateHeaderCrc(std::array<std::byte, RSS_PAGE_SIZE> &bytes) {
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET,
									graph::utils::calculateCrc(bytes.data(), RSS_ALLOCATOR_PAGE_HEADER_BYTES));
	}

	void recalculatePayloadAndHeaderCrc(std::array<std::byte, RSS_PAGE_SIZE> &bytes) {
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET,
									graph::utils::calculateCrc(bytes.data() + RSS_ALLOCATOR_PAGE_HEADER_BYTES,
															   RSS_ALLOCATOR_PAGE_BITMAP_BYTES));
		recalculateHeaderCrc(bytes);
	}

} // namespace

TEST(RelocatableAllocatorPageTest, MatchesGoldenFieldsAndRoundTrips) {
	const auto encoded = AllocatorLeafPageCodec::encode(page());
	EXPECT_EQ(encoded.size(), RSS_PAGE_SIZE);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[0]), 0x5AU);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[1]), 0x59U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[2]), 0x58U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[3]), 0x41U);
	EXPECT_EQ(LittleEndianCodec::readU32(encoded, 8), RSS_ALLOCATOR_PAGE_BITMAP_BYTES);
	EXPECT_EQ(LittleEndianCodec::readU32(encoded, 12), 10U);
	EXPECT_EQ(LittleEndianCodec::readU32(encoded, 16), 3U);

	const AllocatorLeafPage decoded = AllocatorLeafPageCodec::decode(encoded);
	EXPECT_EQ(decoded.page_id, MetadataPageId(7));
	EXPECT_EQ(decoded.first_slot, PhysicalSlotId(101));
	EXPECT_EQ(decoded.slot_count, 10U);
	EXPECT_EQ(decoded.generation, Generation(3));
	EXPECT_EQ(decoded.committed_lsn, LogSequenceNumber(9));
	EXPECT_EQ(decoded.allocated_slots[0], std::byte{0b00000101});
	EXPECT_EQ(decoded.allocated_slots[1], std::byte{0b00000010});
}

TEST(RelocatableAllocatorPageTest, RejectsSizeAndChecksumFailures) {
	const auto encoded = AllocatorLeafPageCodec::encode(page());
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(std::span(encoded).first(4095))); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> oversized(encoded.begin(), encoded.end());
	oversized.push_back(std::byte{});
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);

	auto corrupt = encoded;
	corrupt[0] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
	corrupt = encoded;
	corrupt[RSS_ALLOCATOR_PAGE_HEADER_BYTES] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}

TEST(RelocatableAllocatorPageTest, RejectsMalformedHeaderFields) {
	const auto encoded = AllocatorLeafPageCodec::encode(page());
	auto corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 0, 0);
	recalculateHeaderCrc(corrupt);
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_MAGIC);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 0, 0x4658595AU); // Retired ZYXF free-space leaf.
	recalculateHeaderCrc(corrupt);
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_MAGIC);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 4, RSS_ALLOCATOR_PAGE_VERSION + 1U);
	recalculateHeaderCrc(corrupt);
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_UNSUPPORTED_VERSION);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 6, RSS_ALLOCATOR_PAGE_HEADER_BYTES - 1U);
	recalculateHeaderCrc(corrupt);
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 8, 0);
	recalculateHeaderCrc(corrupt);
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);
	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 20, 1);
	recalculateHeaderCrc(corrupt);
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);
}

TEST(RelocatableAllocatorPageTest, RejectsInvalidRangeAndBitmapState) {
	AllocatorLeafPage invalid = page();
	invalid.page_id = MetadataPageId{};
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = page();
	invalid.first_slot = PhysicalSlotId{};
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = page(0);
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = page(RSS_ALLOCATOR_PAGE_SLOT_CAPACITY + 1U);
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = page(2);
	invalid.first_slot = PhysicalSlotId(std::numeric_limits<uint64_t>::max());
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = page();
	invalid.generation = Generation{};
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = page();
	invalid.committed_lsn = LogSequenceNumber{};
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = page(10);
	invalid.allocated_slots[1] |= std::byte{0b11111100};
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_RESERVED_BITS);
	invalid = page();
	invalid.allocated_slots[2] = std::byte{0x01};
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::encode(invalid)); },
					  FormatError::FERR_RESERVED_BITS);
}

TEST(RelocatableAllocatorPageTest, RejectsTamperedAllocationCountAndUnusedPayloadBits) {
	auto corrupt = AllocatorLeafPageCodec::encode(page());
	LittleEndianCodec::writeU32(corrupt, 16, 2);
	recalculateHeaderCrc(corrupt);
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = AllocatorLeafPageCodec::encode(page(10));
	corrupt[RSS_ALLOCATOR_PAGE_HEADER_BYTES + 1] |= std::byte{0b11111100};
	recalculatePayloadAndHeaderCrc(corrupt);
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);
	corrupt = AllocatorLeafPageCodec::encode(page());
	corrupt[RSS_ALLOCATOR_PAGE_HEADER_BYTES + 2] = std::byte{0x01};
	recalculatePayloadAndHeaderCrc(corrupt);
	expectFormatError([&] { static_cast<void>(AllocatorLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);
}

TEST(RelocatableAllocatorPageTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	const auto encoded = AllocatorLeafPageCodec::encode(page());
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
			recalculatePayloadAndHeaderCrc(mutation);
			try {
				const AllocatorLeafPage decoded = AllocatorLeafPageCodec::decode(mutation);
				if (AllocatorLeafPageCodec::encode(decoded) != mutation) {
					ADD_FAILURE() << "Allocator mutation was silently normalized at byte " << byteOffset << ", bit "
									 << static_cast<unsigned>(bit);
					return;
				}
			} catch (const FormatException &) {
				// Every non-canonical mutation must fail at the typed format boundary.
			}
		}
	}
}
