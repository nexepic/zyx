/**
 * @file Superblock.cpp
 * @brief Explicit codec and A/B selection for relocatable bootstrap superblocks.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/format/Superblock.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <tuple>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t FORMAT_VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t FLAGS_OFFSET = 8;
		constexpr size_t RESERVED0_OFFSET = 12;
		constexpr size_t GENERATION_OFFSET = 16;
		constexpr size_t CHECKPOINT_LSN_OFFSET = 24;
		constexpr size_t SEGMENT_SIZE_OFFSET = 32;
		constexpr size_t PAGE_SIZE_OFFSET = 36;
		constexpr size_t DATA_REGION_OFFSET = 40;
		constexpr size_t CATALOG_ROOT_OFFSET = 48;
		constexpr size_t PAGE_DIRECTORY_ROOT_OFFSET = 64;
		constexpr size_t EXTENT_DIRECTORY_ROOT_OFFSET = 80;
		constexpr size_t FREE_SPACE_ROOT_OFFSET = 96;
		constexpr size_t PAYLOAD_CRC_OFFSET = 112;
		constexpr size_t HEADER_CRC_OFFSET = 116;
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
		constexpr size_t METADATA_EMERGENCY_RESERVE_BYTES = 32;
		constexpr uint64_t RSS_MAX_PHYSICAL_PAGE_GENERATION = std::numeric_limits<uint64_t>::max() >> 5U;
		static_assert(METADATA_EMERGENCY_RESERVE_OFFSET + METADATA_EMERGENCY_RESERVE_BYTES <= RSS_SUPERBLOCK_BYTES);

		[[noreturn]] void throwSuperblockError(FormatError error, const std::string &message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint32_t calculateCrc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		void writePageRef(std::span<std::byte> bytes, size_t offset, const PhysicalPageRef &reference) {
			LittleEndianCodec::writeU64(bytes, offset, reference.physical_slot.value());
			if (!reference.physical_slot.isValid()) {
				LittleEndianCodec::writeU64(bytes, offset + sizeof(uint64_t), 0);
				return;
			}
			const uint64_t packed = (reference.generation.value() << 5U) | reference.frame_index;
			LittleEndianCodec::writeU64(bytes, offset + sizeof(uint64_t), packed);
		}

		[[nodiscard]] PhysicalPageRef readPageRef(std::span<const std::byte> bytes, size_t offset) {
			PhysicalPageRef reference;
			reference.physical_slot = PhysicalSlotId(LittleEndianCodec::readU64(bytes, offset));
			const uint64_t packed = LittleEndianCodec::readU64(bytes, offset + sizeof(uint64_t));
			if (!reference.physical_slot.isValid()) {
				if (packed != 0) {
					throwSuperblockError(FormatError::FERR_INVALID_HEADER,
										 "an absent relocatable superblock root has nonzero location data");
				}
				return reference;
			}
			reference.frame_index = static_cast<uint8_t>(packed & 0x1FU);
			reference.generation = Generation(packed >> 5U);
			return reference;
		}

		void validatePageRef(const PhysicalPageRef &reference) {
			const bool hasSlot = reference.physical_slot.isValid();
			const bool hasLocation = reference.frame_index != 0 || reference.generation.isValid();
			if (!hasSlot && !hasLocation) {
				return;
			}
			if (!reference.isValid() || reference.generation.value() > RSS_MAX_PHYSICAL_PAGE_GENERATION) {
				throwSuperblockError(FormatError::FERR_INVALID_HEADER,
									 "invalid relocatable physical metadata page reference");
			}
		}

		void validateEmergencyReserve(const MetadataEmergencyReserveDescriptor &reserve) {
			if (reserve.isAbsent()) {
				return;
			}
			if (!reserve.isValid() || reserve.generation.value() > RSS_MAX_PHYSICAL_PAGE_GENERATION) {
				throwSuperblockError(FormatError::FERR_INVALID_HEADER,
								 "relocatable metadata emergency reserve descriptor is invalid");
			}
		}

		void validateSuperblock(const Superblock &superblock) {
			if (!superblock.database_id.isValid() || !superblock.generation.isValid()) {
				throwSuperblockError(FormatError::FERR_INVALID_HEADER,
									 "relocatable superblock needs a database ID and generation");
			}
			if ((superblock.flags & ~RSS_SUPERBLOCK_KNOWN_FLAGS) != 0U) {
				throwSuperblockError(FormatError::FERR_RESERVED_BITS,
									 "relocatable Superblock uses unknown lifecycle flags");
			}
			if (superblock.data_region_offset < RSS_FILE_BOOTSTRAP_BYTES ||
				superblock.data_region_offset % RSS_SEGMENT_SIZE != 0) {
				throwSuperblockError(FormatError::FERR_INVALID_HEADER,
									 "relocatable data region must follow the fixed aligned bootstrap area");
			}
			validatePageRef(superblock.catalog_root);
			validatePageRef(superblock.page_directory_root);
			validatePageRef(superblock.extent_directory_root);
			validatePageRef(superblock.allocator_root);
			validatePageRef(superblock.node_entity_directory_root);
			validatePageRef(superblock.edge_entity_directory_root);
			validatePageRef(superblock.entity_property_directory_root);
			validatePageRef(superblock.index_root_directory_root);
			validatePageRef(superblock.vector_location_directory_root);
			validatePageRef(superblock.vector_manifest_directory_root);
			validatePageRef(superblock.large_value_manifest_directory_root);
			validatePageRef(superblock.state_directory_root);
			validatePageRef(superblock.retire_queue_root);
			validateEmergencyReserve(superblock.metadata_emergency_reserve);
		}

		void writeHeader(std::span<std::byte> bytes, const Superblock &superblock, uint32_t payloadCrc,
						 uint32_t headerCrc) {
			LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_SUPERBLOCK_MAGIC);
			LittleEndianCodec::writeU16(bytes, FORMAT_VERSION_OFFSET, RSS_SUPERBLOCK_VERSION);
			LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_SUPERBLOCK_HEADER_BYTES);
			LittleEndianCodec::writeU32(bytes, FLAGS_OFFSET, superblock.flags);
			LittleEndianCodec::writeU32(bytes, RESERVED0_OFFSET, 0);
			LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, superblock.generation.value());
			LittleEndianCodec::writeU64(bytes, CHECKPOINT_LSN_OFFSET, superblock.checkpoint_lsn.value());
			LittleEndianCodec::writeU32(bytes, SEGMENT_SIZE_OFFSET, RSS_SEGMENT_SIZE);
			LittleEndianCodec::writeU32(bytes, PAGE_SIZE_OFFSET, RSS_PAGE_SIZE);
			LittleEndianCodec::writeU64(bytes, DATA_REGION_OFFSET, superblock.data_region_offset);
			writePageRef(bytes, CATALOG_ROOT_OFFSET, superblock.catalog_root);
			writePageRef(bytes, PAGE_DIRECTORY_ROOT_OFFSET, superblock.page_directory_root);
			writePageRef(bytes, EXTENT_DIRECTORY_ROOT_OFFSET, superblock.extent_directory_root);
			writePageRef(bytes, FREE_SPACE_ROOT_OFFSET, superblock.allocator_root);
			writePageRef(bytes, NODE_ENTITY_DIRECTORY_ROOT_OFFSET, superblock.node_entity_directory_root);
			writePageRef(bytes, EDGE_ENTITY_DIRECTORY_ROOT_OFFSET, superblock.edge_entity_directory_root);
			writePageRef(bytes, ENTITY_PROPERTY_DIRECTORY_ROOT_OFFSET, superblock.entity_property_directory_root);
			writePageRef(bytes, INDEX_ROOT_DIRECTORY_ROOT_OFFSET, superblock.index_root_directory_root);
			LittleEndianCodec::writeU64(bytes, NEXT_LOGICAL_PAGE_ID_OFFSET, superblock.next_logical_page_id);
			LittleEndianCodec::writeU64(bytes, NEXT_LOGICAL_SEGMENT_ID_OFFSET, superblock.next_logical_segment_id);
			writePageRef(bytes, VECTOR_LOCATION_DIRECTORY_ROOT_OFFSET, superblock.vector_location_directory_root);
			writePageRef(bytes, VECTOR_MANIFEST_DIRECTORY_ROOT_OFFSET, superblock.vector_manifest_directory_root);
			writePageRef(bytes, LARGE_VALUE_MANIFEST_DIRECTORY_ROOT_OFFSET,
						 superblock.large_value_manifest_directory_root);
			writePageRef(bytes, STATE_DIRECTORY_ROOT_OFFSET, superblock.state_directory_root);
			writePageRef(bytes, RETIRE_QUEUE_ROOT_OFFSET, superblock.retire_queue_root);
			LittleEndianCodec::writeU64(bytes, NEXT_LOGICAL_NODE_ID_OFFSET, superblock.next_logical_node_id);
			LittleEndianCodec::writeU64(bytes, NEXT_LOGICAL_EDGE_ID_OFFSET, superblock.next_logical_edge_id);
			LittleEndianCodec::writeU64(bytes, NEXT_PROPERTY_KEY_TOKEN_OFFSET, superblock.next_property_key_token);
			LittleEndianCodec::writeU64(bytes, NEXT_SCHEMA_TOKEN_ID_OFFSET, superblock.next_schema_token_id);
			LittleEndianCodec::writeU64(bytes, NEXT_LOGICAL_VECTOR_ID_OFFSET, superblock.next_logical_vector_id);
			LittleEndianCodec::writeU64(bytes, NEXT_LARGE_VALUE_ID_OFFSET, superblock.next_large_value_id);
			LittleEndianCodec::writeU64(bytes, METADATA_EMERGENCY_RESERVE_OFFSET,
								 superblock.metadata_emergency_reserve.physical_slot.value());
			LittleEndianCodec::writeU64(bytes, METADATA_EMERGENCY_RESERVE_OFFSET + 8U,
								 superblock.metadata_emergency_reserve.pool_token.value());
			LittleEndianCodec::writeU64(bytes, METADATA_EMERGENCY_RESERVE_OFFSET + 16U,
								 superblock.metadata_emergency_reserve.generation.value());
			LittleEndianCodec::writeU64(bytes, METADATA_EMERGENCY_RESERVE_OFFSET + 24U,
								 superblock.metadata_emergency_reserve.committed_lsn.value());
			LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET, payloadCrc);
			LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, headerCrc);
			LittleEndianCodec::writeU64(bytes, DATABASE_ID_OFFSET, superblock.database_id.value());
		}

		[[nodiscard]] std::optional<Superblock> tryDecode(std::span<const std::byte> bytes) {
			try {
				return SuperblockCodec::decode(bytes);
			} catch (const FormatException &) {
				return std::nullopt;
			}
		}

	} // namespace

	Superblock::Superblock() = default;

	std::array<std::byte, RSS_SUPERBLOCK_BYTES> SuperblockCodec::encode(const Superblock &superblock) {
		validateSuperblock(superblock);
		std::array<std::byte, RSS_SUPERBLOCK_BYTES> bytes{};
		writeHeader(bytes, superblock, 0, 0);
		const uint32_t payloadCrc = calculateCrc(std::span(bytes).subspan(RSS_SUPERBLOCK_HEADER_BYTES));
		writeHeader(bytes, superblock, payloadCrc, 0);
		const uint32_t headerCrc = calculateCrc(std::span(bytes).first(RSS_SUPERBLOCK_HEADER_BYTES));
		writeHeader(bytes, superblock, payloadCrc, headerCrc);
		return bytes;
	}

	Superblock SuperblockCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_SUPERBLOCK_BYTES) {
			throwSuperblockError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable superblock is truncated");
		}
		if (bytes.size() != RSS_SUPERBLOCK_BYTES) {
			throwSuperblockError(FormatError::FERR_INVALID_HEADER, "relocatable superblock has an unexpected size");
		}

		std::array<std::byte, RSS_SUPERBLOCK_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0);
		if (calculateCrc(header) != expectedHeaderCrc) {
			throwSuperblockError(FormatError::FERR_CHECKSUM_MISMATCH,
								 "relocatable superblock header checksum does not match");
		}
		if (LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_SUPERBLOCK_MAGIC) {
			throwSuperblockError(FormatError::FERR_INVALID_MAGIC, "relocatable superblock magic does not match");
		}
		if (LittleEndianCodec::readU16(bytes, FORMAT_VERSION_OFFSET) != RSS_SUPERBLOCK_VERSION) {
			throwSuperblockError(FormatError::FERR_UNSUPPORTED_VERSION, "unsupported relocatable superblock version");
		}
		if (LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) != RSS_SUPERBLOCK_HEADER_BYTES ||
			LittleEndianCodec::readU32(bytes, SEGMENT_SIZE_OFFSET) != RSS_SEGMENT_SIZE ||
			LittleEndianCodec::readU32(bytes, PAGE_SIZE_OFFSET) != RSS_PAGE_SIZE) {
			throwSuperblockError(FormatError::FERR_INVALID_HEADER, "invalid relocatable superblock dimensions");
		}
		const uint32_t flags = LittleEndianCodec::readU32(bytes, FLAGS_OFFSET);
		if ((flags & ~RSS_SUPERBLOCK_KNOWN_FLAGS) != 0U || LittleEndianCodec::readU32(bytes, RESERVED0_OFFSET) != 0) {
			throwSuperblockError(FormatError::FERR_RESERVED_BITS, "relocatable superblock uses reserved bits");
		}
		if (calculateCrc(bytes.subspan(RSS_SUPERBLOCK_HEADER_BYTES)) !=
			LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			throwSuperblockError(FormatError::FERR_CHECKSUM_MISMATCH,
								 "relocatable superblock payload checksum does not match");
		}
		Superblock superblock;
		superblock.flags = flags;
		superblock.database_id = DatabaseId(LittleEndianCodec::readU64(bytes, DATABASE_ID_OFFSET));
		superblock.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		superblock.checkpoint_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, CHECKPOINT_LSN_OFFSET));
		superblock.data_region_offset = LittleEndianCodec::readU64(bytes, DATA_REGION_OFFSET);
		superblock.catalog_root = readPageRef(bytes, CATALOG_ROOT_OFFSET);
		superblock.page_directory_root = readPageRef(bytes, PAGE_DIRECTORY_ROOT_OFFSET);
		superblock.extent_directory_root = readPageRef(bytes, EXTENT_DIRECTORY_ROOT_OFFSET);
		superblock.allocator_root = readPageRef(bytes, FREE_SPACE_ROOT_OFFSET);
		superblock.node_entity_directory_root = readPageRef(bytes, NODE_ENTITY_DIRECTORY_ROOT_OFFSET);
		superblock.edge_entity_directory_root = readPageRef(bytes, EDGE_ENTITY_DIRECTORY_ROOT_OFFSET);
		superblock.entity_property_directory_root = readPageRef(bytes, ENTITY_PROPERTY_DIRECTORY_ROOT_OFFSET);
		superblock.index_root_directory_root = readPageRef(bytes, INDEX_ROOT_DIRECTORY_ROOT_OFFSET);
		superblock.next_logical_page_id = LittleEndianCodec::readU64(bytes, NEXT_LOGICAL_PAGE_ID_OFFSET);
		superblock.next_logical_segment_id = LittleEndianCodec::readU64(bytes, NEXT_LOGICAL_SEGMENT_ID_OFFSET);
		superblock.vector_location_directory_root = readPageRef(bytes, VECTOR_LOCATION_DIRECTORY_ROOT_OFFSET);
		superblock.vector_manifest_directory_root = readPageRef(bytes, VECTOR_MANIFEST_DIRECTORY_ROOT_OFFSET);
		superblock.large_value_manifest_directory_root = readPageRef(bytes, LARGE_VALUE_MANIFEST_DIRECTORY_ROOT_OFFSET);
		superblock.state_directory_root = readPageRef(bytes, STATE_DIRECTORY_ROOT_OFFSET);
		superblock.retire_queue_root = readPageRef(bytes, RETIRE_QUEUE_ROOT_OFFSET);
		superblock.next_logical_node_id = LittleEndianCodec::readU64(bytes, NEXT_LOGICAL_NODE_ID_OFFSET);
		superblock.next_logical_edge_id = LittleEndianCodec::readU64(bytes, NEXT_LOGICAL_EDGE_ID_OFFSET);
		superblock.next_property_key_token = LittleEndianCodec::readU64(bytes, NEXT_PROPERTY_KEY_TOKEN_OFFSET);
		superblock.next_schema_token_id = LittleEndianCodec::readU64(bytes, NEXT_SCHEMA_TOKEN_ID_OFFSET);
		superblock.next_logical_vector_id = LittleEndianCodec::readU64(bytes, NEXT_LOGICAL_VECTOR_ID_OFFSET);
		superblock.next_large_value_id = LittleEndianCodec::readU64(bytes, NEXT_LARGE_VALUE_ID_OFFSET);
		superblock.metadata_emergency_reserve.physical_slot =
				PhysicalSlotId(LittleEndianCodec::readU64(bytes, METADATA_EMERGENCY_RESERVE_OFFSET));
		superblock.metadata_emergency_reserve.pool_token =
				SegmentId(LittleEndianCodec::readU64(bytes, METADATA_EMERGENCY_RESERVE_OFFSET + 8U));
		superblock.metadata_emergency_reserve.generation =
				Generation(LittleEndianCodec::readU64(bytes, METADATA_EMERGENCY_RESERVE_OFFSET + 16U));
		superblock.metadata_emergency_reserve.committed_lsn =
				LogSequenceNumber(LittleEndianCodec::readU64(bytes, METADATA_EMERGENCY_RESERVE_OFFSET + 24U));
		validateSuperblock(superblock);
		return superblock;
	}

	Superblock SuperblockCodec::selectNewest(std::span<const std::byte> first, std::span<const std::byte> second) {
		const std::optional<Superblock> decodedFirst = tryDecode(first);
		const std::optional<Superblock> decodedSecond = tryDecode(second);
		if (!decodedFirst.has_value() && !decodedSecond.has_value()) {
			throwSuperblockError(FormatError::FERR_CHECKSUM_MISMATCH,
								 "neither relocatable bootstrap superblock is valid");
		}
		if (!decodedFirst.has_value()) {
			return *decodedSecond;
		}
		if (!decodedSecond.has_value()) {
			return *decodedFirst;
		}
		if (decodedFirst->generation == decodedSecond->generation) {
			if (*decodedFirst != *decodedSecond) {
				throwSuperblockError(FormatError::FERR_INVALID_STATE,
									 "relocatable bootstrap superblocks disagree at one generation");
			}
			return *decodedFirst;
		}
		return decodedFirst->generation > decodedSecond->generation ? *decodedFirst : *decodedSecond;
	}

} // namespace graph::storage
