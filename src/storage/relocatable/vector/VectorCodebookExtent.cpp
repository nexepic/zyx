/**
 * @file VectorCodebookExtent.cpp
 * @brief Immutable inline and multi-extent Vector Codebook codec implementation.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/vector/VectorCodebookExtent.hpp"

#include <algorithm>
#include <limits>
#include <set>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0U;
		constexpr size_t VERSION_OFFSET = 4U;
		constexpr size_t HEADER_BYTES_OFFSET = 6U;
		constexpr size_t KIND_OFFSET = 8U;
		constexpr size_t FLAGS_OFFSET = 10U;
		constexpr size_t ORDINAL_OFFSET = 12U;
		constexpr size_t CODEBOOK_ID_OFFSET = 16U;
		constexpr size_t CODEBOOK_GENERATION_OFFSET = 24U;
		constexpr size_t EXTENT_ID_OFFSET = 32U;
		constexpr size_t BLOCK_BYTES_OFFSET = 40U;
		constexpr size_t BLOCK_CRC_OFFSET = 44U;
		constexpr size_t HEADER_CRC_OFFSET = 48U;
		constexpr size_t RESERVED1_OFFSET = 52U;
		constexpr size_t RESERVED2_OFFSET = 56U;

		constexpr size_t MANIFEST_EXTENT_COUNT_OFFSET = RSS_VECTOR_CODEBOOK_HEADER_BYTES;
		constexpr size_t MANIFEST_ENTRIES_BYTES_OFFSET = RSS_VECTOR_CODEBOOK_HEADER_BYTES + 4U;
		constexpr size_t ENTRY_EXTENT_ID_OFFSET = 0U;
		constexpr size_t ENTRY_BLOCK_BYTES_OFFSET = 8U;
		constexpr size_t ENTRY_BLOCK_CRC_OFFSET = 12U;

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] uint32_t checksum(std::span<const std::byte> bytes) {
			const std::byte empty{};
			return utils::calculateCrc(bytes.empty() ? &empty : bytes.data(), bytes.size());
		}

		[[nodiscard]] bool isKind(VectorCodebookExtentKind kind) {
			return kind == VectorCodebookExtentKind::VCEK_MANIFEST || kind == VectorCodebookExtentKind::VCEK_DATA;
		}

		void validateExtentHeader(const VectorCodebookExtentHeader &header) {
			const bool manifest = header.kind == VectorCodebookExtentKind::VCEK_MANIFEST;
			const uint8_t invalid = static_cast<uint8_t>(!isKind(header.kind)) |
										static_cast<uint8_t>(!VectorCodebookExtentIdCodec::canEncodeCodebookId(
												header.codebook_id)) |
									static_cast<uint8_t>(!header.codebook_generation.isValid()) |
									static_cast<uint8_t>(!header.extent_id.isValid()) |
									static_cast<uint8_t>(header.block_bytes > RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES) |
										static_cast<uint8_t>(manifest &&
												!VectorCodebookExtentIdCodec::isManifestExtentForCodebook(
														header.extent_id, header.codebook_id)) |
									static_cast<uint8_t>(manifest && header.ordinal != RSS_VECTOR_CODEBOOK_MANIFEST_ORDINAL) |
										static_cast<uint8_t>(!manifest &&
												!VectorCodebookExtentIdCodec::isDataExtentId(header.extent_id)) |
									static_cast<uint8_t>(!manifest && header.ordinal == RSS_VECTOR_CODEBOOK_MANIFEST_ORDINAL) |
									static_cast<uint8_t>(!manifest && header.block_bytes == 0U);
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector Codebook extent header has invalid identity, kind, ordinal, or length");
			}
		}

		void validateManifest(const VectorCodebookManifest &manifest) {
			const bool inlinePayload = manifest.entries.empty();
				const uint8_t invalid =
						static_cast<uint8_t>(!VectorCodebookExtentIdCodec::canEncodeCodebookId(
														manifest.header.codebook_id)) |
						static_cast<uint8_t>(manifest.header.payload_bytes == 0U) |
					static_cast<uint8_t>(inlinePayload && manifest.inline_payload.size() != manifest.header.payload_bytes) |
					static_cast<uint8_t>(inlinePayload &&
										 manifest.inline_payload.size() > RSS_VECTOR_CODEBOOK_INLINE_PAYLOAD_BYTES) |
					static_cast<uint8_t>(!inlinePayload && !manifest.inline_payload.empty()) |
					static_cast<uint8_t>(!inlinePayload &&
										 manifest.header.payload_bytes <= RSS_VECTOR_CODEBOOK_INLINE_PAYLOAD_BYTES) |
					static_cast<uint8_t>(manifest.entries.size() > std::numeric_limits<uint32_t>::max());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector Codebook manifest has a noncanonical inline or multi-extent layout");
			}
			if (inlinePayload) {
				VectorCodebookHeaderCodec::validatePayload(manifest.header, manifest.inline_payload);
				return;
			}
			const size_t expectedExtents = VectorCodebookManifestCodec::requiredDataExtentCount(
					manifest.header.payload_bytes);
			if (manifest.entries.size() != expectedExtents) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector Codebook manifest data-extent count is not canonical for its payload");
			}
			std::set<uint64_t> extentIds;
			uint64_t totalBytes = 0U;
			for (size_t index = 0U; index < manifest.entries.size(); ++index) {
				const VectorCodebookManifestEntry &entry = manifest.entries[index];
				if (totalBytes >= manifest.header.payload_bytes) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable Vector Codebook manifest entries exceed the logical payload length");
				}
				const uint32_t expectedBytes = static_cast<uint32_t>(std::min<uint64_t>(
						RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES, manifest.header.payload_bytes - totalBytes));
					const uint8_t invalidEntry = static_cast<uint8_t>(!entry.extent_id.isValid()) |
											 static_cast<uint8_t>(!VectorCodebookExtentIdCodec::isDataExtentId(entry.extent_id)) |
										 static_cast<uint8_t>(entry.block_bytes != expectedBytes) |
										 static_cast<uint8_t>(!extentIds.insert(entry.extent_id.value()).second);
				if (invalidEntry != 0U) {
					fail(FormatError::FERR_INVALID_HEADER,
						 "relocatable Vector Codebook manifest has an invalid, duplicate, or noncanonical data extent");
				}
				totalBytes += entry.block_bytes;
			}
			if (totalBytes != manifest.header.payload_bytes) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector Codebook manifest entries do not cover the logical payload exactly");
			}
			const uint64_t manifestBytes = RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES +
					static_cast<uint64_t>(manifest.entries.size()) * RSS_VECTOR_CODEBOOK_MANIFEST_ENTRY_BYTES;
			if (manifestBytes > RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable Vector Codebook manifest entry list exceeds one manifest extent");
			}
		}

	} // namespace

	VectorCodebookExtentHeaderCodec::Bytes
	VectorCodebookExtentHeaderCodec::encode(const VectorCodebookExtentHeader &header) {
		validateExtentHeader(header);
		Bytes bytes{};
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_VECTOR_CODEBOOK_EXTENT_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_VECTOR_CODEBOOK_EXTENT_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_VECTOR_CODEBOOK_EXTENT_HEADER_BYTES);
		LittleEndianCodec::writeU16(bytes, KIND_OFFSET, static_cast<uint16_t>(header.kind));
		LittleEndianCodec::writeU16(bytes, FLAGS_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, ORDINAL_OFFSET, header.ordinal);
		LittleEndianCodec::writeU64(bytes, CODEBOOK_ID_OFFSET, header.codebook_id);
		LittleEndianCodec::writeU64(bytes, CODEBOOK_GENERATION_OFFSET, header.codebook_generation.value());
		LittleEndianCodec::writeU64(bytes, EXTENT_ID_OFFSET, header.extent_id.value());
		LittleEndianCodec::writeU32(bytes, BLOCK_BYTES_OFFSET, header.block_bytes);
		LittleEndianCodec::writeU32(bytes, BLOCK_CRC_OFFSET, header.block_crc);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, checksum(bytes));
		return bytes;
	}

	VectorCodebookExtentHeader VectorCodebookExtentHeaderCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_VECTOR_CODEBOOK_EXTENT_HEADER_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable Vector Codebook extent header is truncated");
		}
		if (bytes.size() != RSS_VECTOR_CODEBOOK_EXTENT_HEADER_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Vector Codebook extent header has an invalid size");
		}
		Bytes verified{};
		std::copy(bytes.begin(), bytes.end(), verified.begin());
		const uint32_t expectedCrc = LittleEndianCodec::readU32(verified, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(verified, HEADER_CRC_OFFSET, 0U);
		if (checksum(verified) != expectedCrc) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH,
				 "relocatable Vector Codebook extent header checksum does not match");
		}
		const uint8_t invalidStatic =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) !=
									 RSS_VECTOR_CODEBOOK_EXTENT_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) !=
									 RSS_VECTOR_CODEBOOK_EXTENT_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
									 RSS_VECTOR_CODEBOOK_EXTENT_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, RESERVED1_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU64(bytes, RESERVED2_OFFSET) != 0U);
		if (invalidStatic != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "relocatable Vector Codebook extent header has invalid fixed fields");
		}
		VectorCodebookExtentHeader header;
		header.kind = VectorCodebookExtentKind(LittleEndianCodec::readU16(bytes, KIND_OFFSET));
		header.codebook_id = LittleEndianCodec::readU64(bytes, CODEBOOK_ID_OFFSET);
		header.codebook_generation = Generation(LittleEndianCodec::readU64(bytes, CODEBOOK_GENERATION_OFFSET));
		header.extent_id = ExtentId(LittleEndianCodec::readU64(bytes, EXTENT_ID_OFFSET));
		header.ordinal = LittleEndianCodec::readU32(bytes, ORDINAL_OFFSET);
		header.block_bytes = LittleEndianCodec::readU32(bytes, BLOCK_BYTES_OFFSET);
		header.block_crc = LittleEndianCodec::readU32(bytes, BLOCK_CRC_OFFSET);
		validateExtentHeader(header);
		return header;
	}

	void VectorCodebookExtentHeaderCodec::validateBlock(const VectorCodebookExtentHeader &header,
																std::span<const std::byte> block) {
		validateExtentHeader(header);
		if (block.size() != header.block_bytes) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector Codebook extent payload length disagrees with its header");
		}
		if (checksum(block) != header.block_crc) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH,
				 "relocatable Vector Codebook extent payload checksum does not match");
		}
	}

	size_t VectorCodebookManifestCodec::requiredDataExtentCount(size_t payloadBytes) {
		if (payloadBytes == 0U || payloadBytes <= RSS_VECTOR_CODEBOOK_INLINE_PAYLOAD_BYTES) {
			return 0U;
		}
		return 1U + (payloadBytes - 1U) / RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES;
	}

	std::vector<std::byte> VectorCodebookManifestCodec::encode(const VectorCodebookManifest &manifest) {
		validateManifest(manifest);
		const VectorCodebookHeaderCodec::Bytes encodedHeader = VectorCodebookHeaderCodec::encode(manifest.header);
		const size_t payloadBytes = manifest.isInline()
									? manifest.inline_payload.size()
									: manifest.entries.size() * RSS_VECTOR_CODEBOOK_MANIFEST_ENTRY_BYTES;
		std::vector<std::byte> bytes(RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES + payloadBytes);
		std::copy(encodedHeader.begin(), encodedHeader.end(), bytes.begin());
		LittleEndianCodec::writeU32(bytes, MANIFEST_EXTENT_COUNT_OFFSET,
										 static_cast<uint32_t>(manifest.entries.size()));
		LittleEndianCodec::writeU32(bytes, MANIFEST_ENTRIES_BYTES_OFFSET,
										 static_cast<uint32_t>(manifest.entries.size() *
														   RSS_VECTOR_CODEBOOK_MANIFEST_ENTRY_BYTES));
		if (manifest.isInline()) {
			std::copy(manifest.inline_payload.begin(), manifest.inline_payload.end(),
					  bytes.begin() + RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES);
		} else {
			for (size_t index = 0U; index < manifest.entries.size(); ++index) {
				const size_t offset = RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES +
								  index * RSS_VECTOR_CODEBOOK_MANIFEST_ENTRY_BYTES;
				LittleEndianCodec::writeU64(bytes, offset + ENTRY_EXTENT_ID_OFFSET,
											 manifest.entries[index].extent_id.value());
				LittleEndianCodec::writeU32(bytes, offset + ENTRY_BLOCK_BYTES_OFFSET,
											 manifest.entries[index].block_bytes);
				LittleEndianCodec::writeU32(bytes, offset + ENTRY_BLOCK_CRC_OFFSET,
											 manifest.entries[index].block_crc);
			}
		}
		return bytes;
	}

	VectorCodebookManifest VectorCodebookManifestCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "relocatable Vector Codebook manifest is truncated");
		}
		VectorCodebookManifest manifest;
		manifest.header = VectorCodebookHeaderCodec::decode(bytes.first(RSS_VECTOR_CODEBOOK_HEADER_BYTES));
		const uint32_t extentCount = LittleEndianCodec::readU32(bytes, MANIFEST_EXTENT_COUNT_OFFSET);
		const uint32_t entriesBytes = LittleEndianCodec::readU32(bytes, MANIFEST_ENTRIES_BYTES_OFFSET);
		const uint64_t expectedEntriesBytes =
				static_cast<uint64_t>(extentCount) * RSS_VECTOR_CODEBOOK_MANIFEST_ENTRY_BYTES;
		const uint8_t invalid = static_cast<uint8_t>(expectedEntriesBytes != entriesBytes) |
								static_cast<uint8_t>(expectedEntriesBytes >
														 bytes.size() - RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_HEADER,
				 "relocatable Vector Codebook manifest has inconsistent extent count or entry bytes");
		}
		if (extentCount == 0U) {
			if (entriesBytes != 0U || bytes.size() != RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES +
											 manifest.header.payload_bytes) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable inline Vector Codebook manifest has an inconsistent payload length");
			}
			manifest.inline_payload.assign(bytes.begin() + RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES, bytes.end());
		} else {
			if (bytes.size() != RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES + entriesBytes) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "relocatable multi-extent Vector Codebook manifest has trailing or missing entry bytes");
			}
			manifest.entries.reserve(extentCount);
			for (uint32_t index = 0U; index < extentCount; ++index) {
				const size_t offset = RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES +
								  static_cast<size_t>(index) * RSS_VECTOR_CODEBOOK_MANIFEST_ENTRY_BYTES;
				manifest.entries.push_back({ExtentId(LittleEndianCodec::readU64(bytes, offset + ENTRY_EXTENT_ID_OFFSET)),
										LittleEndianCodec::readU32(bytes, offset + ENTRY_BLOCK_BYTES_OFFSET),
										LittleEndianCodec::readU32(bytes, offset + ENTRY_BLOCK_CRC_OFFSET)});
			}
		}
		validateManifest(manifest);
		return manifest;
	}

} // namespace graph::storage
