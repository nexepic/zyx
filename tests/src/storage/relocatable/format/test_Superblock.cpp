/**
 * @file test_Superblock.cpp
 * @brief Golden-byte, recovery-selection, and corruption tests for relocatable superblocks.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <limits>
#include <random>
#include <span>
#include <vector>

#include <gtest/gtest.h>

#include "graph/storage/relocatable/format/Superblock.hpp"
#include "graph/utils/ChecksumUtils.hpp"

using namespace graph::storage;

namespace {

	constexpr size_t HEADER_CRC_OFFSET = 116;
	constexpr size_t PAYLOAD_CRC_OFFSET = 112;
	constexpr size_t FLAGS_OFFSET = 8;
	constexpr size_t SEGMENT_SIZE_OFFSET = 32;
	constexpr size_t DATA_REGION_OFFSET = 40;
	constexpr size_t CATALOG_ROOT_OFFSET = 48;
	constexpr size_t DATABASE_ID_OFFSET = 120;
	constexpr size_t NODE_ENTITY_DIRECTORY_ROOT_OFFSET = 128;
	constexpr size_t EDGE_ENTITY_DIRECTORY_ROOT_OFFSET = 144;
	constexpr size_t INDEX_ROOT_DIRECTORY_ROOT_OFFSET = 160;
	constexpr size_t NEXT_LOGICAL_PAGE_ID_OFFSET = 176;
	constexpr size_t NEXT_LOGICAL_SEGMENT_ID_OFFSET = 184;
	constexpr size_t VECTOR_LOCATION_DIRECTORY_ROOT_OFFSET = 192;
	constexpr size_t VECTOR_MANIFEST_DIRECTORY_ROOT_OFFSET = 208;
	constexpr size_t STATE_DIRECTORY_ROOT_OFFSET = 224;
	constexpr size_t NEXT_LOGICAL_NODE_ID_OFFSET = 240;
	constexpr size_t NEXT_LOGICAL_EDGE_ID_OFFSET = 248;
	constexpr size_t NEXT_PROPERTY_KEY_TOKEN_OFFSET = 256;
	constexpr size_t NEXT_SCHEMA_TOKEN_ID_OFFSET = 264;
	constexpr size_t NEXT_LOGICAL_VECTOR_ID_OFFSET = 272;
	constexpr size_t RETIRE_QUEUE_ROOT_OFFSET = 280;
	constexpr size_t ENTITY_PROPERTY_DIRECTORY_ROOT_OFFSET = 296;
	constexpr size_t LARGE_VALUE_MANIFEST_DIRECTORY_ROOT_OFFSET = 312;
	constexpr size_t NEXT_LARGE_VALUE_ID_OFFSET = 328;
	constexpr size_t METADATA_EMERGENCY_RESERVE_OFFSET = RSS_SUPERBLOCK_HEADER_BYTES;

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

	Superblock validSuperblock(uint64_t generation, uint64_t checkpointLsn) {
		Superblock result;
		result.database_id = DatabaseId(0x1020304050607080ULL);
		result.generation = Generation(generation);
		result.checkpoint_lsn = LogSequenceNumber(checkpointLsn);
		result.catalog_root = pageRef(10, 1, generation);
		result.page_directory_root = pageRef(11, 2, generation);
		result.extent_directory_root = pageRef(12, 3, generation);
		result.allocator_root = pageRef(13, 4, generation);
		result.node_entity_directory_root = pageRef(14, 5, generation);
		result.edge_entity_directory_root = pageRef(15, 6, generation);
		result.entity_property_directory_root = pageRef(23, 14, generation);
		result.index_root_directory_root = pageRef(17, 8, generation);
		result.vector_location_directory_root = pageRef(18, 9, generation);
		result.vector_manifest_directory_root = pageRef(19, 10, generation);
		result.large_value_manifest_directory_root = pageRef(24, 15, generation);
		result.state_directory_root = pageRef(21, 12, generation);
		result.retire_queue_root = pageRef(22, 13, generation);
		result.next_logical_page_id = 101;
		result.next_logical_segment_id = 201;
		result.next_logical_node_id = 301;
		result.next_logical_edge_id = 401;
		result.next_logical_vector_id = 551;
		result.next_large_value_id = 571;
		result.next_property_key_token = 601;
		result.next_schema_token_id = 701;
		return result;
	}

	void recalculateCrc(std::array<std::byte, RSS_SUPERBLOCK_BYTES> &bytes) {
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET,
									graph::utils::calculateCrc(bytes.data() + RSS_SUPERBLOCK_HEADER_BYTES,
															   bytes.size() - RSS_SUPERBLOCK_HEADER_BYTES));
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET,
									graph::utils::calculateCrc(bytes.data(), RSS_SUPERBLOCK_HEADER_BYTES));
	}

} // namespace

TEST(RelocatableSuperblockTest, CodecMatchesGoldenFieldsAndRoundTrips) {
	const auto encoded = SuperblockCodec::encode(validSuperblock(7, 50));
	EXPECT_EQ(encoded.size(), RSS_SUPERBLOCK_BYTES);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[0]), 0x5AU);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[1]), 0x59U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[2]), 0x58U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[3]), 0x53U);
	EXPECT_EQ(std::to_integer<uint8_t>(encoded[4]), RSS_SUPERBLOCK_VERSION);
	EXPECT_EQ(LittleEndianCodec::readU16(encoded, 6), RSS_SUPERBLOCK_HEADER_BYTES);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, DATA_REGION_OFFSET), RSS_FILE_BOOTSTRAP_BYTES);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, CATALOG_ROOT_OFFSET), 10U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, CATALOG_ROOT_OFFSET + sizeof(uint64_t)), (7U << 5U) | 1U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, NODE_ENTITY_DIRECTORY_ROOT_OFFSET), 14U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, EDGE_ENTITY_DIRECTORY_ROOT_OFFSET), 15U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, ENTITY_PROPERTY_DIRECTORY_ROOT_OFFSET), 23U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, INDEX_ROOT_DIRECTORY_ROOT_OFFSET), 17U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, NEXT_LOGICAL_PAGE_ID_OFFSET), 101U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, NEXT_LOGICAL_SEGMENT_ID_OFFSET), 201U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, VECTOR_LOCATION_DIRECTORY_ROOT_OFFSET), 18U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, VECTOR_MANIFEST_DIRECTORY_ROOT_OFFSET), 19U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, LARGE_VALUE_MANIFEST_DIRECTORY_ROOT_OFFSET), 24U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, STATE_DIRECTORY_ROOT_OFFSET), 21U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, RETIRE_QUEUE_ROOT_OFFSET), 22U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, NEXT_LOGICAL_NODE_ID_OFFSET), 301U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, NEXT_LOGICAL_EDGE_ID_OFFSET), 401U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, NEXT_LOGICAL_VECTOR_ID_OFFSET), 551U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, NEXT_LARGE_VALUE_ID_OFFSET), 571U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, NEXT_PROPERTY_KEY_TOKEN_OFFSET), 601U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, NEXT_SCHEMA_TOKEN_ID_OFFSET), 701U);
	EXPECT_NE(LittleEndianCodec::readU32(encoded, PAYLOAD_CRC_OFFSET), 0U);
	EXPECT_NE(LittleEndianCodec::readU32(encoded, HEADER_CRC_OFFSET), 0U);

	const Superblock decoded = SuperblockCodec::decode(encoded);
	EXPECT_EQ(decoded.database_id, DatabaseId(0x1020304050607080ULL));
	EXPECT_EQ(decoded.generation, Generation(7));
	EXPECT_EQ(decoded.checkpoint_lsn, LogSequenceNumber(50));
	EXPECT_EQ(decoded.data_region_offset, RSS_FILE_BOOTSTRAP_BYTES);
	EXPECT_EQ(decoded.catalog_root.physical_slot, PhysicalSlotId(10));
	EXPECT_EQ(decoded.catalog_root.frame_index, 1U);
	EXPECT_EQ(decoded.catalog_root.generation, Generation(7));
	EXPECT_EQ(decoded.allocator_root.physical_slot, PhysicalSlotId(13));
	EXPECT_EQ(decoded.node_entity_directory_root, pageRef(14, 5, 7));
	EXPECT_EQ(decoded.edge_entity_directory_root, pageRef(15, 6, 7));
	EXPECT_EQ(decoded.entity_property_directory_root, pageRef(23, 14, 7));
	EXPECT_EQ(decoded.index_root_directory_root, pageRef(17, 8, 7));
	EXPECT_EQ(decoded.vector_location_directory_root, pageRef(18, 9, 7));
	EXPECT_EQ(decoded.vector_manifest_directory_root, pageRef(19, 10, 7));
	EXPECT_EQ(decoded.large_value_manifest_directory_root, pageRef(24, 15, 7));
	EXPECT_EQ(decoded.state_directory_root, pageRef(21, 12, 7));
	EXPECT_EQ(decoded.retire_queue_root, pageRef(22, 13, 7));
	EXPECT_EQ(decoded.next_logical_page_id, 101U);
	EXPECT_EQ(decoded.next_logical_segment_id, 201U);
	EXPECT_EQ(decoded.next_logical_node_id, 301U);
	EXPECT_EQ(decoded.next_logical_edge_id, 401U);
	EXPECT_EQ(decoded.next_logical_vector_id, 551U);
	EXPECT_EQ(decoded.next_large_value_id, 571U);
	EXPECT_EQ(decoded.next_property_key_token, 601U);
	EXPECT_EQ(decoded.next_schema_token_id, 701U);
}

TEST(RelocatableSuperblockTest, RejectsOrCanonicallyRoundTripsDeterministicFuzzedPages) {
	// Keep the codec fuzz corpus deterministic so it runs identically on every
	// supported platform and remains suitable for the regular unit-test suite.
	std::mt19937_64 random(0x535559585F534255ULL);
	for (size_t trial = 0U; trial < 256U; ++trial) {
		std::array<std::byte, RSS_SUPERBLOCK_BYTES> bytes;
		for (std::byte &byte: bytes) {
			byte = static_cast<std::byte>(random() & 0xFFU);
		}
		try {
			const Superblock decoded = SuperblockCodec::decode(bytes);
			EXPECT_EQ(SuperblockCodec::encode(decoded), bytes) << "random corpus trial " << trial;
		} catch (const FormatException &) {
			// Random on-disk bytes must fail closed through the codec's typed error.
		}
	}

	const auto encoded = SuperblockCodec::encode(validSuperblock(7U, 50U));
	for (size_t trial = 0U; trial < 256U; ++trial) {
		auto mutated = encoded;
		const size_t offset = static_cast<size_t>(random() % mutated.size());
		const auto mask = static_cast<uint8_t>((random() & 0xFFU) | 1U);
		mutated[offset] ^= static_cast<std::byte>(mask);
		recalculateCrc(mutated);
		try {
			const Superblock decoded = SuperblockCodec::decode(mutated);
			EXPECT_EQ(SuperblockCodec::encode(decoded), mutated) << "mutated corpus trial " << trial;
		} catch (const FormatException &) {
			// A checksum-valid mutation is accepted only when all structural checks pass.
		}
	}
}

TEST(RelocatableSuperblockTest, RejectsOrExactlyRoundTripsEveryChecksumValidSingleBitMutation) {
	const auto encoded = SuperblockCodec::encode(validSuperblock(7U, 50U));
	for (size_t byteOffset = 0U; byteOffset < encoded.size(); ++byteOffset) {
		for (uint8_t bit = 0U; bit < 8U; ++bit) {
			auto mutation = encoded;
			mutation[byteOffset] ^= static_cast<std::byte>(uint8_t{1U} << bit);
			recalculateCrc(mutation);
			try {
				const Superblock decoded = SuperblockCodec::decode(mutation);
				EXPECT_EQ(SuperblockCodec::encode(decoded), mutation)
						<< "checksum-valid mutation at byte " << byteOffset << ", bit "
						<< static_cast<unsigned>(bit);
			} catch (const FormatException &) {
				// All rejected mutations must stop at the typed format boundary.
			}
		}
	}
}

TEST(RelocatableSuperblockTest, PersistsAndValidatesTheMetadataEmergencyReserveDescriptor) {
	Superblock superblock = validSuperblock(7, 50);
	superblock.metadata_emergency_reserve = {PhysicalSlotId(2),
											 SegmentId(RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN), Generation(7),
											 LogSequenceNumber(50)};
	const auto encoded = SuperblockCodec::encode(superblock);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, METADATA_EMERGENCY_RESERVE_OFFSET), 2U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, METADATA_EMERGENCY_RESERVE_OFFSET + 8U),
			  RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, METADATA_EMERGENCY_RESERVE_OFFSET + 16U), 7U);
	EXPECT_EQ(LittleEndianCodec::readU64(encoded, METADATA_EMERGENCY_RESERVE_OFFSET + 24U), 50U);
	EXPECT_EQ(SuperblockCodec::decode(encoded).metadata_emergency_reserve, superblock.metadata_emergency_reserve);

	auto payloadCorrupt = encoded;
	payloadCorrupt[METADATA_EMERGENCY_RESERVE_OFFSET + 8U] ^= std::byte{0x01U};
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(payloadCorrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	Superblock partial = validSuperblock(7, 50);
	partial.metadata_emergency_reserve.physical_slot = PhysicalSlotId(2);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(partial)); }, FormatError::FERR_INVALID_HEADER);

	Superblock bootstrapToken = superblock;
	bootstrapToken.metadata_emergency_reserve.pool_token = SegmentId(RSS_BOOTSTRAP_METADATA_SEGMENT_ID);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(bootstrapToken)); },
					  FormatError::FERR_INVALID_HEADER);

	Superblock bootstrapSlot = superblock;
	bootstrapSlot.metadata_emergency_reserve.physical_slot = PhysicalSlotId(1);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(bootstrapSlot)); },
					  FormatError::FERR_INVALID_HEADER);

	auto partialBytes = SuperblockCodec::encode(validSuperblock(7, 50));
	LittleEndianCodec::writeU64(partialBytes, METADATA_EMERGENCY_RESERVE_OFFSET, 2U);
	recalculateCrc(partialBytes);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(partialBytes)); },
					  FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableSuperblockTest, ClassifiesEveryEmergencyReserveDescriptorCompletenessBoundary) {
	MetadataEmergencyReserveDescriptor absent;
	EXPECT_TRUE(absent.isAbsent());
	EXPECT_FALSE(absent.isValid());

	auto poolOnly = absent;
	poolOnly.pool_token = SegmentId(RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN);
	EXPECT_FALSE(poolOnly.isAbsent());

	auto generationOnly = absent;
	generationOnly.generation = Generation(1U);
	EXPECT_FALSE(generationOnly.isAbsent());

	auto lsnOnly = absent;
	lsnOnly.committed_lsn = LogSequenceNumber(1U);
	EXPECT_FALSE(lsnOnly.isAbsent());

	MetadataEmergencyReserveDescriptor valid{PhysicalSlotId(2U),
										  SegmentId(RSS_INITIAL_METADATA_EMERGENCY_RESERVE_POOL_TOKEN), Generation(1U),
										  LogSequenceNumber(1U)};
	EXPECT_TRUE(valid.isValid());

	auto missingGeneration = valid;
	missingGeneration.generation = Generation{};
	EXPECT_FALSE(missingGeneration.isValid());

	auto missingLsn = valid;
	missingLsn.committed_lsn = LogSequenceNumber{};
	EXPECT_FALSE(missingLsn.isValid());
}

TEST(RelocatableSuperblockTest, AllowsEmptyRootsAndSelectsTheNewestValidGeneration) {
	Superblock emptyRoots;
	emptyRoots.database_id = DatabaseId(0x1020304050607080ULL);
	emptyRoots.generation = Generation(1);
	EXPECT_EQ(emptyRoots.next_logical_vector_id, 1U);
	EXPECT_EQ(emptyRoots.next_large_value_id, 1U);
	EXPECT_EQ(emptyRoots.next_property_key_token, 1U);
	EXPECT_EQ(emptyRoots.next_schema_token_id, 1U);
	const Superblock decodedEmpty = SuperblockCodec::decode(SuperblockCodec::encode(emptyRoots));
	EXPECT_FALSE(decodedEmpty.catalog_root.isValid());
	EXPECT_FALSE(decodedEmpty.page_directory_root.isValid());
	EXPECT_FALSE(decodedEmpty.node_entity_directory_root.isValid());
	EXPECT_FALSE(decodedEmpty.edge_entity_directory_root.isValid());
	EXPECT_FALSE(decodedEmpty.entity_property_directory_root.isValid());
	EXPECT_FALSE(decodedEmpty.index_root_directory_root.isValid());
	EXPECT_FALSE(decodedEmpty.vector_location_directory_root.isValid());
	EXPECT_FALSE(decodedEmpty.vector_manifest_directory_root.isValid());
	EXPECT_FALSE(decodedEmpty.large_value_manifest_directory_root.isValid());
	EXPECT_FALSE(decodedEmpty.state_directory_root.isValid());
	EXPECT_EQ(decodedEmpty.next_logical_page_id, 1U);
	EXPECT_EQ(decodedEmpty.next_logical_segment_id, 1U);
	EXPECT_EQ(decodedEmpty.next_logical_node_id, 1U);
	EXPECT_EQ(decodedEmpty.next_logical_edge_id, 1U);
	EXPECT_EQ(decodedEmpty.next_logical_vector_id, 1U);
	EXPECT_EQ(decodedEmpty.next_large_value_id, 1U);
	EXPECT_EQ(decodedEmpty.next_property_key_token, 1U);
	EXPECT_EQ(decodedEmpty.next_schema_token_id, 1U);

	const auto first = SuperblockCodec::encode(validSuperblock(8, 40));
	const auto second = SuperblockCodec::encode(validSuperblock(9, 41));
	const Superblock selected = SuperblockCodec::selectNewest(first, second);
	EXPECT_EQ(selected.generation, Generation(9));
	EXPECT_EQ(selected.checkpoint_lsn, LogSequenceNumber(41));
}

TEST(RelocatableSuperblockTest, PersistsTheTypedPageDirectoryBootstrapFlag) {
	Superblock superblock = validSuperblock(7U, 50U);
	superblock.flags = RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP;
	const auto encoded = SuperblockCodec::encode(superblock);
	EXPECT_EQ(LittleEndianCodec::readU32(encoded, FLAGS_OFFSET), RSS_SUPERBLOCK_FLAG_PAGE_DIRECTORY_BOOTSTRAP);
	const Superblock decoded = SuperblockCodec::decode(encoded);
	EXPECT_TRUE(decoded.usesPageDirectoryBootstrap());

	superblock.flags |= 1U << 31U;
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(superblock)); },
					  FormatError::FERR_RESERVED_BITS);
}

TEST(RelocatableSuperblockTest, RejectsInvalidInMemoryValues) {
	Superblock superblock = validSuperblock(1, 1);
	superblock.database_id = DatabaseId{};
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(superblock)); },
					  FormatError::FERR_INVALID_HEADER);

	superblock = validSuperblock(1, 1);
	superblock.generation = Generation{};
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(superblock)); },
					  FormatError::FERR_INVALID_HEADER);

	superblock = validSuperblock(1, 1);
	superblock.data_region_offset = RSS_FILE_BOOTSTRAP_BYTES - 1U;
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(superblock)); },
					  FormatError::FERR_INVALID_HEADER);

	superblock = validSuperblock(1, 1);
	superblock.data_region_offset = 2U * RSS_SEGMENT_SIZE + 1U;
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(superblock)); },
					  FormatError::FERR_INVALID_HEADER);

	superblock = validSuperblock(1, 1);
	superblock.catalog_root.frame_index = 0;
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(superblock)); },
					  FormatError::FERR_INVALID_HEADER);

	superblock = validSuperblock(1, 1);
	superblock.catalog_root = PhysicalPageRef{};
	superblock.catalog_root.frame_index = 1;
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(superblock)); },
					  FormatError::FERR_INVALID_HEADER);

	superblock = validSuperblock(1, 1);
	superblock.catalog_root.generation = Generation(std::numeric_limits<uint64_t>::max());
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(superblock)); },
					  FormatError::FERR_INVALID_HEADER);

	superblock = validSuperblock(1, 1);
	superblock.index_root_directory_root.frame_index = 0;
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(superblock)); },
					  FormatError::FERR_INVALID_HEADER);

	superblock = validSuperblock(1, 1);
	superblock.vector_manifest_directory_root.frame_index = 0;
	expectFormatError([&] { static_cast<void>(SuperblockCodec::encode(superblock)); },
					  FormatError::FERR_INVALID_HEADER);

}

TEST(RelocatableSuperblockTest, RejectsTruncatedChecksummedAndMalformedDiskBytes) {
	const auto encoded = SuperblockCodec::encode(validSuperblock(3, 9));
	expectFormatError(
			[&] { static_cast<void>(SuperblockCodec::decode(std::span(encoded).first(RSS_SUPERBLOCK_BYTES - 1U))); },
			FormatError::FERR_BUFFER_TOO_SMALL);
	std::vector<std::byte> oversized(encoded.begin(), encoded.end());
	oversized.push_back(std::byte{0});
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(oversized)); }, FormatError::FERR_INVALID_HEADER);

	auto corrupt = encoded;
	corrupt[RSS_SUPERBLOCK_HEADER_BYTES] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	corrupt[0] ^= std::byte{0x01};
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 0, 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); }, FormatError::FERR_INVALID_MAGIC);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 4, RSS_SUPERBLOCK_VERSION + 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); },
					  FormatError::FERR_UNSUPPORTED_VERSION);
	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 4, RSS_SUPERBLOCK_VERSION - 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); },
					  FormatError::FERR_UNSUPPORTED_VERSION);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, FLAGS_OFFSET, RSS_SUPERBLOCK_KNOWN_FLAGS | (1U << 31U));
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); }, FormatError::FERR_RESERVED_BITS);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 12, 1);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); }, FormatError::FERR_RESERVED_BITS);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, DATABASE_ID_OFFSET, 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, SEGMENT_SIZE_OFFSET, RSS_SEGMENT_SIZE - 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU16(corrupt, 6, RSS_SUPERBLOCK_HEADER_BYTES - 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU32(corrupt, 36, RSS_PAGE_SIZE - 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, DATA_REGION_OFFSET, RSS_FILE_BOOTSTRAP_BYTES + 1U);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, CATALOG_ROOT_OFFSET + sizeof(uint64_t), 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, CATALOG_ROOT_OFFSET, 0);
	LittleEndianCodec::writeU64(corrupt, CATALOG_ROOT_OFFSET + sizeof(uint64_t), 1);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, INDEX_ROOT_DIRECTORY_ROOT_OFFSET + sizeof(uint64_t), 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);

	corrupt = encoded;
	LittleEndianCodec::writeU64(corrupt, VECTOR_LOCATION_DIRECTORY_ROOT_OFFSET + sizeof(uint64_t), 0);
	recalculateCrc(corrupt);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::decode(corrupt)); }, FormatError::FERR_INVALID_HEADER);
}

TEST(RelocatableSuperblockTest, AAndBSelectionSurvivesOneCorruptCopyAndRejectsSplitBrain) {
	const auto older = SuperblockCodec::encode(validSuperblock(4, 20));
	const auto newer = SuperblockCodec::encode(validSuperblock(5, 21));
	auto corrupt = newer;
	corrupt[0] ^= std::byte{0xFF};
	EXPECT_EQ(SuperblockCodec::selectNewest(older, corrupt).generation, Generation(4));
	EXPECT_EQ(SuperblockCodec::selectNewest(corrupt, newer).generation, Generation(5));
	EXPECT_EQ(SuperblockCodec::selectNewest(newer, older).generation, Generation(5));
	EXPECT_EQ(SuperblockCodec::selectNewest(older, older).generation, Generation(4));

	Superblock incompatible = validSuperblock(4, 22);
	expectFormatError(
			[&] { static_cast<void>(SuperblockCodec::selectNewest(older, SuperblockCodec::encode(incompatible))); },
			FormatError::FERR_INVALID_STATE);
	expectFormatError([&] { static_cast<void>(SuperblockCodec::selectNewest(corrupt, corrupt)); },
					  FormatError::FERR_CHECKSUM_MISMATCH);
}
