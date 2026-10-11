/**
 * @file EntityLabelSet.cpp
 * @brief Explicit little-endian codec for overflow Node label sets.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/entity/EntityLabelSet.hpp"

#include <algorithm>
#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0U;
		constexpr size_t VERSION_OFFSET = 4U;
		constexpr size_t HEADER_BYTES_OFFSET = 6U;
		constexpr size_t LABEL_COUNT_OFFSET = 8U;
		constexpr size_t PAYLOAD_BYTES_OFFSET = 12U;
		constexpr size_t FLAGS_OFFSET = 16U;
		constexpr size_t CHECKSUM_OFFSET = 20U;
		constexpr size_t LABELS_OFFSET = RSS_ENTITY_LABEL_SET_HEADER_BYTES;
		constexpr size_t LABEL_BYTES = RSS_ENTITY_LABEL_TOKEN_BYTES;

		[[noreturn]] void throwLabelSetError(FormatError error, const char *message) {
			throw FormatException(error, message);
		}

		void validateLabelCount(size_t labelCount) {
			if (labelCount > RSS_ENTITY_LABEL_SET_MAX_OVERFLOW_LABELS) {
				throwLabelSetError(FormatError::FERR_INVALID_STATE,
							   "relocatable Entity label overflow exceeds one typed slotted-page record");
			}
		}

		void validateLabels(std::span<const uint64_t> labelIds) {
			if (labelIds.empty() ||
				std::ranges::any_of(labelIds, [](uint64_t labelId) { return labelId == 0U; }) ||
				!std::ranges::is_sorted(labelIds) || std::ranges::adjacent_find(labelIds) != labelIds.end()) {
				throwLabelSetError(FormatError::FERR_INVALID_HEADER,
							   "relocatable Entity label overflow requires nonzero, strictly ordered labels");
			}
			validateLabelCount(labelIds.size());
		}

		[[nodiscard]] size_t checkedTotalBytes(size_t labelCount) {
			validateLabelCount(labelCount);
			return LABELS_OFFSET + labelCount * LABEL_BYTES;
		}

		[[nodiscard]] uint32_t checksum(std::span<const std::byte> bytes) {
			return utils::calculateCrc(bytes.data(), bytes.size());
		}

	} // namespace

	std::vector<std::byte> EntityLabelSetCodec::encode(std::span<const uint64_t> labelIds) {
		validateLabels(labelIds);
		const uint32_t labelCount = static_cast<uint32_t>(labelIds.size());
		const size_t totalBytes = checkedTotalBytes(labelCount);
		const size_t payloadBytes = totalBytes - LABELS_OFFSET;

		std::vector<std::byte> bytes(totalBytes);
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_ENTITY_LABEL_SET_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_ENTITY_LABEL_SET_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_ENTITY_LABEL_SET_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, LABEL_COUNT_OFFSET, labelCount);
		LittleEndianCodec::writeU32(bytes, PAYLOAD_BYTES_OFFSET, static_cast<uint32_t>(payloadBytes));
		LittleEndianCodec::writeU32(bytes, FLAGS_OFFSET, 0U);
		for (size_t index = 0U; index < labelIds.size(); ++index) {
			LittleEndianCodec::writeU64(bytes, LABELS_OFFSET + index * LABEL_BYTES, labelIds[index]);
		}
		LittleEndianCodec::writeU32(bytes, CHECKSUM_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, CHECKSUM_OFFSET, checksum(bytes));
		return bytes;
	}

	std::vector<uint64_t> EntityLabelSetCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_ENTITY_LABEL_SET_HEADER_BYTES) {
			throwLabelSetError(FormatError::FERR_BUFFER_TOO_SMALL,
						   "relocatable Entity label overflow record is truncated");
		}
		if (bytes.size() > RSS_ENTITY_LABEL_SET_MAX_RECORD_BYTES) {
			throwLabelSetError(FormatError::FERR_INVALID_HEADER,
						   "relocatable Entity label overflow exceeds one typed slotted-page record");
		}
		const uint32_t expectedChecksum = LittleEndianCodec::readU32(bytes, CHECKSUM_OFFSET);
		std::vector<std::byte> checksumBytes(bytes.begin(), bytes.end());
		LittleEndianCodec::writeU32(checksumBytes, CHECKSUM_OFFSET, 0U);
		if (checksum(checksumBytes) != expectedChecksum) {
			throwLabelSetError(FormatError::FERR_CHECKSUM_MISMATCH,
						   "relocatable Entity label overflow checksum does not match");
		}
		if (LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_ENTITY_LABEL_SET_MAGIC) {
			throwLabelSetError(FormatError::FERR_INVALID_MAGIC,
						   "relocatable Entity label overflow has an invalid magic");
		}
		if (LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_ENTITY_LABEL_SET_VERSION) {
			throwLabelSetError(FormatError::FERR_UNSUPPORTED_VERSION,
						   "relocatable Entity label overflow has an unsupported version");
		}
		const uint32_t labelCount = LittleEndianCodec::readU32(bytes, LABEL_COUNT_OFFSET);
		const size_t totalBytes = checkedTotalBytes(static_cast<size_t>(labelCount));
		const uint8_t invalidHeader =
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
									  RSS_ENTITY_LABEL_SET_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, PAYLOAD_BYTES_OFFSET) !=
									  totalBytes - LABELS_OFFSET) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(bytes.size() != totalBytes) | static_cast<uint8_t>(labelCount == 0U);
		if (invalidHeader != 0U) {
			throwLabelSetError(FormatError::FERR_INVALID_HEADER,
						   "relocatable Entity label overflow has inconsistent bounds or reserved fields");
		}

		std::vector<uint64_t> labelIds;
		labelIds.reserve(labelCount);
		for (uint32_t index = 0U; index < labelCount; ++index) {
			labelIds.push_back(LittleEndianCodec::readU64(bytes, LABELS_OFFSET + static_cast<size_t>(index) * LABEL_BYTES));
		}
		validateLabels(labelIds);
		return labelIds;
	}

} // namespace graph::storage
