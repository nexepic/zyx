/**
 * @file test_CatalogRadix.cpp
 * @brief Golden-byte, route, and corruption tests for relocatable Catalog radix pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/metadata/CatalogRadix.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t HEADER_CRC_OFFSET = 52;
	constexpr size_t PAYLOAD_CRC_OFFSET = 48;
	constexpr size_t KIND_OFFSET = 8;
	constexpr size_t LEVEL_OFFSET = 10;
	constexpr size_t FLAGS_OFFSET = 12;
	constexpr size_t CHILD_BYTES_OFFSET = 16;
	constexpr size_t CHILD_COUNT_OFFSET = 20;
	constexpr size_t PAGE_ID_OFFSET = 24;
	constexpr size_t RESERVED_OFFSET = 56;

	void expectFormatError(const std::function<void()> &operation, FormatError expected) {
		try {
			operation();
			FAIL() << "Expected FormatException";
		} catch (const FormatException &error) {
			EXPECT_EQ(error.error(), expected);
		}
	}

	PhysicalPageRef pageRef(uint64_t slot, uint8_t frame, uint64_t generation) {
		PhysicalPageRef result;
		result.physical_slot = PhysicalSlotId(slot);
		result.frame_index = frame;
		result.generation = Generation(generation);
		return result;
	}

	CatalogRadixPage validPage() {
		CatalogRadixPage page;
		page.kind = CatalogRadixPageKind::CRPK_ROOT;
		page.level = 2;
		page.page_id = PageId(0x0102030405060708ULL);
		page.generation = Generation(6);
		page.committed_lsn = LogSequenceNumber(20);
		page.children[0] = pageRef(10, 1, 5);
		page.children[1] = pageRef(11, 2, 6);
		page.children[RSS_CATALOG_RADIX_FANOUT - 1U] = pageRef(12, 31, 7);
		return page;
	}

	void recalculateCrc(std::array<std::byte, RSS_PAGE_SIZE> &bytes) {
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET,
									graph::utils::calculateCrc(bytes.data() + RSS_CATALOG_RADIX_HEADER_BYTES,
															   bytes.size() - RSS_CATALOG_RADIX_HEADER_BYTES));
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET,
									graph::utils::calculateCrc(bytes.data(), RSS_CATALOG_RADIX_HEADER_BYTES));
	}

} // namespace

TEST(RelocatableCatalogRadixTest, CodecMatchesGoldenFieldsAndRoundTrips) {
	const auto encoded = CatalogRadixPageCodec::encode(validPage());
	EXPECT_EQ(encoded.size(), RSS_PAGE_SIZE);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[0]), 0x5AU);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[1]), 0x59U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[2]), 0x58U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[3]), 0x52U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[4]), 4U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[6]), 96U);
	EXPECT_EQ(LittleEndianCodec::readU16(encoded, KIND_OFFSET), static_cast<uint16_t>(CatalogRadixPageKind::CRPK_ROOT));
	EXPECT_EQ(LittleEndianCodec::readU32(encoded, FLAGS_OFFSET),
			  static_cast<uint32_t>(MetadataDirectoryType::MDT_CATALOG));
	EXPECT_EQ(LittleEndianCodec::readU16(encoded, LEVEL_OFFSET), 2U);
	EXPECT_EQ(LittleEndianCodec::readU32(encoded, CHILD_COUNT_OFFSET), 3U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, PAGE_ID_OFFSET), 0x0102030405060708ULL);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, RSS_CATALOG_RADIX_HEADER_BYTES), 10U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, RSS_CATALOG_RADIX_HEADER_BYTES + sizeof(uint64_t)), (5U << 5U) | 1U);
	EXPECT_NE(LittleEndianCodec::readU32(encoded, PAYLOAD_CRC_OFFSET), 0U);
	EXPECT_NE(LittleEndianCodec::readU32(encoded, HEADER_CRC_OFFSET), 0U);

	const CatalogRadixPage decoded = CatalogRadixPageCodec::decode(encoded);
	EXPECT_EQ(decoded.kind, CatalogRadixPageKind::CRPK_ROOT);
	EXPECT_EQ(decoded.directory_type, MetadataDirectoryType::MDT_CATALOG);
	EXPECT_EQ(decoded.level, 2U);
	EXPECT_EQ(decoded.page_id, PageId(0x0102030405060708ULL));
	EXPECT_EQ(decoded.generation, Generation(6));
	EXPECT_EQ(decoded.committed_lsn, LogSequenceNumber(20));
	EXPECT_EQ(decoded.children[1].physical_slot, PhysicalSlotId(11));
	EXPECT_EQ(decoded.children[RSS_CATALOG_RADIX_FANOUT - 1U].frame_index, 31U);
}

TEST(RelocatableCatalogRadixTest, CodecSupportsEmptyAndFullChildArrays) {
	CatalogRadixPage empty;
	empty.page_id = PageId(1);
	empty.generation = Generation(1);
	empty.committed_lsn = LogSequenceNumber(1);
	const CatalogRadixPage decodedEmpty = CatalogRadixPageCodec::decode(CatalogRadixPageCodec::encode(empty));
	EXPECT_FALSE(decodedEmpty.children[0].isValid());

	CatalogRadixPage full;
	full.kind = CatalogRadixPageKind::CRPK_INTERNAL;
	full.level = RSS_CATALOG_RADIX_MAX_LEVEL;
	full.page_id = PageId(2);
	full.generation = Generation(2);
	full.committed_lsn = LogSequenceNumber(2);
	for (uint32_t index = 0; index < RSS_CATALOG_RADIX_FANOUT; ++index) {
		full.children[index] = pageRef(1000U + index, static_cast<uint8_t>(index % 31U + 1U), 2);
	}
	const auto encodedFull = CatalogRadixPageCodec::encode(full);
	EXPECT_EQ(LittleEndianCodec::readU32(encodedFull, CHILD_COUNT_OFFSET), RSS_CATALOG_RADIX_FANOUT);
	EXPECT_EQ(CatalogRadixPageCodec::decode(encodedFull).children[RSS_CATALOG_RADIX_FANOUT - 1U].physical_slot,
			  PhysicalSlotId(1000U + RSS_CATALOG_RADIX_FANOUT - 1U));
}

TEST(RelocatableCatalogRadixTest, RejectsInvalidInMemoryPages) {
	CatalogRadixPage page = validPage();
	page.kind = static_cast<CatalogRadixPageKind>(99);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::encode(page)); }, FormatError::FERR_INVALID_ENUM);

	page = validPage();
	page.directory_type = static_cast<MetadataDirectoryType>(99);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::encode(page)); }, FormatError::FERR_INVALID_ENUM);

	page = validPage();
	page.level = RSS_CATALOG_RADIX_MAX_LEVEL + 1U;
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::encode(page)); },
					  FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.page_id = PageId{};
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::encode(page)); },
					  FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.generation = Generation{};
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::encode(page)); },
					  FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.committed_lsn = LogSequenceNumber{};
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::encode(page)); },
					  FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.children[0].frame_index = 0;
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::encode(page)); },
					  FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.children[0] = PhysicalPageRef{};
	page.children[0].frame_index = 1;
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::encode(page)); },
					  FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.children[0].generation = Generation(std::numeric_limits<uint64_t>::max());
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::encode(page)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableCatalogRadixTest, PersistsAllocatorFreeChildSummaryOnlyForAllocatorDirectories) {
	CatalogRadixPage page = validPage();
	page.allocator_free_children[0] = std::byte{0x01};
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::encode(page)); }, FormatError::FERR_RESERVED_BITS);

	page = validPage();
	page.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	page.allocator_free_children[0] = std::byte{0x03};
	const CatalogRadixPage decoded = CatalogRadixPageCodec::decode(CatalogRadixPageCodec::encode(page));
	EXPECT_EQ(decoded.allocator_free_children[0], std::byte{0x03});

	page.allocator_free_children.back() = std::byte{0x04};
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::encode(page)); }, FormatError::FERR_RESERVED_BITS);
}

TEST(RelocatableCatalogRadixTest, RejectsTruncatedChecksummedAndMalformedDiskPages) {
	const auto encoded = CatalogRadixPageCodec::encode(validPage());
	expectFormatError(
			[&] { static_cast<void>(CatalogRadixPageCodec::decode(std::span(encoded).first(RSS_PAGE_SIZE - 1U))); },
			FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> oversized(encoded.begin(), encoded.end());
	oversized.push_back(std::byte{0});
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(oversized)); },
					  FormatError::FERR_INVALID_HEADER);

	auto corrupt = encoded;
	corrupt[RSS_CATALOG_RADIX_HEADER_BYTES] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	corrupt[0] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 0, 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_MAGIC);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 4, RSS_CATALOG_RADIX_PAGE_VERSION + 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_UNSUPPORTED_VERSION);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, KIND_OFFSET, 99);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_ENUM);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, FLAGS_OFFSET, 0x100U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, FLAGS_OFFSET, 99U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, RESERVED_OFFSET, 1);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_RESERVED_BITS);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, CHILD_BYTES_OFFSET, RSS_CATALOG_RADIX_CHILD_BYTES - 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, LEVEL_OFFSET, RSS_CATALOG_RADIX_MAX_LEVEL + 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, CHILD_COUNT_OFFSET, RSS_CATALOG_RADIX_FANOUT + 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, RSS_CATALOG_RADIX_HEADER_BYTES + sizeof(uint64_t), 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, RSS_CATALOG_RADIX_HEADER_BYTES, 0);
	LittleEndianCodec::writeU64(corrupt, RSS_CATALOG_RADIX_HEADER_BYTES + sizeof(uint64_t), 1);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, RSS_CATALOG_RADIX_HEADER_BYTES + RSS_CATALOG_RADIX_CHILD_BYTES, 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableCatalogRadixTest, RoutesSegmentIdsByRadixDigitAndRejectsAbsentChildren) {
	EXPECT_EQ(CatalogRadixPageCodec::childIndex(SegmentId(1), 0), 0U);
	EXPECT_EQ(CatalogRadixPageCodec::childIndex(SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES), 0), 0U);
	EXPECT_EQ(CatalogRadixPageCodec::childIndex(SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U), 0), 1U);
	EXPECT_EQ(
			CatalogRadixPageCodec::childIndex(
					SegmentId(static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT + 1U), 1),
			1U);
	EXPECT_LT(CatalogRadixPageCodec::childIndex(SegmentId(std::numeric_limits<uint64_t>::max()),
												RSS_CATALOG_RADIX_MAX_LEVEL),
			  RSS_CATALOG_RADIX_FANOUT);

	CatalogRadixPage page = validPage();
	page.level = 0;
	page.children.fill(PhysicalPageRef{});
	page.children[0] = pageRef(100, 1, 1);
	page.children[1] = pageRef(101, 1, 1);
	EXPECT_EQ(CatalogRadixPageCodec::childFor(page, SegmentId(1)).physical_slot, PhysicalSlotId(100));
	EXPECT_EQ(CatalogRadixPageCodec::childFor(page, SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES)).physical_slot,
			  PhysicalSlotId(100));
	EXPECT_EQ(CatalogRadixPageCodec::childFor(page, SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U)).physical_slot,
			  PhysicalSlotId(101));

	page = validPage();
	page.kind = CatalogRadixPageKind::CRPK_INTERNAL;
	page.level = 1;
	page.children.fill(PhysicalPageRef{});
	page.children[1] = pageRef(100, 1, 1);
	EXPECT_EQ(CatalogRadixPageCodec::childFor(
					  page,
					  SegmentId(static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT + 1U))
					  .physical_slot,
			  PhysicalSlotId(100));
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::childFor(page, SegmentId(1))); },
					  FormatError::FERR_INVALID_HEADER);

	page = validPage();
	page.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::childFor(page, SegmentId(1))); },
					  FormatError::FERR_INVALID_STATE);

	page = validPage();
	page.level = 1;
	expectFormatError(
			[&] {
				static_cast<void>(CatalogRadixPageCodec::childFor(
						page, SegmentId(static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT *
												RSS_CATALOG_RADIX_FANOUT +
										1U)));
			},
			FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::childIndex(SegmentId{}, 0)); },
					  FormatError::FERR_INVALID_HEADER);
	expectFormatError(
			[&] {
				static_cast<void>(CatalogRadixPageCodec::childIndex(SegmentId(1), RSS_CATALOG_RADIX_MAX_LEVEL + 1U));
			},
			FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableCatalogRadixTest, FindsSparseRoutesWithoutTreatingAbsenceAsCorruption) {
	CatalogRadixPage page = validPage();
	page.level = 0;
	page.children.fill(PhysicalPageRef{});
	page.children[0] = pageRef(100, 1, 1);
	const std::optional<PhysicalPageRef> present = CatalogRadixPageCodec::findChildFor(page, SegmentId(1));
	ASSERT_TRUE(present.has_value());
	EXPECT_EQ(present->physical_slot, PhysicalSlotId(100));
	const std::optional<PhysicalPageRef> ordinalPresent = CatalogRadixPageCodec::findChildForLeafOrdinal(page, 0);
	ASSERT_TRUE(ordinalPresent.has_value());
	EXPECT_EQ(ordinalPresent->physical_slot, PhysicalSlotId(100));
	EXPECT_FALSE(CatalogRadixPageCodec::findChildFor(page, SegmentId(RSS_CATALOG_PAGE_MAX_ENTRIES + 1U)).has_value());
	EXPECT_FALSE(CatalogRadixPageCodec::findChildForLeafOrdinal(page, 1).has_value());
	EXPECT_FALSE(CatalogRadixPageCodec::findChildFor(
						 page,
						 SegmentId(static_cast<uint64_t>(RSS_CATALOG_PAGE_MAX_ENTRIES) * RSS_CATALOG_RADIX_FANOUT + 1U))
						 .has_value());

	page.directory_type = MetadataDirectoryType::MDT_ALLOCATOR;
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::findChildFor(page, SegmentId(1))); },
					  FormatError::FERR_INVALID_STATE);
	EXPECT_TRUE(CatalogRadixPageCodec::findChildForLeafOrdinal(page, 0).has_value());
	page.directory_type = MetadataDirectoryType::MDT_CATALOG;
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::findChildFor(page, SegmentId{})); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableCatalogRadixTest, SupportsAllDirectoryTypesAndValidatesLeafOrdinalRoutes) {
	for (const MetadataDirectoryType directoryType: {
				 MetadataDirectoryType::MDT_ALLOCATOR,
				 MetadataDirectoryType::MDT_PAGE,
				 MetadataDirectoryType::MDT_EXTENT,
				 MetadataDirectoryType::MDT_NODE_ENTITY,
				 MetadataDirectoryType::MDT_EDGE_ENTITY,
				 MetadataDirectoryType::MDT_INDEX_ROOT,
				 MetadataDirectoryType::MDT_VECTOR_LOCATION,
				 MetadataDirectoryType::MDT_VECTOR_MANIFEST,
				 MetadataDirectoryType::MDT_LARGE_VALUE_MANIFEST,
				 MetadataDirectoryType::MDT_STATE,
		 }) {
		CatalogRadixPage page = validPage();
		page.directory_type = directoryType;
		EXPECT_EQ(CatalogRadixPageCodec::decode(CatalogRadixPageCodec::encode(page)).directory_type, directoryType);
	}

	CatalogRadixPage root = validPage();
	root.level = 0;
	root.children.fill(PhysicalPageRef{});
	root.children[0] = pageRef(100, 1, 1);
	root.children[RSS_CATALOG_RADIX_FANOUT - 1U] = pageRef(101, 1, 1);
	EXPECT_EQ(CatalogRadixPageCodec::childForLeafOrdinal(root, 0).physical_slot, PhysicalSlotId(100));
	EXPECT_EQ(CatalogRadixPageCodec::childForLeafOrdinal(root, RSS_CATALOG_RADIX_FANOUT - 1U).physical_slot,
			  PhysicalSlotId(101));
	expectFormatError(
			[&] { static_cast<void>(CatalogRadixPageCodec::childForLeafOrdinal(root, RSS_CATALOG_RADIX_FANOUT)); },
			FormatError::FERR_INVALID_HEADER);
	root.children[0] = {};
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::childForLeafOrdinal(root, 0)); },
					  FormatError::FERR_INVALID_HEADER);

	CatalogRadixPage internal = validPage();
	internal.kind = CatalogRadixPageKind::CRPK_INTERNAL;
	internal.level = 0;
	internal.children.fill(PhysicalPageRef{});
	internal.children[1] = pageRef(101, 1, 1);
	EXPECT_EQ(CatalogRadixPageCodec::childForLeafOrdinal(internal, 1).physical_slot, PhysicalSlotId(101));
	expectFormatError(
			[&] {
				static_cast<void>(CatalogRadixPageCodec::childIndexForLeafOrdinal(0, RSS_CATALOG_RADIX_MAX_LEVEL + 1U));
			},
			FormatError::FERR_INVALID_HEADER);

	auto corrupt = CatalogRadixPageCodec::encode(validPage());
	LittleEndianCodec::writeU32(corrupt, CHILD_COUNT_OFFSET, 0U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(CatalogRadixPageCodec::decode(corrupt)); },
					  FormatError::FERR_INVALID_HEADER);
}
