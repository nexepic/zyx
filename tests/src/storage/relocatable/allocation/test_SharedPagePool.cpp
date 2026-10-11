/**
 * @file test_SharedPagePool.cpp
 * @brief Unit tests for relocatable shared page-pool codecs and frame maps.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <source_location>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	void expectFormatError(const std::function<void()> &operation, FormatError expected,
						   const std::source_location location = std::source_location::current()) {
		try {
			operation();
			ADD_FAILURE_AT(location.file_name(), static_cast<int>(location.line())) << "Expected FormatException";
		} catch (const FormatException &error) {
			if (error.error() != expected) {
				ADD_FAILURE_AT(location.file_name(), static_cast<int>(location.line()))
						<< "Unexpected FormatError: " << error.what();
			}
		}
	}

	SharedPagePoolHeader poolHeader(uint32_t frames = 0) {
		return {.segment_id = SegmentId(81),
				.allocated_frames = frames,
				.generation = Generation(7),
				.committed_lsn = LogSequenceNumber(13)};
	}

	PageOwnerHeader ownerHeader(PageOwnerKind kind = PageOwnerKind::POK_INDEX, uint32_t payloadBytes = 512) {
		return {.page_id = PageId(31),
				.owner_id = 0xAABBCCDDU,
				.kind = kind,
				.payload_bytes = payloadBytes,
				.generation = Generation(7),
				.committed_lsn = LogSequenceNumber(13)};
	}

	template <size_t Size>
	void recalculateCrc(std::array<std::byte, Size> &bytes, size_t checksumOffset) {
		LittleEndianCodec::writeU32(bytes, checksumOffset, 0U);
		LittleEndianCodec::writeU32(bytes, checksumOffset, graph::utils::calculateCrc(bytes.data(), bytes.size()));
	}

} // namespace

TEST(RelocatableSharedPagePoolTest, PoolHeaderRoundTripsAndUsesEveryFixedField) {
	SharedPagePoolHeader source = poolHeader((1U << 0U) | (1U << 12U) | (1U << 30U));
	source.control_sequence = 42U;
	const auto bytes = SharedPagePoolHeaderCodec::encode(source);
	EXPECT_EQ(SharedPagePoolHeaderCodec::decode(bytes).segment_id, source.segment_id);
	EXPECT_EQ(SharedPagePoolHeaderCodec::decode(bytes).allocated_frames, source.allocated_frames);
	EXPECT_EQ(SharedPagePoolHeaderCodec::decode(bytes).generation, source.generation);
	EXPECT_EQ(SharedPagePoolHeaderCodec::decode(bytes).committed_lsn, source.committed_lsn);
	EXPECT_EQ(SharedPagePoolHeaderCodec::decode(bytes).control_sequence, source.control_sequence);

	std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES - 1U> shortBytes{};
	expectFormatError([&] { static_cast<void>(SharedPagePoolHeaderCodec::decode(shortBytes)); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	std::array<std::byte, RSS_SHARED_PAGE_POOL_HEADER_BYTES + 1U> longBytes{};
	expectFormatError([&] { static_cast<void>(SharedPagePoolHeaderCodec::decode(longBytes)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableSharedPagePoolTest, PoolHeaderRejectsMalformedDiskBytesAndInvalidValues) {
	const auto encoded = SharedPagePoolHeaderCodec::encode(poolHeader());
	for (const size_t offset: {size_t{0}, size_t{4}, size_t{40}}) {
		auto corrupt = encoded;
		corrupt[offset] ^= std::byte{0xFF};
		expectFormatError([&] { static_cast<void>(SharedPagePoolHeaderCodec::decode(corrupt)); },
						  FormatError::FERR_CHECKSUM_MISMATCH);
	}
	auto reserved = encoded;
	LittleEndianCodec::writeU32(reserved, 44, 1);
	LittleEndianCodec::writeU32(reserved, 40, 0);
	LittleEndianCodec::writeU32(reserved, 40, graph::utils::calculateCrc(reserved.data(), reserved.size()));
	expectFormatError([&] { static_cast<void>(SharedPagePoolHeaderCodec::decode(reserved)); },
					  FormatError::FERR_RESERVED_BITS);
	auto wrongCount = encoded;
	LittleEndianCodec::writeU32(wrongCount, 20, 1);
	LittleEndianCodec::writeU32(wrongCount, 40, 0);
	LittleEndianCodec::writeU32(wrongCount, 40, graph::utils::calculateCrc(wrongCount.data(), wrongCount.size()));
	expectFormatError([&] { static_cast<void>(SharedPagePoolHeaderCodec::decode(wrongCount)); },
					  FormatError::FERR_INVALID_HEADER);

	SharedPagePoolHeader noSegment = poolHeader();
	noSegment.segment_id = {};
	expectFormatError([&] { static_cast<void>(SharedPagePoolHeaderCodec::encode(noSegment)); },
					  FormatError::FERR_INVALID_HEADER);
	SharedPagePoolHeader invalidFrame = poolHeader(1U << RSS_PAGE_POOL_DATA_FRAME_COUNT);
	expectFormatError([&] { static_cast<void>(SharedPagePoolHeaderCodec::encode(invalidFrame)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableSharedPagePoolTest, FixedHeadersRejectOrExactlyRoundTripEveryChecksumValidSingleBitMutation) {
	SharedPagePoolHeader pool = poolHeader((1U << 0U) | (1U << 7U) | (1U << 30U));
	pool.control_sequence = 42U;
	const auto encodedPool = SharedPagePoolHeaderCodec::encode(pool);
	for (size_t byteOffset = 0U; byteOffset < encodedPool.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encodedPool;
			mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
			recalculateCrc(mutation, 40U);
			try {
				const SharedPagePoolHeader decoded = SharedPagePoolHeaderCodec::decode(mutation);
				if (SharedPagePoolHeaderCodec::encode(decoded) != mutation) {
					ADD_FAILURE() << "shared pool-header mutation was normalized at byte " << byteOffset << ", bit "
							  << static_cast<unsigned>(bit);
					return;
				}
			} catch (const FormatException &) {
				// A checksum-valid mutation is safe only when the typed boundary rejects it.
			}
		}
	}

	const auto encodedOwner = PageOwnerHeaderCodec::encode(ownerHeader(PageOwnerKind::POK_ENTITY, 777U));
	for (size_t byteOffset = 0U; byteOffset < encodedOwner.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encodedOwner;
			mutation[byteOffset] ^= std::byte{static_cast<uint8_t>(1U << bit)};
			recalculateCrc(mutation, 48U);
			try {
				const PageOwnerHeader decoded = PageOwnerHeaderCodec::decode(mutation);
				if (PageOwnerHeaderCodec::encode(decoded) != mutation) {
					ADD_FAILURE() << "page owner-header mutation was normalized at byte " << byteOffset << ", bit "
							  << static_cast<unsigned>(bit);
					return;
				}
			} catch (const FormatException &) {
				// A checksum-valid mutation is safe only when the typed boundary rejects it.
			}
		}
	}
}

TEST(RelocatableSharedPagePoolTest, OwnerHeaderRoundTripsAndRejectsCorruption) {
	const PageOwnerHeader source = ownerHeader();
	const auto encoded = PageOwnerHeaderCodec::encode(source);
	const PageOwnerHeader decoded = PageOwnerHeaderCodec::decode(encoded);
	EXPECT_EQ(decoded.page_id, source.page_id);
	EXPECT_EQ(decoded.owner_id, source.owner_id);
	EXPECT_EQ(decoded.kind, source.kind);
	EXPECT_EQ(decoded.flags, source.flags);
	EXPECT_EQ(decoded.payload_bytes, source.payload_bytes);
	EXPECT_EQ(decoded.generation, source.generation);
	EXPECT_EQ(decoded.committed_lsn, source.committed_lsn);
	for (const PageOwnerKind kind:
			 {PageOwnerKind::POK_PROPERTY, PageOwnerKind::POK_INDEX, PageOwnerKind::POK_VECTOR_RAW,
			  PageOwnerKind::POK_VECTOR_PQ, PageOwnerKind::POK_VECTOR_ADJACENCY,
			  PageOwnerKind::POK_VECTOR_MANIFEST, PageOwnerKind::POK_STATE, PageOwnerKind::POK_ENTITY,
			  PageOwnerKind::POK_ENTITY_LABEL}) {
		EXPECT_EQ(PageOwnerHeaderCodec::decode(PageOwnerHeaderCodec::encode(ownerHeader(kind))).kind, kind);
	}

	std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES - 1U> shortBytes{};
	expectFormatError([&] { static_cast<void>(PageOwnerHeaderCodec::decode(shortBytes)); },
					  FormatError::FERR_BUFFER_TOO_SMALL);
	std::array<std::byte, RSS_PAGE_OWNER_HEADER_BYTES + 1U> longBytes{};
	expectFormatError([&] { static_cast<void>(PageOwnerHeaderCodec::decode(longBytes)); },
					  FormatError::FERR_INVALID_HEADER);

	auto corrupt = encoded;
	corrupt[0] ^= std::byte{0xFF};
	expectFormatError([&] { static_cast<void>(PageOwnerHeaderCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
	auto invalidKind = encoded;
	LittleEndianCodec::writeU16(invalidKind, 24, 99);
	LittleEndianCodec::writeU32(invalidKind, 48, 0);
	LittleEndianCodec::writeU32(invalidKind, 48, graph::utils::calculateCrc(invalidKind.data(), invalidKind.size()));
	expectFormatError([&] { static_cast<void>(PageOwnerHeaderCodec::decode(invalidKind)); },
					  FormatError::FERR_INVALID_HEADER);
	// These numeric values belonged to the retired Root/Base/Delta adjacency
	// pages. The clean global-tree format must reject them before page decoding.
	for (const uint16_t retiredKind: {13U, 14U, 15U}) {
		auto retiredKindBytes = encoded;
		LittleEndianCodec::writeU16(retiredKindBytes, 24, retiredKind);
		LittleEndianCodec::writeU32(retiredKindBytes, 48, 0);
		LittleEndianCodec::writeU32(retiredKindBytes, 48,
									graph::utils::calculateCrc(retiredKindBytes.data(), retiredKindBytes.size()));
		expectFormatError([&] { static_cast<void>(PageOwnerHeaderCodec::decode(retiredKindBytes)); },
						  FormatError::FERR_INVALID_HEADER);
	}
	PageOwnerHeader retired = source;
	retired.flags = RSS_PAGE_OWNER_FLAG_RETIRED;
	EXPECT_EQ(PageOwnerHeaderCodec::decode(PageOwnerHeaderCodec::encode(retired)).flags, RSS_PAGE_OWNER_FLAG_RETIRED);
	auto reserved = encoded;
	LittleEndianCodec::writeU16(reserved, 26, 2);
	LittleEndianCodec::writeU32(reserved, 48, 0);
	LittleEndianCodec::writeU32(reserved, 48, graph::utils::calculateCrc(reserved.data(), reserved.size()));
	expectFormatError([&] { static_cast<void>(PageOwnerHeaderCodec::decode(reserved)); },
					  FormatError::FERR_RESERVED_BITS);
}

TEST(RelocatableSharedPagePoolTest, OwnerHeaderValidatesBoundsBeforeEncoding) {
	PageOwnerHeader noPage = ownerHeader();
	noPage.page_id = {};
	expectFormatError([&] { static_cast<void>(PageOwnerHeaderCodec::encode(noPage)); },
					  FormatError::FERR_INVALID_HEADER);
	PageOwnerHeader noKind = ownerHeader(PageOwnerKind::POK_NONE);
	expectFormatError([&] { static_cast<void>(PageOwnerHeaderCodec::encode(noKind)); },
					  FormatError::FERR_INVALID_HEADER);
	PageOwnerHeader badPayload = ownerHeader(PageOwnerKind::POK_PROPERTY, RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES + 1U);
	expectFormatError([&] { static_cast<void>(PageOwnerHeaderCodec::encode(badPayload)); },
					  FormatError::FERR_INVALID_HEADER);
	PageOwnerHeader noGeneration = ownerHeader();
	noGeneration.generation = {};
	expectFormatError([&] { static_cast<void>(PageOwnerHeaderCodec::encode(noGeneration)); },
					  FormatError::FERR_INVALID_HEADER);
	PageOwnerHeader noLsn = ownerHeader();
	noLsn.committed_lsn = {};
	expectFormatError([&] { static_cast<void>(PageOwnerHeaderCodec::encode(noLsn)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableSharedPagePoolTest, FrameMapAllocatesReleasesAndReusesEveryDataFrame) {
	SharedPagePoolHeader header = poolHeader();
	for (uint8_t frame = RSS_PAGE_POOL_FIRST_DATA_FRAME; frame <= RSS_PAGE_POOL_DATA_FRAME_COUNT; ++frame) {
		const std::optional<uint8_t> allocated = SharedPagePoolFrameMap::allocateFirstFree(header);
		ASSERT_TRUE(allocated.has_value());
		EXPECT_EQ(*allocated, frame);
		EXPECT_TRUE(SharedPagePoolFrameMap::isAllocated(header, frame));
	}
	EXPECT_FALSE(SharedPagePoolFrameMap::allocateFirstFree(header).has_value());
	SharedPagePoolFrameMap::release(header, 7);
	EXPECT_FALSE(SharedPagePoolFrameMap::isAllocated(header, 7));
	EXPECT_EQ(SharedPagePoolFrameMap::allocateFirstFree(header), std::optional<uint8_t>(7));

	expectFormatError([&] { SharedPagePoolFrameMap::release(header, 0); }, FormatError::FERR_INVALID_HEADER);
	expectFormatError([&] { static_cast<void>(SharedPagePoolFrameMap::isAllocated(header, 32)); },
					  FormatError::FERR_INVALID_HEADER);
	SharedPagePoolFrameMap::release(header, 7);
	expectFormatError([&] { SharedPagePoolFrameMap::release(header, 7); }, FormatError::FERR_INVALID_STATE);
}

TEST(RelocatableSharedPagePoolTest, FrameMapRejectsInvalidHeaders) {
	SharedPagePoolHeader invalid = poolHeader(1U << RSS_PAGE_POOL_DATA_FRAME_COUNT);
	expectFormatError([&] { static_cast<void>(SharedPagePoolFrameMap::allocateFirstFree(invalid)); },
					  FormatError::FERR_INVALID_HEADER);
	invalid = poolHeader();
	invalid.generation = {};
	expectFormatError([&] { static_cast<void>(SharedPagePoolFrameMap::isAllocated(invalid, 1)); },
					  FormatError::FERR_INVALID_HEADER);
}
