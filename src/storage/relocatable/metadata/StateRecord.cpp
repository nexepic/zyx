/**
 * @file StateRecord.cpp
 * @brief Explicit, checksummed codec for relocatable State records.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/metadata/StateRecord.hpp"

#include <algorithm>
#include <array>
#include <limits>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0;
		constexpr size_t VERSION_OFFSET = 4;
		constexpr size_t HEADER_BYTES_OFFSET = 6;
		constexpr size_t STATE_ID_OFFSET = 8;
		constexpr size_t STORAGE_KIND_OFFSET = 16;
		constexpr size_t KEY_BYTES_OFFSET = 20;
		constexpr size_t INLINE_BYTES_OFFSET = 24;
		constexpr size_t LARGE_VALUE_ID_OFFSET = 28;
		constexpr size_t PAYLOAD_CRC_OFFSET = 36;
		constexpr size_t HEADER_CRC_OFFSET = 40;
		constexpr size_t RESERVED_OFFSET = 44;

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] bool isKnownStorageKind(StateRecordStorageKind kind) {
			return kind == StateRecordStorageKind::SRS_INLINE || kind == StateRecordStorageKind::SRS_LARGE_VALUE;
		}

		void validate(const StateRecord &record) {
			const uint8_t invalid =
					static_cast<uint8_t>(record.state_id == 0U) |
					static_cast<uint8_t>(record.key.empty()) |
					static_cast<uint8_t>(!isKnownStorageKind(record.storage_kind)) |
					static_cast<uint8_t>(record.key.size() > std::numeric_limits<uint32_t>::max()) |
					static_cast<uint8_t>(record.inline_bytes.size() > std::numeric_limits<uint32_t>::max()) |
					static_cast<uint8_t>(record.storage_kind == StateRecordStorageKind::SRS_INLINE &&
										 record.large_value_id.isValid()) |
					static_cast<uint8_t>(record.storage_kind == StateRecordStorageKind::SRS_LARGE_VALUE &&
										 (!record.large_value_id.isValid() || !record.inline_bytes.empty()));
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "relocatable State record has an invalid identity or storage mode");
			}
		}

		[[nodiscard]] uint32_t crc(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

		void writeHeader(std::span<std::byte> bytes, const StateRecord &record, uint32_t payload_crc,
						 uint32_t header_crc) {
			LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_STATE_RECORD_MAGIC);
			LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_STATE_RECORD_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_STATE_RECORD_HEADER_BYTES);
		LittleEndianCodec::writeU64(bytes, STATE_ID_OFFSET, record.state_id);
		bytes[STORAGE_KIND_OFFSET] = static_cast<std::byte>(record.storage_kind);
		std::fill(bytes.begin() + STORAGE_KIND_OFFSET + 1U, bytes.begin() + KEY_BYTES_OFFSET, std::byte{});
			LittleEndianCodec::writeU32(bytes, KEY_BYTES_OFFSET, static_cast<uint32_t>(record.key.size()));
			LittleEndianCodec::writeU32(bytes, INLINE_BYTES_OFFSET, static_cast<uint32_t>(record.inline_bytes.size()));
			LittleEndianCodec::writeU64(bytes, LARGE_VALUE_ID_OFFSET, record.large_value_id.value());
			LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET, payload_crc);
			LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, header_crc);
			LittleEndianCodec::writeU32(bytes, RESERVED_OFFSET, 0U);
		}

	} // namespace

	std::vector<std::byte> StateRecordCodec::encode(const StateRecord &record) {
		validate(record);
		std::vector<std::byte> bytes(RSS_STATE_RECORD_HEADER_BYTES + record.key.size() + record.inline_bytes.size());
		std::span<std::byte> payload = std::span(bytes).subspan(RSS_STATE_RECORD_HEADER_BYTES);
		std::copy_n(reinterpret_cast<const std::byte *>(record.key.data()), record.key.size(), payload.begin());
		std::copy(record.inline_bytes.begin(), record.inline_bytes.end(), payload.begin() + record.key.size());
		const uint32_t payload_crc = crc(payload);
		writeHeader(bytes, record, payload_crc, 0U);
		writeHeader(bytes, record, payload_crc, crc(std::span(bytes).first(RSS_STATE_RECORD_HEADER_BYTES)));
		return bytes;
	}

	StateRecord StateRecordCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_STATE_RECORD_HEADER_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable State record is truncated");
		}
		std::array<std::byte, RSS_STATE_RECORD_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expected_header_crc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0U);
		if (crc(header) != expected_header_crc) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "relocatable State record header checksum does not match");
		}
		const uint8_t raw_storage_kind = std::to_integer<uint8_t>(bytes[STORAGE_KIND_OFFSET]);
		const uint8_t invalid_header =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_STATE_RECORD_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_STATE_RECORD_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
										 RSS_STATE_RECORD_HEADER_BYTES) |
				static_cast<uint8_t>(raw_storage_kind != static_cast<uint8_t>(StateRecordStorageKind::SRS_INLINE) &&
									 raw_storage_kind != static_cast<uint8_t>(StateRecordStorageKind::SRS_LARGE_VALUE)) |
				static_cast<uint8_t>(bytes[17] != std::byte{} || bytes[18] != std::byte{} || bytes[19] != std::byte{}) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, RESERVED_OFFSET) != 0U);
		if (invalid_header != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable State record has invalid fixed fields");
		}
		const uint32_t key_bytes = LittleEndianCodec::readU32(bytes, KEY_BYTES_OFFSET);
		const uint32_t inline_bytes = LittleEndianCodec::readU32(bytes, INLINE_BYTES_OFFSET);
		const size_t payload_bytes = bytes.size() - RSS_STATE_RECORD_HEADER_BYTES;
		if (key_bytes > payload_bytes || inline_bytes != payload_bytes - key_bytes) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable State record payload lengths do not match its bytes");
		}
		const std::span<const std::byte> payload = bytes.subspan(RSS_STATE_RECORD_HEADER_BYTES);
		if (crc(payload) != LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "relocatable State record payload checksum does not match");
		}
		StateRecord record;
		record.state_id = LittleEndianCodec::readU64(bytes, STATE_ID_OFFSET);
		record.storage_kind = static_cast<StateRecordStorageKind>(raw_storage_kind);
		record.large_value_id = LargeValueId(LittleEndianCodec::readU64(bytes, LARGE_VALUE_ID_OFFSET));
		record.key.assign(reinterpret_cast<const char *>(payload.data()), key_bytes);
		record.inline_bytes.assign(payload.begin() + key_bytes, payload.end());
		validate(record);
		return record;
	}

} // namespace graph::storage
