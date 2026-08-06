/**
 * @file test_CatalogPage.cpp
 * @brief Golden-byte and corruption tests for relocatable Catalog leaf pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <source_location>
#include <span>
#include <utility>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/metadata/CatalogPage.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t HEADER_CRC_OFFSET = 60;
	constexpr size_t PAYLOAD_CRC_OFFSET = 56;
	constexpr size_t PAGE_ID_OFFSET = 16;
	constexpr size_t MINIMUM_SEGMENT_ID_OFFSET = 24;
	constexpr size_t GENERATION_OFFSET = 40;
	constexpr size_t ENTRY_BYTES_OFFSET = 8;
	constexpr size_t ENTRY_COUNT_OFFSET = 12;
	constexpr size_t ENTRY_OFFSET = RSS_CATALOG_PAGE_HEADER_BYTES;
	constexpr size_t ENTRY_PHYSICAL_SLOT_OFFSET = 8;
	constexpr size_t ENTRY_KIND_OFFSET = 40;
	constexpr size_t ENTRY_STATE_OFFSET = 42;
	constexpr size_t ENTRY_RESERVED_OFFSET = 44;

	void expectFormatError(const std::function<void()> &operation, FormatError expected,
						   const std::source_location location = std::source_location::current()) {
		try {
			operation();
			ADD_FAILURE_AT(location.file_name(), static_cast<int>(location.line())) << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	CatalogEntry entry(uint64_t segmentId, uint64_t slot, SegmentKind kind, SegmentState state, uint64_t generation,
					   uint64_t lsn) {
		CatalogEntry result;
		result.segment_id = SegmentId(segmentId);
		result.physical_slot = PhysicalSlotId(slot);
		result.kind = kind;
		result.owner_id = segmentId * 10U;
		result.generation = Generation(generation);
		result.state = state;
		result.committed_lsn = LogSequenceNumber(lsn);
		return result;
	}

	CatalogLeafPage validPage() {
		CatalogLeafPage page;
		page.page_id = PageId(0x0102030405060708ULL);
		page.minimum_segment_id = SegmentId(100);
		page.maximum_segment_id = SegmentId(101);
		page.generation = Generation(5);
		page.committed_lsn = LogSequenceNumber(12);
		page.entries = {
				entry(100, 700, SegmentKind::SGK_ENTITY_PAGE_POOL, SegmentState::SGS_SEALED, 3, 7),
				entry(101, 701, SegmentKind::SGK_PROPERTY_PAGE_POOL, SegmentState::SGS_RETIRED, 4, 12),
		};
		return page;
	}

	void recalculateCrc(std::array<std::byte, RSS_PAGE_SIZE> &bytes) {
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET,
									graph::utils::calculateCrc(bytes.data() + RSS_CATALOG_PAGE_HEADER_BYTES,
															   bytes.size() - RSS_CATALOG_PAGE_HEADER_BYTES));
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET,
									graph::utils::calculateCrc(bytes.data(), RSS_CATALOG_PAGE_HEADER_BYTES));
	}

} // namespace

TEST(RelocatableCatalogPageTest, CodecMatchesGoldenFieldsAndRoundTrips) {
	const CatalogLeafPage page = validPage();
	ASSERT_EQ(static_cast<uint16_t>(page.entries[0].kind), static_cast<uint16_t>(SegmentKind::SGK_ENTITY_PAGE_POOL));
	const auto encoded = CatalogLeafPageCodec::encode(page);
	ASSERT_EQ(LittleEndianCodec::readU16(encoded, ENTRY_OFFSET + ENTRY_KIND_OFFSET),
			  static_cast<uint16_t>(SegmentKind::SGK_ENTITY_PAGE_POOL));
	ASSERT_EQ(LittleEndianCodec::readU16(encoded, ENTRY_OFFSET + RSS_CATALOG_ENTRY_BYTES + ENTRY_KIND_OFFSET),
			  static_cast<uint16_t>(SegmentKind::SGK_PROPERTY_PAGE_POOL));
	EXPECT_EQ(encoded.size(), RSS_PAGE_SIZE);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[0]), 0x5AU);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[1]), 0x59U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[2]), 0x58U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[3]), 0x43U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[4]), 0x04U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[6]), 0x40U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[8]), 0x30U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[12]), 0x02U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[PAGE_ID_OFFSET]), 0x08U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[MINIMUM_SEGMENT_ID_OFFSET]), 100U);
	EXPECT_NE(LittleEndianCodec::readU32(encoded, PAYLOAD_CRC_OFFSET), 0u);
	EXPECT_NE(LittleEndianCodec::readU32(encoded, HEADER_CRC_OFFSET), 0u);

	EXPECT_EQ(LittleEndianCodec::readU64(encoded, ENTRY_OFFSET), 100u);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, ENTRY_OFFSET + ENTRY_PHYSICAL_SLOT_OFFSET), 700u);
	EXPECT_EQ(LittleEndianCodec::readU16(encoded, ENTRY_OFFSET + ENTRY_KIND_OFFSET),
			  static_cast<uint16_t>(SegmentKind::SGK_ENTITY_PAGE_POOL));
	EXPECT_EQ(LittleEndianCodec::readU16(encoded, ENTRY_OFFSET + ENTRY_STATE_OFFSET),
			  static_cast<uint16_t>(SegmentState::SGS_SEALED));

	const CatalogLeafPage decoded = CatalogLeafPageCodec::decode(encoded);
	EXPECT_EQ(decoded.page_id, PageId(0x0102030405060708ULL));
	EXPECT_EQ(decoded.minimum_segment_id, SegmentId(100));
	EXPECT_EQ(decoded.maximum_segment_id, SegmentId(101));
	EXPECT_EQ(decoded.generation, Generation(5));
	EXPECT_EQ(decoded.committed_lsn, LogSequenceNumber(12));
	ASSERT_EQ(decoded.entries.size(), 2u);
	EXPECT_EQ(decoded.entries[0].physical_slot, PhysicalSlotId(700));
	EXPECT_EQ(decoded.entries[1].kind, SegmentKind::SGK_PROPERTY_PAGE_POOL);
	EXPECT_EQ(decoded.entries[1].state, SegmentState::SGS_RETIRED);
}

TEST(RelocatableCatalogPageTest, CodecSupportsEmptyAndFullLeafPages) {
	EXPECT_EQ(RSS_CATALOG_PAGE_MAX_ENTRIES, 84U);
	CatalogLeafPage empty;
	empty.page_id = PageId(1);
	empty.generation = Generation(1);
	empty.committed_lsn = LogSequenceNumber(1);
	const CatalogLeafPage decodedEmpty = CatalogLeafPageCodec::decode(CatalogLeafPageCodec::encode(empty));
	EXPECT_TRUE(decodedEmpty.entries.empty());
	EXPECT_FALSE(decodedEmpty.minimum_segment_id.isValid());
	EXPECT_FALSE(decodedEmpty.maximum_segment_id.isValid());

	CatalogLeafPage full;
	full.page_id = PageId(2);
	full.minimum_segment_id = SegmentId(1000);
	full.maximum_segment_id = SegmentId(1000 + RSS_CATALOG_PAGE_MAX_ENTRIES - 1U);
	full.generation = Generation(1);
	full.committed_lsn = LogSequenceNumber(9);
	for (uint32_t index = 0; index < RSS_CATALOG_PAGE_MAX_ENTRIES; ++index) {
		full.entries.push_back(
				entry(1000U + index, 2000U + index, SegmentKind::SGK_VECTOR_PQ_POOL, SegmentState::SGS_SEALED, 1, 9));
	}
	const CatalogLeafPage decodedFull = CatalogLeafPageCodec::decode(CatalogLeafPageCodec::encode(full));
	EXPECT_EQ(decodedFull.entries.size(), RSS_CATALOG_PAGE_MAX_ENTRIES);
	EXPECT_EQ(decodedFull.entries.front().segment_id, SegmentId(1000));
	EXPECT_EQ(decodedFull.entries.back().segment_id, full.maximum_segment_id);
}

TEST(RelocatableCatalogPageTest, CodecRejectsInvalidInMemoryPages) {
	CatalogLeafPage page = validPage();
	page.page_id = PageId{};
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.generation = Generation{};
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.committed_lsn = LogSequenceNumber{};
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.minimum_segment_id = SegmentId(99);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_HEADER);

	CatalogLeafPage empty;
	empty.page_id = PageId(1);
	empty.generation = Generation(1);
	empty.committed_lsn = LogSequenceNumber(1);
	empty.minimum_segment_id = SegmentId(1);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(empty)); },
					  FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.minimum_segment_id = SegmentId{};
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.maximum_segment_id = SegmentId{};
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.entries.resize(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U, page.entries.front());
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.entries[0].kind = SegmentKind::SGK_NONE;
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_ENUM);

	page = validPage();
	page.entries[0].state = SegmentState::SGS_BUILDING;
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_STATE);

	page = validPage();
	page.entries[0].physical_slot = PhysicalSlotId{};
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.entries[0].generation = Generation{};
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.entries[0].committed_lsn = LogSequenceNumber{};
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.entries[0].committed_lsn = LogSequenceNumber(13);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_STATE);

	page = validPage();
	std::swap(page.entries[0], page.entries[1]);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.entries[1].physical_slot = page.entries[0].physical_slot;
	page.entries[1].state = SegmentState::SGS_SEALED;
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::encode(page)); }, FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.entries[1].physical_slot = page.entries[0].physical_slot;
	EXPECT_EQ(CatalogLeafPageCodec::decode(CatalogLeafPageCodec::encode(page)).entries[1].state,
			  SegmentState::SGS_RETIRED);

}

TEST(RelocatableCatalogPageTest, CodecRejectsTruncatedChecksummedAndMalformedDiskPages) {
	const auto encoded = CatalogLeafPageCodec::encode(validPage());
	expectFormatError(
			[&] { static_cast<void>(CatalogLeafPageCodec::decode(std::span(encoded).first(RSS_PAGE_SIZE - 1U))); },
			FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> oversized(encoded.begin(), encoded.end());
	oversized.push_back(std::byte{0});
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);

	auto corrupt = encoded;
	corrupt[RSS_CATALOG_PAGE_HEADER_BYTES] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	corrupt[0] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 0, 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_MAGIC);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 4, RSS_CATALOG_PAGE_VERSION + 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_UNSUPPORTED_VERSION);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, GENERATION_OFFSET, 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, ENTRY_BYTES_OFFSET, RSS_CATALOG_ENTRY_BYTES - 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, ENTRY_COUNT_OFFSET, RSS_CATALOG_PAGE_MAX_ENTRIES + 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableCatalogPageTest, CodecValidatesEveryPersistedEntryInvariant) {
	const auto encoded = CatalogLeafPageCodec::encode(validPage());

	auto corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, PAGE_ID_OFFSET, 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, GENERATION_OFFSET, 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, MINIMUM_SEGMENT_ID_OFFSET, 99);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, ENTRY_OFFSET + ENTRY_KIND_OFFSET, 99);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_ENUM);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, ENTRY_OFFSET + ENTRY_STATE_OFFSET,
								static_cast<uint16_t>(SegmentState::SGS_BUILDING));
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_STATE);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, ENTRY_OFFSET + ENTRY_STATE_OFFSET, 99);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_ENUM);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, ENTRY_OFFSET + ENTRY_RESERVED_OFFSET, 1);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, ENTRY_OFFSET + RSS_CATALOG_ENTRY_BYTES + ENTRY_PHYSICAL_SLOT_OFFSET, 700);
	LittleEndianCodec::writeU16(corrupt, ENTRY_OFFSET + RSS_CATALOG_ENTRY_BYTES + ENTRY_STATE_OFFSET,
								static_cast<uint16_t>(SegmentState::SGS_SEALED));
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogLeafPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
}
