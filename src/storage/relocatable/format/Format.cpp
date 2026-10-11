/**
 * @file Format.cpp
 * @brief Explicit relocatable envelope codec implementation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/format/Format.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t FORMAT_VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t KIND_OFFSET = 8;
		constexpr size_t STATE_OFFSET = 10;
		constexpr size_t FLAGS_OFFSET = 12;
		constexpr size_t SEGMENT_ID_OFFSET = 16;
		constexpr size_t OWNER_ID_OFFSET = 24;
		constexpr size_t GENERATION_OFFSET = 32;
		constexpr size_t COMMITTED_LSN_OFFSET = 40;
		constexpr size_t PAYLOAD_BYTES_OFFSET = 48;
		constexpr size_t HEADER_CRC_OFFSET = 52;
		constexpr size_t PAYLOAD_CRC_OFFSET = 56;
		constexpr size_t RESERVED_OFFSET = 60;

		[[noreturn]] void throwFormatError(FormatError error, const std::string &message) {
			throw FormatException(error, message);
		}

		void requireRange(size_t size, size_t offset, size_t width) {
			if (offset > size || width > size - offset) {
				throwFormatError(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable codec buffer is too small");
			}
		}

		bool isValidSegmentKind(uint16_t rawKind) {
			return rawKind <= static_cast<uint16_t>(SegmentKind::SGK_BULK_SCRATCH);
		}

		bool isValidSegmentState(uint16_t rawState) {
			return rawState <= static_cast<uint16_t>(SegmentState::SGS_OBSOLETE);
		}

		void validateEnvelope(const SegmentEnvelope &envelope) {
			if (envelope.magic != RSS_SEGMENT_MAGIC) {
				throwFormatError(FormatError::FERR_INVALID_MAGIC, "relocatable segment magic does not match");
			}
			if (envelope.format_version != RSS_FORMAT_VERSION) {
				throwFormatError(FormatError::FERR_UNSUPPORTED_VERSION, "unsupported relocatable segment format version");
			}
			if (!isValidSegmentKind(static_cast<uint16_t>(envelope.kind)) ||
				!isValidSegmentState(static_cast<uint16_t>(envelope.state))) {
				throwFormatError(FormatError::FERR_INVALID_ENUM, "invalid relocatable segment kind or state");
			}
			if ((envelope.flags & ~RSS_ENVELOPE_KNOWN_FLAGS) != 0 || envelope.reserved != 0) {
				throwFormatError(FormatError::FERR_RESERVED_BITS, "relocatable segment uses unknown flags or reserved bytes");
			}
			if (envelope.header_bytes < RSS_ENVELOPE_BYTES || envelope.header_bytes > RSS_MAX_HEADER_BYTES ||
				envelope.header_bytes % alignof(uint64_t) != 0) {
				throwFormatError(FormatError::FERR_INVALID_HEADER, "invalid relocatable segment header size");
			}
			if (envelope.payload_bytes > RSS_SEGMENT_SIZE - envelope.header_bytes) {
				throwFormatError(FormatError::FERR_INVALID_HEADER,
								 "relocatable segment payload exceeds the physical segment");
			}
			if (envelope.state == SegmentState::SGS_FREE) {
				if (envelope.kind != SegmentKind::SGK_NONE || envelope.owner_id != 0 || envelope.payload_bytes != 0) {
					throwFormatError(FormatError::FERR_INVALID_HEADER,
									 "a free relocatable segment must not have an owner or layout");
				}
			} else if (envelope.kind == SegmentKind::SGK_NONE || !envelope.segment_id.isValid()) {
				throwFormatError(FormatError::FERR_INVALID_HEADER,
								 "an allocated relocatable segment needs a kind and segment ID");
			}
		}

		void encodeFields(std::span<std::byte> bytes, const SegmentEnvelope &envelope, uint32_t headerCrc) {
			LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, envelope.magic);
			LittleEndianCodec::writeU16(bytes, FORMAT_VERSION_OFFSET, envelope.format_version);
			LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, envelope.header_bytes);
			LittleEndianCodec::writeU16(bytes, KIND_OFFSET, static_cast<uint16_t>(envelope.kind));
			LittleEndianCodec::writeU16(bytes, STATE_OFFSET, static_cast<uint16_t>(envelope.state));
			LittleEndianCodec::writeU32(bytes, FLAGS_OFFSET, envelope.flags);
			LittleEndianCodec::writeU64(bytes, SEGMENT_ID_OFFSET, envelope.segment_id.value());
			LittleEndianCodec::writeU64(bytes, OWNER_ID_OFFSET, envelope.owner_id);
			LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, envelope.generation.value());
			LittleEndianCodec::writeU64(bytes, COMMITTED_LSN_OFFSET, envelope.committed_lsn.value());
			LittleEndianCodec::writeU32(bytes, PAYLOAD_BYTES_OFFSET, envelope.payload_bytes);
			LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, headerCrc);
			LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET, envelope.payload_crc);
			LittleEndianCodec::writeU32(bytes, RESERVED_OFFSET, envelope.reserved);
		}

		uint32_t calculateEnvelopeCrc(const std::array<std::byte, RSS_ENVELOPE_BYTES> &bytes) {
			return utils::calculateCrc(reinterpret_cast<const char *>(bytes.data()), bytes.size());
		}

	} // namespace

	FormatException::FormatException(FormatError error, const std::string &message) :
		std::runtime_error(message), error_(error) {}

	void LittleEndianCodec::writeU16(std::span<std::byte> bytes, size_t offset, uint16_t value) {
		requireRange(bytes.size(), offset, sizeof(value));
		for (size_t index = 0; index < sizeof(value); ++index) {
			bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
		}
	}

	void LittleEndianCodec::writeU32(std::span<std::byte> bytes, size_t offset, uint32_t value) {
		requireRange(bytes.size(), offset, sizeof(value));
		for (size_t index = 0; index < sizeof(value); ++index) {
			bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
		}
	}

	void LittleEndianCodec::writeU64(std::span<std::byte> bytes, size_t offset, uint64_t value) {
		requireRange(bytes.size(), offset, sizeof(value));
		for (size_t index = 0; index < sizeof(value); ++index) {
			bytes[offset + index] = static_cast<std::byte>((value >> (index * 8U)) & 0xFFU);
		}
	}

	uint16_t LittleEndianCodec::readU16(std::span<const std::byte> bytes, size_t offset) {
		requireRange(bytes.size(), offset, sizeof(uint16_t));
		uint16_t value = 0;
		for (size_t index = 0; index < sizeof(value); ++index) {
			value |= static_cast<uint16_t>(std::to_integer<uint8_t>(bytes[offset + index])) << (index * 8U);
		}
		return value;
	}

	uint32_t LittleEndianCodec::readU32(std::span<const std::byte> bytes, size_t offset) {
		requireRange(bytes.size(), offset, sizeof(uint32_t));
		uint32_t value = 0;
		for (size_t index = 0; index < sizeof(value); ++index) {
			value |= static_cast<uint32_t>(std::to_integer<uint8_t>(bytes[offset + index])) << (index * 8U);
		}
		return value;
	}

	uint64_t LittleEndianCodec::readU64(std::span<const std::byte> bytes, size_t offset) {
		requireRange(bytes.size(), offset, sizeof(uint64_t));
		uint64_t value = 0;
		for (size_t index = 0; index < sizeof(value); ++index) {
			value |= static_cast<uint64_t>(std::to_integer<uint8_t>(bytes[offset + index])) << (index * 8U);
		}
		return value;
	}

	std::array<std::byte, RSS_ENVELOPE_BYTES> SegmentEnvelopeCodec::encode(const SegmentEnvelope &envelope) {
		validateEnvelope(envelope);
		std::array<std::byte, RSS_ENVELOPE_BYTES> bytes{};
		encodeFields(bytes, envelope, 0);
		encodeFields(bytes, envelope, calculateEnvelopeCrc(bytes));
		return bytes;
	}

	SegmentEnvelope SegmentEnvelopeCodec::decode(std::span<const std::byte> bytes) {
		requireRange(bytes.size(), 0, RSS_ENVELOPE_BYTES);
		SegmentEnvelope envelope;
		envelope.magic = LittleEndianCodec::readU32(bytes, MAGIC_OFFSET);
		envelope.format_version = LittleEndianCodec::readU16(bytes, FORMAT_VERSION_OFFSET);
		envelope.header_bytes = LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET);
		const uint16_t rawKind = LittleEndianCodec::readU16(bytes, KIND_OFFSET);
		const uint16_t rawState = LittleEndianCodec::readU16(bytes, STATE_OFFSET);
		envelope.kind = static_cast<SegmentKind>(rawKind);
		envelope.state = static_cast<SegmentState>(rawState);
		envelope.flags = LittleEndianCodec::readU32(bytes, FLAGS_OFFSET);
		envelope.segment_id = SegmentId(LittleEndianCodec::readU64(bytes, SEGMENT_ID_OFFSET));
		envelope.owner_id = LittleEndianCodec::readU64(bytes, OWNER_ID_OFFSET);
		envelope.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		envelope.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, COMMITTED_LSN_OFFSET));
		envelope.payload_bytes = LittleEndianCodec::readU32(bytes, PAYLOAD_BYTES_OFFSET);
		envelope.header_crc = LittleEndianCodec::readU32(bytes, HEADER_CRC_OFFSET);
		envelope.payload_crc = LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET);
		envelope.reserved = LittleEndianCodec::readU32(bytes, RESERVED_OFFSET);

		std::array<std::byte, RSS_ENVELOPE_BYTES> crcBytes{};
		std::copy_n(bytes.begin(), RSS_ENVELOPE_BYTES, crcBytes.begin());
		LittleEndianCodec::writeU32(crcBytes, HEADER_CRC_OFFSET, 0);
		if (calculateEnvelopeCrc(crcBytes) != envelope.header_crc) {
			throwFormatError(FormatError::FERR_CHECKSUM_MISMATCH, "relocatable segment envelope checksum does not match");
		}

		validateEnvelope(envelope);
		return envelope;
	}

} // namespace graph::storage
