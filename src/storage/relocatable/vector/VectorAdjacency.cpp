/**
 * @file VectorAdjacency.cpp
 * @brief Explicit variable-length DiskANN adjacency-record codec for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/VectorAdjacency.hpp"

#include <algorithm>
#include <array>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t NEIGHBOR_COUNT_OFFSET = 8;
		constexpr size_t FLAGS_OFFSET = 10;
		constexpr size_t VECTOR_ID_OFFSET = 12;
		constexpr size_t PAYLOAD_CRC_OFFSET = 20;
		constexpr size_t HEADER_CRC_OFFSET = 24;
		constexpr size_t RESERVED_OFFSET = 28;

		[[noreturn]] void throwAdjacencyError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint32_t checksum(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		void validateRecord(const VectorAdjacencyRecord &record) {
			const uint8_t invalid =
					static_cast<uint8_t>(record.vector_id == 0U) |
					static_cast<uint8_t>(record.neighbors.size() > RSS_VECTOR_ADJACENCY_MAX_NEIGHBORS);
			if (invalid != 0U) {
				throwAdjacencyError(FormatError::FERR_INVALID_HEADER,
									"relocatable vector adjacency has an invalid vector ID or neighbor count");
			}
			uint64_t previous = 0;
			for (const uint64_t neighbor: record.neighbors) {
				const uint8_t invalidNeighbor = static_cast<uint8_t>(neighbor == 0U) |
												static_cast<uint8_t>(neighbor == record.vector_id) |
												static_cast<uint8_t>(neighbor <= previous);
				if (invalidNeighbor != 0U) {
					throwAdjacencyError(
							FormatError::FERR_INVALID_HEADER,
							"relocatable vector adjacency neighbors must be nonzero, non-self, and strictly ordered");
				}
				previous = neighbor;
			}
		}

	} // namespace

	std::vector<std::byte> VectorAdjacencyCodec::encode(const VectorAdjacencyRecord &record) {
		validateRecord(record);
		const size_t payloadBytes = record.neighbors.size() * RSS_VECTOR_ADJACENCY_NEIGHBOR_BYTES;
		std::vector<std::byte> bytes(RSS_VECTOR_ADJACENCY_HEADER_BYTES + payloadBytes);
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_VECTOR_ADJACENCY_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_VECTOR_ADJACENCY_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_VECTOR_ADJACENCY_HEADER_BYTES);
		LittleEndianCodec::writeU16(bytes, NEIGHBOR_COUNT_OFFSET, static_cast<uint16_t>(record.neighbors.size()));
		LittleEndianCodec::writeU16(bytes, FLAGS_OFFSET, 0);
		LittleEndianCodec::writeU64(bytes, VECTOR_ID_OFFSET, record.vector_id);
		for (size_t index = 0; index < record.neighbors.size(); ++index) {
			LittleEndianCodec::writeU64(
					bytes, RSS_VECTOR_ADJACENCY_HEADER_BYTES + index * RSS_VECTOR_ADJACENCY_NEIGHBOR_BYTES,
					record.neighbors[index]);
		}
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET,
									checksum(std::span(bytes).subspan(RSS_VECTOR_ADJACENCY_HEADER_BYTES)));
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET,
									checksum(std::span(bytes).first(RSS_VECTOR_ADJACENCY_HEADER_BYTES)));
		return bytes;
	}

	VectorAdjacencyRecord VectorAdjacencyCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_VECTOR_ADJACENCY_HEADER_BYTES) {
			throwAdjacencyError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable vector adjacency record is truncated");
		}
		std::array<std::byte, RSS_VECTOR_ADJACENCY_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0);
		if (checksum(header) != expectedHeaderCrc) {
			throwAdjacencyError(FormatError::FERR_CHECKSUM_MISMATCH,
								"relocatable vector adjacency header checksum does not match");
		}
		const uint16_t neighborCount = LittleEndianCodec::readU16(bytes, NEIGHBOR_COUNT_OFFSET);
		const uint64_t expectedBytes = static_cast<uint64_t>(RSS_VECTOR_ADJACENCY_HEADER_BYTES) +
									   static_cast<uint64_t>(neighborCount) * RSS_VECTOR_ADJACENCY_NEIGHBOR_BYTES;
		const uint8_t invalidHeader =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_VECTOR_ADJACENCY_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) !=
									 RSS_VECTOR_ADJACENCY_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
									 RSS_VECTOR_ADJACENCY_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, RESERVED_OFFSET) != 0U) |
				static_cast<uint8_t>(neighborCount > RSS_VECTOR_ADJACENCY_MAX_NEIGHBORS) |
				static_cast<uint8_t>(expectedBytes != bytes.size());
		if (invalidHeader != 0U) {
			throwAdjacencyError(FormatError::FERR_INVALID_HEADER, "relocatable vector adjacency header is invalid");
		}
		if (checksum(bytes.subspan(RSS_VECTOR_ADJACENCY_HEADER_BYTES)) !=
			LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			throwAdjacencyError(FormatError::FERR_CHECKSUM_MISMATCH,
								"relocatable vector adjacency payload checksum does not match");
		}
		VectorAdjacencyRecord record;
		record.vector_id = LittleEndianCodec::readU64(bytes, VECTOR_ID_OFFSET);
		record.neighbors.reserve(neighborCount);
		for (uint16_t index = 0; index < neighborCount; ++index) {
			record.neighbors.push_back(LittleEndianCodec::readU64(
					bytes, RSS_VECTOR_ADJACENCY_HEADER_BYTES +
								   static_cast<size_t>(index) * RSS_VECTOR_ADJACENCY_NEIGHBOR_BYTES));
		}
		validateRecord(record);
		return record;
	}

} // namespace graph::storage
