/**
 * @file VectorCodebook.cpp
 * @brief Explicit immutable codebook-extent header codec for relocatable vectors.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/VectorCodebook.hpp"

#include <algorithm>
#include <limits>

#include "graph/storage/relocatable/vector/VectorCodebookExtent.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t ELEMENT_TYPE_OFFSET = 8;
		constexpr size_t FLAGS_OFFSET = 10;
		constexpr size_t DIMENSIONS_OFFSET = 12;
		constexpr size_t CENTROID_COUNT_OFFSET = 16;
		constexpr size_t ELEMENT_BYTES_OFFSET = 20;
		constexpr size_t PAYLOAD_BYTES_OFFSET = 24;
		constexpr size_t PAYLOAD_CRC_OFFSET = 28;
		constexpr size_t HEADER_CRC_OFFSET = 32;
		constexpr size_t SUBSPACE_COUNT_OFFSET = 36;
		constexpr size_t CODEBOOK_ID_OFFSET = 40;
		constexpr size_t GENERATION_OFFSET = 48;
		constexpr size_t TRAILING_RESERVED_OFFSET = 56;

		[[noreturn]] void throwCodebookError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		[[nodiscard]] uint32_t checksum(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		[[nodiscard]] uint32_t requiredElementBytes(VectorCodebookElementType type) {
			switch (type) {
				case VectorCodebookElementType::VCET_FLOAT32:
					return 4;
				case VectorCodebookElementType::VCET_UINT8:
					return 1;
			}
			throwCodebookError(FormatError::FERR_INVALID_ENUM, "relocatable vector codebook uses an unknown element type");
		}

		[[nodiscard]] uint32_t expectedPayloadBytes(const VectorCodebookHeader &header) {
			const uint64_t values = static_cast<uint64_t>(header.dimensions) * header.centroid_count;
			const uint64_t bytes = values * header.element_bytes;
			if (bytes > std::numeric_limits<uint32_t>::max()) {
				throwCodebookError(FormatError::FERR_ARITHMETIC_OVERFLOW,
								   "relocatable vector codebook payload exceeds its persistent length field");
			}
			return static_cast<uint32_t>(bytes);
		}

		void validateHeader(const VectorCodebookHeader &header) {
			const uint8_t invalid =
					static_cast<uint8_t>(!VectorCodebookExtentIdCodec::canEncodeCodebookId(header.codebook_id)) |
					static_cast<uint8_t>(!header.generation.isValid()) | static_cast<uint8_t>(header.dimensions == 0U) |
					static_cast<uint8_t>(header.subspace_count == 0U) |
					static_cast<uint8_t>(header.subspace_count > header.dimensions) |
					static_cast<uint8_t>(header.dimensions % header.subspace_count != 0U) |
					static_cast<uint8_t>(header.centroid_count == 0U) |
					static_cast<uint8_t>(header.element_bytes != requiredElementBytes(header.element_type)) |
					static_cast<uint8_t>(header.payload_bytes != expectedPayloadBytes(header));
			if (invalid != 0U) {
				throwCodebookError(FormatError::FERR_INVALID_HEADER,
								   "relocatable vector codebook header has invalid identity, shape, or payload length");
			}
		}

	} // namespace

	VectorCodebookHeaderCodec::Bytes VectorCodebookHeaderCodec::encode(const VectorCodebookHeader &header) {
		validateHeader(header);
		Bytes bytes{};
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_VECTOR_CODEBOOK_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_VECTOR_CODEBOOK_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_VECTOR_CODEBOOK_HEADER_BYTES);
		LittleEndianCodec::writeU16(bytes, ELEMENT_TYPE_OFFSET, static_cast<uint16_t>(header.element_type));
		LittleEndianCodec::writeU16(bytes, FLAGS_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes, DIMENSIONS_OFFSET, header.dimensions);
		LittleEndianCodec::writeU32(bytes, CENTROID_COUNT_OFFSET, header.centroid_count);
		LittleEndianCodec::writeU32(bytes, ELEMENT_BYTES_OFFSET, header.element_bytes);
		LittleEndianCodec::writeU32(bytes, PAYLOAD_BYTES_OFFSET, header.payload_bytes);
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET, header.payload_crc);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes, SUBSPACE_COUNT_OFFSET, header.subspace_count);
		LittleEndianCodec::writeU64(bytes, CODEBOOK_ID_OFFSET, header.codebook_id);
		LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, header.generation.value());
		LittleEndianCodec::writeU64(bytes, TRAILING_RESERVED_OFFSET, 0);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, checksum(bytes));
		return bytes;
	}

	VectorCodebookHeader VectorCodebookHeaderCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_VECTOR_CODEBOOK_HEADER_BYTES) {
			throwCodebookError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable vector codebook header is truncated");
		}
		if (bytes.size() != RSS_VECTOR_CODEBOOK_HEADER_BYTES) {
			throwCodebookError(FormatError::FERR_INVALID_HEADER, "relocatable vector codebook header has an invalid size");
		}
		Bytes copied{};
		std::copy_n(bytes.begin(), copied.size(), copied.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(copied, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(copied, HEADER_CRC_OFFSET, 0);
		if (checksum(copied) != expectedHeaderCrc) {
			throwCodebookError(FormatError::FERR_CHECKSUM_MISMATCH,
							   "relocatable vector codebook header checksum does not match");
		}
		const uint8_t invalidStatic =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_VECTOR_CODEBOOK_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) !=
									 RSS_VECTOR_CODEBOOK_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
									 RSS_VECTOR_CODEBOOK_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, TRAILING_RESERVED_OFFSET) != 0U);
		if (invalidStatic != 0U) {
			throwCodebookError(FormatError::FERR_INVALID_HEADER, "relocatable vector codebook header is invalid");
		}
		VectorCodebookHeader header;
		header.codebook_id = LittleEndianCodec::readU64(bytes, CODEBOOK_ID_OFFSET);
		header.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		header.element_type = VectorCodebookElementType(LittleEndianCodec::readU16(bytes, ELEMENT_TYPE_OFFSET));
		header.dimensions = LittleEndianCodec::readU32(bytes, DIMENSIONS_OFFSET);
		header.subspace_count = LittleEndianCodec::readU32(bytes, SUBSPACE_COUNT_OFFSET);
		header.centroid_count = LittleEndianCodec::readU32(bytes, CENTROID_COUNT_OFFSET);
		header.element_bytes = LittleEndianCodec::readU32(bytes, ELEMENT_BYTES_OFFSET);
		header.payload_bytes = LittleEndianCodec::readU32(bytes, PAYLOAD_BYTES_OFFSET);
		header.payload_crc = LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET);
		validateHeader(header);
		return header;
	}

	void VectorCodebookHeaderCodec::validatePayload(const VectorCodebookHeader &header,
													std::span<const std::byte> payload) {
		validateHeader(header);
		if (payload.size() != header.payload_bytes) {
			throwCodebookError(FormatError::FERR_INVALID_HEADER,
							   "relocatable vector codebook payload length disagrees with its immutable header");
		}
		if (checksum(payload) != header.payload_crc) {
			throwCodebookError(FormatError::FERR_CHECKSUM_MISMATCH,
							   "relocatable vector codebook payload checksum does not match");
		}
	}

} // namespace graph::storage
