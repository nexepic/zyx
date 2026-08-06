/**
 * @file TestSlottedPageRecord.hpp
 * @brief Test-only opaque payload for slotted-page maintenance fixtures.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage::test {

	constexpr uint32_t TEST_SLOTTED_PAGE_RECORD_MAGIC = 0x5254535AU; // Little-endian bytes: ZSTR
	constexpr uint16_t TEST_SLOTTED_PAGE_RECORD_VERSION = 1U;
	constexpr uint16_t TEST_SLOTTED_PAGE_RECORD_HEADER_BYTES = 24U;

	struct TestSlottedPageRecord {
		uint64_t logical_id = 0U;
		std::vector<std::byte> payload;

		constexpr bool operator==(const TestSlottedPageRecord &) const = default;
	};

	class TestSlottedPageRecordCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const TestSlottedPageRecord &record) {
			if (record.logical_id == 0U || record.payload.size() > std::numeric_limits<uint32_t>::max()) {
				throw FormatException(FormatError::FERR_INVALID_HEADER,
								  "test slotted-page record has an invalid identity or length");
			}
			std::vector<std::byte> bytes(TEST_SLOTTED_PAGE_RECORD_HEADER_BYTES + record.payload.size());
			LittleEndianCodec::writeU32(bytes, 0U, TEST_SLOTTED_PAGE_RECORD_MAGIC);
			LittleEndianCodec::writeU16(bytes, 4U, TEST_SLOTTED_PAGE_RECORD_VERSION);
			LittleEndianCodec::writeU16(bytes, 6U, TEST_SLOTTED_PAGE_RECORD_HEADER_BYTES);
			LittleEndianCodec::writeU64(bytes, 8U, record.logical_id);
			LittleEndianCodec::writeU32(bytes, 16U, static_cast<uint32_t>(record.payload.size()));
			LittleEndianCodec::writeU32(bytes, 20U, 0U);
			std::copy(record.payload.begin(), record.payload.end(), bytes.begin() + TEST_SLOTTED_PAGE_RECORD_HEADER_BYTES);
			return bytes;
		}

		[[nodiscard]] static TestSlottedPageRecord decode(std::span<const std::byte> bytes) {
			if (bytes.size() < TEST_SLOTTED_PAGE_RECORD_HEADER_BYTES) {
				throw FormatException(FormatError::FERR_BUFFER_TOO_SMALL, "test slotted-page record is truncated");
			}
			const uint8_t invalid =
					static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, 0U) != TEST_SLOTTED_PAGE_RECORD_MAGIC) |
					static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, 4U) != TEST_SLOTTED_PAGE_RECORD_VERSION) |
					static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, 6U) != TEST_SLOTTED_PAGE_RECORD_HEADER_BYTES) |
					static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, 20U) != 0U) |
					static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, 16U) !=
										 bytes.size() - TEST_SLOTTED_PAGE_RECORD_HEADER_BYTES);
			if (invalid != 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER, "test slotted-page record has an invalid header");
			}
			TestSlottedPageRecord record;
			record.logical_id = LittleEndianCodec::readU64(bytes, 8U);
			if (record.logical_id == 0U) {
				throw FormatException(FormatError::FERR_INVALID_HEADER, "test slotted-page record has an invalid identity");
			}
			record.payload.assign(bytes.begin() + TEST_SLOTTED_PAGE_RECORD_HEADER_BYTES, bytes.end());
			return record;
		}
	};

} // namespace graph::storage::test
