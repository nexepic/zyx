/**
 * @file NativeBulkEdgeRecord.cpp
 * @brief Canonical bounded scratch record implementation for native Edge ingest.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#include "graph/storage/relocatable/entity/NativeBulkEdgeRecord.hpp"

#include <algorithm>
#include <limits>
#include <string_view>

#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/storage/relocatable/property/PropertyValueCodec.hpp"
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0U;
		constexpr size_t VERSION_OFFSET = 4U;
		constexpr size_t HEADER_BYTES_OFFSET = 6U;
		constexpr size_t FLAGS_OFFSET = 8U;
		constexpr size_t EDGE_ID_OFFSET = 12U;
		constexpr size_t SOURCE_ID_OFFSET = 20U;
		constexpr size_t TARGET_ID_OFFSET = 28U;
		constexpr size_t TYPE_BYTES_OFFSET = 36U;
		constexpr size_t PROPERTY_COUNT_OFFSET = 40U;
		constexpr size_t PAYLOAD_BYTES_OFFSET = 44U;
		constexpr size_t PAYLOAD_CRC_OFFSET = 48U;
		constexpr size_t HEADER_CRC_OFFSET = 52U;
		constexpr size_t RESERVED_OFFSET = 56U;
		constexpr size_t PROPERTY_KEY_BYTES = sizeof(uint32_t);
		constexpr uint16_t KNOWN_FLAGS = 0U;

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

		[[nodiscard]] bool bytewiseLess(std::string_view left, std::string_view right) {
			return std::lexicographical_compare(left.begin(), left.end(), right.begin(), right.end(),
														[](char lhs, char rhs) {
																return static_cast<unsigned char>(lhs) < static_cast<unsigned char>(rhs);
														});
		}

		[[nodiscard]] std::vector<const LogicalGraphWriteProperty *>
		canonicalProperties(const NativeBulkEdgeRecord &record) {
			if (record.edge_id == 0U || record.source_node_id == 0U || record.target_node_id == 0U ||
				record.type.size() > std::numeric_limits<uint32_t>::max() ||
				record.properties.size() > std::numeric_limits<uint32_t>::max()) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge record requires bounded nonzero identities and dimensions");
			}
			std::vector<const LogicalGraphWriteProperty *> result;
			result.reserve(record.properties.size());
			for (const LogicalGraphWriteProperty &property: record.properties) {
				if (property.key.empty() || property.key.size() > std::numeric_limits<uint32_t>::max()) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native bulk Edge record has an invalid Property key");
				}
				result.push_back(&property);
			}
			std::ranges::sort(result, [](const LogicalGraphWriteProperty *left, const LogicalGraphWriteProperty *right) {
				return bytewiseLess(left->key, right->key);
			});
			for (size_t ordinal = 1U; ordinal < result.size(); ++ordinal) {
				if (!bytewiseLess(result[ordinal - 1U]->key, result[ordinal]->key)) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "native bulk Edge record has duplicate Property keys");
				}
			}
			return result;
		}

		[[nodiscard]] size_t payloadSize(const NativeBulkEdgeRecord &record,
											 std::span<const LogicalGraphWriteProperty *const> properties) {
			size_t result = record.type.size();
			for (const LogicalGraphWriteProperty *property: properties) {
				result = checkedAdd(result, PROPERTY_KEY_BYTES, "native bulk Edge Property key size overflowed");
				result = checkedAdd(result, property->key.size(), "native bulk Edge Property key size overflowed");
				result = checkedAdd(result, CanonicalPropertyValueCodec::encodedSize(property->value),
									"native bulk Edge Property value size overflowed");
			}
			if (result > std::numeric_limits<uint32_t>::max()) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "native bulk Edge payload exceeds its wire field");
			}
			return result;
		}

		void appendU32(std::vector<std::byte> &bytes, uint32_t value) {
			const size_t offset = bytes.size();
			bytes.resize(checkedAdd(offset, sizeof(value), "native bulk Edge record size overflowed"));
			LittleEndianCodec::writeU32(bytes, offset, value);
		}

		void appendBytes(std::vector<std::byte> &bytes, std::string_view value) {
			bytes.insert(bytes.end(), reinterpret_cast<const std::byte *>(value.data()),
						 reinterpret_cast<const std::byte *>(value.data() + value.size()));
		}

	} // namespace

	size_t NativeBulkEdgeRecordCodec::encodedSize(const NativeBulkEdgeRecord &record) {
		const std::vector<const LogicalGraphWriteProperty *> properties = canonicalProperties(record);
		const size_t payload = payloadSize(record, properties);
		return checkedAdd(RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES, payload,
						  "native bulk Edge record total size overflowed");
	}

	std::vector<std::byte> NativeBulkEdgeRecordCodec::encode(const NativeBulkEdgeRecord &record) {
		const std::vector<const LogicalGraphWriteProperty *> properties = canonicalProperties(record);
		const size_t payloadBytes = payloadSize(record, properties);
		const size_t totalBytes = checkedAdd(RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES, payloadBytes,
										"native bulk Edge record total size overflowed");
		std::vector<std::byte> result(totalBytes);
		std::vector<std::byte> payload;
		payload.reserve(payloadBytes);
		appendBytes(payload, record.type);
		for (const LogicalGraphWriteProperty *property: properties) {
			appendU32(payload, static_cast<uint32_t>(property->key.size()));
			appendBytes(payload, property->key);
			CanonicalPropertyValueCodec::append(payload, property->value);
		}
		if (payload.size() != payloadBytes) {
			fail(FormatError::FERR_INVALID_STATE,
				 "native bulk Edge record Property codec changed its measured wire size");
		}
		std::copy(payload.begin(), payload.end(), result.begin() + RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES);
		LittleEndianCodec::writeU32(result, MAGIC_OFFSET, RSS_NATIVE_BULK_EDGE_RECORD_MAGIC);
		LittleEndianCodec::writeU16(result, VERSION_OFFSET, RSS_NATIVE_BULK_EDGE_RECORD_VERSION);
		LittleEndianCodec::writeU16(result, HEADER_BYTES_OFFSET, RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES);
		LittleEndianCodec::writeU16(result, FLAGS_OFFSET, KNOWN_FLAGS);
		LittleEndianCodec::writeU64(result, EDGE_ID_OFFSET, record.edge_id);
		LittleEndianCodec::writeU64(result, SOURCE_ID_OFFSET, record.source_node_id);
		LittleEndianCodec::writeU64(result, TARGET_ID_OFFSET, record.target_node_id);
		LittleEndianCodec::writeU32(result, TYPE_BYTES_OFFSET, static_cast<uint32_t>(record.type.size()));
		LittleEndianCodec::writeU32(result, PROPERTY_COUNT_OFFSET, static_cast<uint32_t>(properties.size()));
		LittleEndianCodec::writeU32(result, PAYLOAD_BYTES_OFFSET, static_cast<uint32_t>(payloadBytes));
		LittleEndianCodec::writeU32(result, PAYLOAD_CRC_OFFSET, checksum(payload));
		LittleEndianCodec::writeU32(result, HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU64(result, RESERVED_OFFSET, 0U);
		LittleEndianCodec::writeU32(result, HEADER_CRC_OFFSET,
								checksum(std::span<const std::byte>(result).first(RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES)));
		return result;
	}

	NativeBulkEdgeRecord NativeBulkEdgeRecordCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES ||
			LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_NATIVE_BULK_EDGE_RECORD_MAGIC ||
			LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_NATIVE_BULK_EDGE_RECORD_VERSION ||
			LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) != RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES ||
			LittleEndianCodec::readU16(bytes, FLAGS_OFFSET) != KNOWN_FLAGS ||
			LittleEndianCodec::readU64(bytes, RESERVED_OFFSET) != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge record has an invalid header");
		}
		std::vector<std::byte> header(bytes.begin(), bytes.begin() + RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES);
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0U);
		if (checksum(header) != expectedHeaderCrc) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "native bulk Edge record header checksum does not match");
		}
		const uint32_t payloadBytes = LittleEndianCodec::readU32(bytes, PAYLOAD_BYTES_OFFSET);
		if (payloadBytes != bytes.size() - RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge record payload size does not match its frame");
		}
		const std::span<const std::byte> payload = bytes.subspan(RSS_NATIVE_BULK_EDGE_RECORD_HEADER_BYTES);
		if (checksum(payload) != LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "native bulk Edge record payload checksum does not match");
		}
		const uint32_t typeBytes = LittleEndianCodec::readU32(bytes, TYPE_BYTES_OFFSET);
		const uint32_t propertyCount = LittleEndianCodec::readU32(bytes, PROPERTY_COUNT_OFFSET);
		if (typeBytes > payload.size()) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge record type exceeds its payload");
		}
		NativeBulkEdgeRecord result;
		result.edge_id = LittleEndianCodec::readU64(bytes, EDGE_ID_OFFSET);
		result.source_node_id = LittleEndianCodec::readU64(bytes, SOURCE_ID_OFFSET);
		result.target_node_id = LittleEndianCodec::readU64(bytes, TARGET_ID_OFFSET);
		if (result.edge_id == 0U || result.source_node_id == 0U || result.target_node_id == 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge record has a zero logical identity");
		}
		result.type.assign(reinterpret_cast<const char *>(payload.data()), typeBytes);
		result.properties.reserve(propertyCount);
		size_t offset = typeBytes;
		std::string previousKey;
		for (uint32_t ordinal = 0U; ordinal < propertyCount; ++ordinal) {
			if (payload.size() - offset < PROPERTY_KEY_BYTES) {
				fail(FormatError::FERR_BUFFER_TOO_SMALL, "native bulk Edge Property key length is truncated");
			}
			const uint32_t keyBytes = LittleEndianCodec::readU32(payload, offset);
			offset += PROPERTY_KEY_BYTES;
			if (keyBytes == 0U || keyBytes > payload.size() - offset) {
				fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge Property key is invalid");
			}
			std::string key(reinterpret_cast<const char *>(payload.data() + offset), keyBytes);
			offset += keyBytes;
			if (!previousKey.empty() && !bytewiseLess(previousKey, key)) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "native bulk Edge Property keys are not in canonical unique order");
			}
			PropertyValue value = CanonicalPropertyValueCodec::decodeNext(payload, offset);
			result.properties.push_back({std::move(key), std::move(value)});
			previousKey = result.properties.back().key;
		}
		if (offset != payload.size()) {
			fail(FormatError::FERR_INVALID_HEADER, "native bulk Edge record has trailing payload bytes");
		}
		return result;
	}

} // namespace graph::storage
