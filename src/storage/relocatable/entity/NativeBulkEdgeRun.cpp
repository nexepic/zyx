/**
 * @file NativeBulkEdgeRun.cpp
 * @brief Checked transaction-private native bulk Edge run framing.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/relocatable/entity/NativeBulkEdgeRun.hpp"

#include <algorithm>
#include <limits>

#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0U;
		constexpr size_t VERSION_OFFSET = 4U;
		constexpr size_t HEADER_BYTES_OFFSET = 6U;
		constexpr size_t FLAGS_OFFSET = 8U;
		constexpr size_t RECORD_COUNT_OFFSET = 12U;
		constexpr size_t RECORD_BYTES_OFFSET = 16U;
		constexpr size_t RECORDS_CRC_OFFSET = 20U;
		constexpr size_t LOGICAL_BYTES_OFFSET = 24U;
		constexpr size_t HEADER_CRC_OFFSET = 32U;
		constexpr size_t RESERVED0_OFFSET = 36U;
		constexpr size_t RESERVED1_OFFSET = 40U;
		constexpr size_t RESERVED2_OFFSET = 48U;
		constexpr size_t RESERVED3_OFFSET = 56U;
		constexpr size_t RECORD_LENGTH_BYTES = sizeof(uint32_t);
		constexpr size_t RECORD_CRC_BYTES = sizeof(uint32_t);

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] uint32_t checksum(std::span<const std::byte> bytes) {
			const std::byte empty{};
			return utils::calculateCrc(bytes.empty() ? &empty : bytes.data(), bytes.size());
		}

		[[nodiscard]] size_t checkedAdd(size_t left, size_t right, const char *message) {
			if (right > std::numeric_limits<size_t>::max() - left) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, message);
			}
			return left + right;
		}

		void validateRun(const NativeBulkEdgeRun &run) {
			if (run.records.empty() || run.records.size() > std::numeric_limits<uint32_t>::max()) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge run requires a nonempty bounded record sequence");
			}
			uint64_t previousId = 0U;
			for (const NativeBulkEdgeRecord &record: run.records) {
				if (record.edge_id == 0U || record.edge_id <= previousId) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native bulk Edge run records must have strictly increasing nonzero IDs");
				}
				static_cast<void>(NativeBulkEdgeRecordCodec::encodedSize(record));
				previousId = record.edge_id;
			}
		}

		[[nodiscard]] size_t recordsSize(const NativeBulkEdgeRun &run) {
			validateRun(run);
			size_t result = 0U;
			for (const NativeBulkEdgeRecord &record: run.records) {
				const size_t recordSize = NativeBulkEdgeRecordCodec::encodedSize(record);
				if (recordSize > std::numeric_limits<uint32_t>::max()) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "native bulk Edge run record exceeds its length field");
				}
				result = checkedAdd(result, RECORD_LENGTH_BYTES + RECORD_CRC_BYTES,
										"native bulk Edge run framing size overflowed");
				result = checkedAdd(result, recordSize, "native bulk Edge run record bytes overflowed");
			}
			if (result > std::numeric_limits<uint32_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
					 "native bulk Edge run records exceed their wire field");
			}
			return result;
		}

	} // namespace

	size_t NativeBulkEdgeRunCodec::encodedSize(const NativeBulkEdgeRun &run) {
		return checkedAdd(RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES, recordsSize(run),
						  "native bulk Edge run total size overflowed");
	}

	std::vector<std::byte> NativeBulkEdgeRunCodec::encode(const NativeBulkEdgeRun &run) {
		const size_t encodedRecordsSize = recordsSize(run);
		std::vector<std::byte> result(RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES + encodedRecordsSize);
		std::span<std::byte> records = std::span<std::byte>(result).subspan(RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES);
		size_t offset = 0U;
		uint64_t logicalBytes = 0U;
		for (const NativeBulkEdgeRecord &record: run.records) {
			const std::vector<std::byte> encodedRecord = NativeBulkEdgeRecordCodec::encode(record);
			LittleEndianCodec::writeU32(records, offset, static_cast<uint32_t>(encodedRecord.size()));
			offset += RECORD_LENGTH_BYTES;
			LittleEndianCodec::writeU32(records, offset, checksum(encodedRecord));
			offset += RECORD_CRC_BYTES;
			std::copy(encodedRecord.begin(), encodedRecord.end(), records.begin() + offset);
			offset += encodedRecord.size();
			if (logicalBytes > std::numeric_limits<uint64_t>::max() - encodedRecord.size()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "native bulk Edge run logical bytes overflowed");
			}
			logicalBytes += encodedRecord.size();
		}
		if (offset != records.size()) {
			fail(FormatError::FERR_INVALID_STATE, "native bulk Edge run measured bytes do not match its encoder");
		}
		LittleEndianCodec::writeU32(result, MAGIC_OFFSET, RSS_NATIVE_BULK_EDGE_RUN_MAGIC);
		LittleEndianCodec::writeU16(result, VERSION_OFFSET, RSS_NATIVE_BULK_EDGE_RUN_VERSION);
		LittleEndianCodec::writeU16(result, HEADER_BYTES_OFFSET, RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES);
		LittleEndianCodec::writeU16(result, FLAGS_OFFSET, 0U);
		LittleEndianCodec::writeU32(result, RECORD_COUNT_OFFSET, static_cast<uint32_t>(run.records.size()));
		LittleEndianCodec::writeU32(result, RECORD_BYTES_OFFSET, static_cast<uint32_t>(records.size()));
		LittleEndianCodec::writeU32(result, RECORDS_CRC_OFFSET, checksum(records));
		LittleEndianCodec::writeU64(result, LOGICAL_BYTES_OFFSET, logicalBytes);
		LittleEndianCodec::writeU32(result, HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(result, RESERVED0_OFFSET, 0U);
		LittleEndianCodec::writeU64(result, RESERVED1_OFFSET, 0U);
		LittleEndianCodec::writeU64(result, RESERVED2_OFFSET, 0U);
		LittleEndianCodec::writeU64(result, RESERVED3_OFFSET, 0U);
		LittleEndianCodec::writeU32(result, HEADER_CRC_OFFSET,
							checksum(std::span<const std::byte>(result).first(RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES)));
		return result;
	}

	NativeBulkEdgeRun NativeBulkEdgeRunCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES ||
			LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_NATIVE_BULK_EDGE_RUN_MAGIC ||
			LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_NATIVE_BULK_EDGE_RUN_VERSION ||
			LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) != RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES ||
			LittleEndianCodec::readU16(bytes, FLAGS_OFFSET) != 0U ||
			LittleEndianCodec::readU32(bytes, RESERVED0_OFFSET) != 0U ||
			LittleEndianCodec::readU64(bytes, RESERVED1_OFFSET) != 0U ||
			LittleEndianCodec::readU64(bytes, RESERVED2_OFFSET) != 0U ||
			LittleEndianCodec::readU64(bytes, RESERVED3_OFFSET) != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge run has an invalid header");
		}
		std::vector<std::byte> header(bytes.begin(), bytes.begin() + RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES);
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0U);
		if (checksum(header) != expectedHeaderCrc) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "native bulk Edge run header checksum does not match");
		}
		const uint32_t recordCount = LittleEndianCodec::readU32(bytes, RECORD_COUNT_OFFSET);
		const uint32_t recordBytes = LittleEndianCodec::readU32(bytes, RECORD_BYTES_OFFSET);
		if (recordCount == 0U || recordBytes != bytes.size() - RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES ||
			recordCount > recordBytes / (RECORD_LENGTH_BYTES + RECORD_CRC_BYTES)) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge run dimensions are invalid");
		}
		const std::span<const std::byte> records = bytes.subspan(RSS_NATIVE_BULK_EDGE_RUN_HEADER_BYTES);
		if (checksum(records) != LittleEndianCodec::readU32(bytes, RECORDS_CRC_OFFSET)) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "native bulk Edge run aggregate checksum does not match");
		}
		NativeBulkEdgeRun result;
		result.records.reserve(recordCount);
		size_t offset = 0U;
		uint64_t logicalBytes = 0U;
		uint64_t previousId = 0U;
		for (uint32_t ordinal = 0U; ordinal < recordCount; ++ordinal) {
			if (records.size() - offset < RECORD_LENGTH_BYTES + RECORD_CRC_BYTES) {
				fail(FormatError::FERR_BUFFER_TOO_SMALL, "native bulk Edge run record framing is truncated");
			}
			const uint32_t length = LittleEndianCodec::readU32(records, offset);
			offset += RECORD_LENGTH_BYTES;
			const uint32_t expectedCrc = LittleEndianCodec::readU32(records, offset);
			offset += RECORD_CRC_BYTES;
			if (length == 0U || length > records.size() - offset) {
				fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge run record length is invalid");
			}
			const std::span<const std::byte> encodedRecord = records.subspan(offset, length);
			if (checksum(encodedRecord) != expectedCrc) {
				fail(FormatError::FERR_CHECKSUM_MISMATCH, "native bulk Edge run record checksum does not match");
			}
			NativeBulkEdgeRecord record = NativeBulkEdgeRecordCodec::decode(encodedRecord);
			if (record.edge_id <= previousId) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge run records are not strictly ordered by Edge ID");
			}
			previousId = record.edge_id;
			if (logicalBytes > std::numeric_limits<uint64_t>::max() - length) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "native bulk Edge run logical bytes overflowed");
			}
			logicalBytes += length;
			result.records.push_back(std::move(record));
			offset += length;
		}
		if (offset != records.size() || logicalBytes != LittleEndianCodec::readU64(bytes, LOGICAL_BYTES_OFFSET)) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge run has trailing or inconsistent record bytes");
		}
		return result;
	}

} // namespace graph::storage
