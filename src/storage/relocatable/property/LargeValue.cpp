/**
 * @file LargeValue.cpp
 * @brief Checked wire codecs for generic immutable LargeValue pages.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#include "graph/storage/relocatable/property/LargeValue.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <ranges>
#include <stdexcept>

#include "graph/utils/ChecksumUtils.hpp"

namespace graph::storage {

	namespace {

		constexpr size_t MAGIC_OFFSET = 0U;
		constexpr size_t VERSION_OFFSET = 4U;
		constexpr size_t HEADER_BYTES_OFFSET = 6U;
		constexpr size_t FLAGS_OFFSET = 8U;
		constexpr size_t KIND_OFFSET = 12U;
		constexpr size_t CODEC_OFFSET = 14U;
		constexpr size_t MEDIA_TYPE_BYTES_OFFSET = 16U;
		constexpr size_t ENTRY_BYTES_OFFSET = 18U;
		constexpr size_t CHUNK_COUNT_OFFSET = 20U;
		constexpr size_t VALUE_ID_OFFSET = 24U;
		constexpr size_t PAGE_ID_OFFSET = 32U;
		constexpr size_t GENERATION_OFFSET = 40U;
		constexpr size_t LSN_OFFSET = 48U;
		constexpr size_t LOGICAL_BYTES_OFFSET = 56U;
		constexpr size_t CONTENT_HASH_OFFSET = 64U;
		constexpr size_t PAYLOAD_CRC_OFFSET = 96U;
		constexpr size_t HEADER_CRC_OFFSET = 100U;
		constexpr size_t NODE_LEVEL_OFFSET = 104U;
		constexpr size_t NODE_FIRST_CHUNK_OFFSET = 106U;
		constexpr size_t NODE_FIRST_OFFSET_OFFSET = 110U;
		constexpr size_t NODE_LOGICAL_BYTES_OFFSET = 118U;
		constexpr size_t MEDIA_TYPE_OFFSET = 128U;

		constexpr size_t CHUNK_ORDINAL_OFFSET = 0U;
		constexpr size_t CHUNK_CODEC_OFFSET = 4U;
		constexpr size_t CHUNK_PAGE_COUNT_OFFSET = 6U;
		constexpr size_t CHUNK_UNCOMPRESSED_OFFSET = 8U;
		constexpr size_t CHUNK_STORED_BYTES_OFFSET = 16U;
		constexpr size_t CHUNK_UNCOMPRESSED_BYTES_OFFSET = 20U;
		constexpr size_t CHUNK_STORED_CRC_OFFSET = 24U;
		constexpr size_t CHUNK_UNCOMPRESSED_CRC_OFFSET = 28U;
		constexpr size_t CHUNK_FIRST_PAGE_ID_OFFSET = 32U;

		constexpr size_t CHILD_PAGE_ID_OFFSET = 0U;
		constexpr size_t CHILD_FIRST_CHUNK_OFFSET = 8U;
		constexpr size_t CHILD_FIRST_OFFSET_OFFSET = 16U;
		constexpr size_t CHILD_LOGICAL_BYTES_OFFSET = 24U;

		constexpr size_t DATA_VALUE_ID_OFFSET = 16U;
		constexpr size_t DATA_PAGE_ID_OFFSET = 24U;
		constexpr size_t DATA_GENERATION_OFFSET = 32U;
		constexpr size_t DATA_CHUNK_ORDINAL_OFFSET = 40U;
		constexpr size_t DATA_PAGE_ORDINAL_OFFSET = 44U;
		constexpr size_t DATA_PAGE_COUNT_OFFSET = 46U;
		constexpr size_t DATA_STORED_CHUNK_BYTES_OFFSET = 48U;
		constexpr size_t DATA_STORED_OFFSET_OFFSET = 52U;
		constexpr size_t DATA_PAYLOAD_BYTES_OFFSET = 56U;
		constexpr size_t DATA_PAYLOAD_CRC_OFFSET = 60U;
		constexpr size_t DATA_HEADER_CRC_OFFSET = 64U;

		constexpr size_t REF_GENERATION_OFFSET = 32U;
		constexpr size_t REF_LOGICAL_BYTES_OFFSET = 40U;
		constexpr size_t REF_CONTENT_HASH_OFFSET = 48U;
		constexpr size_t REF_PAYLOAD_CRC_OFFSET = 80U;
		constexpr size_t REF_HEADER_CRC_OFFSET = 84U;

		[[noreturn]] void fail(FormatError error, const char *message) { throw FormatException(error, message); }

		[[nodiscard]] uint32_t checksum(std::span<const std::byte> bytes) {
			const std::byte empty{};
			return utils::calculateCrc(bytes.empty() ? &empty : bytes.data(), bytes.size());
		}

		[[nodiscard]] bool isKind(LargeValueKind kind) {
			return kind == LargeValueKind::LVK_BYTES || kind == LargeValueKind::LVK_UTF8_TEXT;
		}

		[[nodiscard]] bool isChunkCodec(LargeValueCodec codec) {
			return codec == LargeValueCodec::LVC_RAW || codec == LargeValueCodec::LVC_LZ4 ||
				   codec == LargeValueCodec::LVC_ZSTD;
		}

		[[nodiscard]] bool isReferenceCodec(LargeValueCodec codec) {
			return isChunkCodec(codec) || codec == LargeValueCodec::LVC_MIXED;
		}

		[[nodiscard]] bool hasContentHash(const std::array<std::byte, RSS_LARGE_VALUE_CONTENT_HASH_BYTES> &hash) {
			return std::ranges::any_of(hash, [](std::byte value) { return value != std::byte{}; });
		}

		void validateMediaType(const std::string &mediaType) {
			if (mediaType.empty() || mediaType.size() > RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "LargeValue media type must be nonempty and within the format policy bound");
			}
			for (const unsigned char character: mediaType) {
				if (character < 0x20U || character > 0x7EU) {
					fail(FormatError::FERR_INVALID_HEADER, "LargeValue media type must be printable ASCII metadata");
				}
			}
		}

		void validateReference(const LargeValueRef &reference) {
			const uint8_t invalid = static_cast<uint8_t>(!reference.value_id.isValid()) |
									static_cast<uint8_t>(!reference.generation.isValid()) |
									static_cast<uint8_t>(!hasContentHash(reference.content_hash)) |
									static_cast<uint8_t>(!isKind(reference.kind)) |
									static_cast<uint8_t>(!isReferenceCodec(reference.codec));
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue reference has invalid logical metadata");
			}
			validateMediaType(reference.media_type);
		}

		[[nodiscard]] uint16_t expectedDataPageCount(uint32_t storedBytes) {
			if (storedBytes == 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue chunk has no stored bytes");
			}
			const uint64_t pages =
					(static_cast<uint64_t>(storedBytes) + RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES - 1U) /
					RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES;
			if (pages > std::numeric_limits<uint16_t>::max()) { // Cannot occur with the current uint32 byte field.
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue chunk requires too many data pages");
			}
			return static_cast<uint16_t>(pages);
		}

		void validateManifest(const LargeValueManifest &manifest) {
			const LargeValueRef reference = manifest.reference();
			validateReference(reference);
			const bool root = manifest.node_kind == LargeValueManifestNodeKind::LVMNK_ROOT;
			const bool internal = manifest.node_kind == LargeValueManifestNodeKind::LVMNK_INTERNAL;
			const bool leaf = manifest.node_kind == LargeValueManifestNodeKind::LVMNK_LEAF;
			const uint8_t invalid =
					static_cast<uint8_t>(!root && !internal && !leaf) |
					static_cast<uint8_t>(!manifest.page_id.isValid()) |
					static_cast<uint8_t>(!manifest.committed_lsn.isValid()) |
					static_cast<uint8_t>(manifest.chunks.size() > RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS) |
					static_cast<uint8_t>(manifest.children.size() > RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN) |
					static_cast<uint8_t>(root && manifest.node_level == 0U && !manifest.children.empty()) |
					static_cast<uint8_t>(root && manifest.node_level != 0U && !manifest.chunks.empty()) |
					static_cast<uint8_t>(internal && manifest.node_level == 0U) |
					static_cast<uint8_t>(internal && manifest.children.empty()) |
					static_cast<uint8_t>(internal && !manifest.chunks.empty()) |
					static_cast<uint8_t>(leaf && manifest.node_level != 0U) |
					static_cast<uint8_t>(leaf && !manifest.children.empty()) |
					static_cast<uint8_t>(manifest.logical_bytes == 0U &&
										 (!manifest.chunks.empty() || !manifest.children.empty()));
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue manifest has an invalid node shape");
			}

			const uint64_t nodeSpan = root ? manifest.logical_bytes : manifest.node_logical_bytes;
			if ((manifest.logical_bytes != 0U && nodeSpan == 0U) ||
				manifest.first_uncompressed_offset > std::numeric_limits<uint64_t>::max() - nodeSpan ||
				(root && (manifest.first_chunk_ordinal != 0U || manifest.first_uncompressed_offset != 0U ||
						  manifest.node_logical_bytes != 0U))) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue manifest node range is invalid");
			}

			uint64_t expectedOffset = manifest.first_uncompressed_offset;
			uint32_t expectedOrdinal = manifest.first_chunk_ordinal;
			LargeValueCodec firstCodec = LargeValueCodec::LVC_RAW;
			bool mixedCodecs = false;
			for (size_t index = 0U; index < manifest.chunks.size(); ++index) {
				const LargeValueChunk &chunk = manifest.chunks[index];
				const uint8_t invalidChunk =
						static_cast<uint8_t>(chunk.ordinal != expectedOrdinal) |
						static_cast<uint8_t>(!isChunkCodec(chunk.codec)) |
						static_cast<uint8_t>(chunk.uncompressed_offset != expectedOffset) |
						static_cast<uint8_t>(chunk.uncompressed_bytes == 0U) |
						static_cast<uint8_t>(!chunk.first_data_page_id.isValid()) |
						static_cast<uint8_t>(chunk.data_page_count != expectedDataPageCount(chunk.stored_bytes));
				if (invalidChunk != 0U ||
					chunk.first_data_page_id.value() > std::numeric_limits<uint64_t>::max() -
															   (static_cast<uint64_t>(chunk.data_page_count) - 1U) ||
					expectedOffset > std::numeric_limits<uint64_t>::max() - chunk.uncompressed_bytes) {
					fail(FormatError::FERR_INVALID_HEADER, "LargeValue manifest chunk is malformed");
				}
				if (index == 0U) {
					firstCodec = chunk.codec;
				} else {
					mixedCodecs = mixedCodecs || chunk.codec != firstCodec;
				}
				expectedOffset += chunk.uncompressed_bytes;
				if (expectedOrdinal == std::numeric_limits<uint32_t>::max()) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue chunk ordinal overflows its wire width");
				}
				++expectedOrdinal;
			}
			if (!manifest.chunks.empty() && expectedOffset != manifest.first_uncompressed_offset + nodeSpan) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue manifest chunk range is not contiguous");
			}
			if (root && !manifest.chunks.empty() &&
				manifest.codec != (mixedCodecs ? LargeValueCodec::LVC_MIXED : firstCodec)) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue manifest aggregate codec does not match its chunks");
			}
			uint64_t childOffset = manifest.first_uncompressed_offset;
			uint32_t childOrdinal = manifest.first_chunk_ordinal;
			for (const LargeValueManifestChild &child: manifest.children) {
				if (!child.page_id.isValid() || child.first_chunk_ordinal != childOrdinal || child.chunk_count == 0U ||
					child.uncompressed_offset != childOffset || child.logical_bytes == 0U ||
					child.logical_bytes > nodeSpan - (childOffset - manifest.first_uncompressed_offset) ||
					child.first_chunk_ordinal > std::numeric_limits<uint32_t>::max() - 1U) {
					fail(FormatError::FERR_INVALID_HEADER, "LargeValue manifest child range is malformed");
				}
				childOffset += child.logical_bytes;
				if (childOrdinal > std::numeric_limits<uint32_t>::max() - child.chunk_count) {
					fail(FormatError::FERR_ARITHMETIC_OVERFLOW,
						 "LargeValue manifest child ordinal overflows its wire width");
				}
				childOrdinal += child.chunk_count;
			}
			if (!manifest.children.empty() && childOffset != manifest.first_uncompressed_offset + nodeSpan) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue manifest child range is not contiguous");
			}
			if (root && manifest.node_level != 0U && manifest.children.empty()) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue manifest internal node has no children");
			}
		}

		void validateDataPage(const LargeValueDataPage &page) {
			const uint8_t invalid =
					static_cast<uint8_t>(!page.value_id.isValid()) | static_cast<uint8_t>(!page.page_id.isValid()) |
					static_cast<uint8_t>(!page.generation.isValid()) | static_cast<uint8_t>(page.page_count == 0U) |
					static_cast<uint8_t>(page.page_ordinal >= page.page_count) |
					static_cast<uint8_t>(page.stored_chunk_bytes == 0U) | static_cast<uint8_t>(page.payload.empty()) |
					static_cast<uint8_t>(page.payload.size() > RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES) |
					static_cast<uint8_t>(page.stored_offset >= page.stored_chunk_bytes) |
					static_cast<uint8_t>(page.payload.size() > page.stored_chunk_bytes - page.stored_offset) |
					static_cast<uint8_t>(page.page_count != expectedDataPageCount(page.stored_chunk_bytes));
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_HEADER, "LargeValue data page has invalid chunk bounds");
			}
			const uint32_t expectedOffset =
					static_cast<uint32_t>(page.page_ordinal) * RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES;
			const uint32_t expectedBytes =
					std::min(RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES, page.stored_chunk_bytes - expectedOffset);
			if (page.stored_offset != expectedOffset || page.payload.size() != expectedBytes) {
				fail(FormatError::FERR_INVALID_HEADER,
					 "LargeValue data page does not cover its canonical stored-byte range");
			}
		}

		void validateManifestOwner(const PageOwnerHeader &owner, const LargeValueManifest &manifest,
								   bool allowRetired) {
			const uint8_t invalid = static_cast<uint8_t>(owner.page_id != manifest.page_id) |
									static_cast<uint8_t>(owner.owner_id != manifest.value_id.value()) |
									static_cast<uint8_t>(owner.kind != PageOwnerKind::POK_LARGE_VALUE_MANIFEST) |
									static_cast<uint8_t>((owner.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) != 0U) |
									static_cast<uint8_t>(!allowRetired && owner.flags != 0U) |
									static_cast<uint8_t>(owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
									static_cast<uint8_t>(!owner.generation.isValid()) |
									static_cast<uint8_t>(!owner.committed_lsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE,
					 "LargeValue manifest page owner disagrees with the immutable manifest");
			}
		}

		void validateDataOwner(const PageOwnerHeader &owner, const LargeValueDataPage &page, bool allowRetired) {
			const uint8_t invalid = static_cast<uint8_t>(owner.page_id != page.page_id) |
									static_cast<uint8_t>(owner.owner_id != page.value_id.value()) |
									static_cast<uint8_t>(owner.kind != PageOwnerKind::POK_LARGE_VALUE_DATA) |
									static_cast<uint8_t>((owner.flags & ~RSS_PAGE_OWNER_KNOWN_FLAGS) != 0U) |
									static_cast<uint8_t>(!allowRetired && owner.flags != 0U) |
									static_cast<uint8_t>(owner.payload_bytes != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) |
									static_cast<uint8_t>(!owner.generation.isValid()) |
									static_cast<uint8_t>(!owner.committed_lsn.isValid());
			if (invalid != 0U) {
				fail(FormatError::FERR_INVALID_STATE, "LargeValue data frame owner disagrees with chunk identity");
			}
		}

		void writeBytes(std::span<std::byte> target, size_t offset, std::span<const std::byte> source) {
			if (source.size() > target.size() - offset) {
				fail(FormatError::FERR_ARITHMETIC_OVERFLOW, "LargeValue codec output exceeds its page capacity");
			}
			std::copy(source.begin(), source.end(), target.begin() + static_cast<std::ptrdiff_t>(offset));
		}

		[[nodiscard]] std::array<std::byte, RSS_LARGE_VALUE_CONTENT_HASH_BYTES>
		readHash(std::span<const std::byte> bytes, size_t offset) {
			std::array<std::byte, RSS_LARGE_VALUE_CONTENT_HASH_BYTES> hash{};
			std::copy_n(bytes.begin() + static_cast<std::ptrdiff_t>(offset), hash.size(), hash.begin());
			return hash;
		}

		void requireZero(std::span<const std::byte> bytes, const char *message) {
			if (std::ranges::any_of(bytes, [](std::byte value) { return value != std::byte{}; })) {
				fail(FormatError::FERR_RESERVED_BITS, message);
			}
		}

	} // namespace

	LargeValueRef LargeValueManifest::reference() const {
		return {value_id, generation, logical_bytes, content_hash, kind, codec, media_type};
	}

	std::vector<std::byte> LargeValueRefCodec::encode(const LargeValueRef &reference) {
		validateReference(reference);
		std::vector<std::byte> bytes(RSS_LARGE_VALUE_REF_HEADER_BYTES + reference.media_type.size());
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_LARGE_VALUE_REF_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_LARGE_VALUE_REF_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_LARGE_VALUE_REF_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, FLAGS_OFFSET, 0U);
		LittleEndianCodec::writeU16(bytes, KIND_OFFSET, static_cast<uint16_t>(reference.kind));
		LittleEndianCodec::writeU16(bytes, CODEC_OFFSET, static_cast<uint16_t>(reference.codec));
		LittleEndianCodec::writeU16(bytes, MEDIA_TYPE_BYTES_OFFSET, static_cast<uint16_t>(reference.media_type.size()));
		LittleEndianCodec::writeU16(bytes, ENTRY_BYTES_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, CHUNK_COUNT_OFFSET, 0U);
		LittleEndianCodec::writeU64(bytes, VALUE_ID_OFFSET, reference.value_id.value());
		LittleEndianCodec::writeU64(bytes, REF_GENERATION_OFFSET, reference.generation.value());
		LittleEndianCodec::writeU64(bytes, REF_LOGICAL_BYTES_OFFSET, reference.logical_bytes);
		writeBytes(bytes, REF_CONTENT_HASH_OFFSET, reference.content_hash);
		writeBytes(bytes, RSS_LARGE_VALUE_REF_HEADER_BYTES,
				   std::as_bytes(std::span(reference.media_type.data(), reference.media_type.size())));
		LittleEndianCodec::writeU32(bytes, REF_PAYLOAD_CRC_OFFSET,
									checksum(std::span(bytes).subspan(RSS_LARGE_VALUE_REF_HEADER_BYTES)));
		LittleEndianCodec::writeU32(bytes, REF_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, REF_HEADER_CRC_OFFSET,
									checksum(std::span(bytes).first(RSS_LARGE_VALUE_REF_HEADER_BYTES)));
		return bytes;
	}

	LargeValueRef LargeValueRefCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_LARGE_VALUE_REF_HEADER_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "LargeValue reference is truncated");
		}
		std::array<std::byte, RSS_LARGE_VALUE_REF_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, REF_HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, REF_HEADER_CRC_OFFSET, 0U);
		if (checksum(header) != expectedHeaderCrc) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "LargeValue reference header checksum does not match");
		}
		const uint16_t mediaTypeBytes = LittleEndianCodec::readU16(bytes, MEDIA_TYPE_BYTES_OFFSET);
		const uint8_t invalid =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) != RSS_LARGE_VALUE_REF_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) != RSS_LARGE_VALUE_REF_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
									 RSS_LARGE_VALUE_REF_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, ENTRY_BYTES_OFFSET) != 0U) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, CHUNK_COUNT_OFFSET) != 0U) |
				static_cast<uint8_t>(mediaTypeBytes > RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES) |
				static_cast<uint8_t>(bytes.size() != RSS_LARGE_VALUE_REF_HEADER_BYTES + mediaTypeBytes);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue reference header is invalid");
		}
		requireZero(bytes.subspan(88U, RSS_LARGE_VALUE_REF_HEADER_BYTES - 88U),
					"LargeValue reference reserved bytes are nonzero");
		if (checksum(bytes.subspan(RSS_LARGE_VALUE_REF_HEADER_BYTES)) !=
			LittleEndianCodec::readU32(bytes, REF_PAYLOAD_CRC_OFFSET)) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "LargeValue reference media type checksum does not match");
		}
		LargeValueRef reference;
		reference.value_id = LargeValueId(LittleEndianCodec::readU64(bytes, VALUE_ID_OFFSET));
		reference.generation = Generation(LittleEndianCodec::readU64(bytes, REF_GENERATION_OFFSET));
		reference.logical_bytes = LittleEndianCodec::readU64(bytes, REF_LOGICAL_BYTES_OFFSET);
		reference.content_hash = readHash(bytes, REF_CONTENT_HASH_OFFSET);
		reference.kind = LargeValueKind(LittleEndianCodec::readU16(bytes, KIND_OFFSET));
		reference.codec = LargeValueCodec(LittleEndianCodec::readU16(bytes, CODEC_OFFSET));
		const char *media = reinterpret_cast<const char *>(bytes.data() + RSS_LARGE_VALUE_REF_HEADER_BYTES);
		reference.media_type.assign(media, mediaTypeBytes);
		validateReference(reference);
		return reference;
	}

	std::vector<std::byte> LargeValueManifestPageCodec::encode(const LargeValueManifest &manifest) {
		validateManifest(manifest);
		std::vector<std::byte> bytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_LARGE_VALUE_MANIFEST_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_LARGE_VALUE_MANIFEST_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, FLAGS_OFFSET, static_cast<uint32_t>(manifest.node_kind));
		LittleEndianCodec::writeU16(bytes, KIND_OFFSET, static_cast<uint16_t>(manifest.kind));
		LittleEndianCodec::writeU16(bytes, CODEC_OFFSET, static_cast<uint16_t>(manifest.codec));
		LittleEndianCodec::writeU16(bytes, MEDIA_TYPE_BYTES_OFFSET, static_cast<uint16_t>(manifest.media_type.size()));
		const bool childEntries = !manifest.children.empty();
		const uint16_t entryBytes =
				childEntries ? RSS_LARGE_VALUE_MANIFEST_CHILD_ENTRY_BYTES : RSS_LARGE_VALUE_MANIFEST_CHUNK_ENTRY_BYTES;
		const uint32_t entryCount =
				static_cast<uint32_t>(childEntries ? manifest.children.size() : manifest.chunks.size());
		LittleEndianCodec::writeU16(bytes, ENTRY_BYTES_OFFSET, entryBytes);
		LittleEndianCodec::writeU32(bytes, CHUNK_COUNT_OFFSET, entryCount);
		LittleEndianCodec::writeU64(bytes, VALUE_ID_OFFSET, manifest.value_id.value());
		LittleEndianCodec::writeU64(bytes, PAGE_ID_OFFSET, manifest.page_id.value());
		LittleEndianCodec::writeU64(bytes, GENERATION_OFFSET, manifest.generation.value());
		LittleEndianCodec::writeU64(bytes, LSN_OFFSET, manifest.committed_lsn.value());
		LittleEndianCodec::writeU64(bytes, LOGICAL_BYTES_OFFSET, manifest.logical_bytes);
		writeBytes(bytes, CONTENT_HASH_OFFSET, manifest.content_hash);
		writeBytes(bytes, MEDIA_TYPE_OFFSET,
				   std::as_bytes(std::span(manifest.media_type.data(), manifest.media_type.size())));
		LittleEndianCodec::writeU16(bytes, NODE_LEVEL_OFFSET, manifest.node_level);
		LittleEndianCodec::writeU32(bytes, NODE_FIRST_CHUNK_OFFSET, manifest.first_chunk_ordinal);
		LittleEndianCodec::writeU64(bytes, NODE_FIRST_OFFSET_OFFSET, manifest.first_uncompressed_offset);
		LittleEndianCodec::writeU64(bytes, NODE_LOGICAL_BYTES_OFFSET, manifest.node_logical_bytes);
		if (childEntries) {
			for (size_t index = 0U; index < manifest.children.size(); ++index) {
				const size_t offset =
						RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES + index * RSS_LARGE_VALUE_MANIFEST_CHILD_ENTRY_BYTES;
				const LargeValueManifestChild &child = manifest.children[index];
				LittleEndianCodec::writeU64(bytes, offset + CHILD_PAGE_ID_OFFSET, child.page_id.value());
				LittleEndianCodec::writeU32(bytes, offset + CHILD_FIRST_CHUNK_OFFSET, child.first_chunk_ordinal);
				LittleEndianCodec::writeU32(bytes, offset + CHILD_FIRST_CHUNK_OFFSET + 4U, child.chunk_count);
				LittleEndianCodec::writeU64(bytes, offset + CHILD_FIRST_OFFSET_OFFSET, child.uncompressed_offset);
				LittleEndianCodec::writeU64(bytes, offset + CHILD_LOGICAL_BYTES_OFFSET, child.logical_bytes);
			}
		} else {
			for (size_t index = 0U; index < manifest.chunks.size(); ++index) {
				const size_t offset =
						RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES + index * RSS_LARGE_VALUE_MANIFEST_CHUNK_ENTRY_BYTES;
				const LargeValueChunk &chunk = manifest.chunks[index];
				LittleEndianCodec::writeU32(bytes, offset + CHUNK_ORDINAL_OFFSET, chunk.ordinal);
				LittleEndianCodec::writeU16(bytes, offset + CHUNK_CODEC_OFFSET, static_cast<uint16_t>(chunk.codec));
				LittleEndianCodec::writeU16(bytes, offset + CHUNK_PAGE_COUNT_OFFSET, chunk.data_page_count);
				LittleEndianCodec::writeU64(bytes, offset + CHUNK_UNCOMPRESSED_OFFSET, chunk.uncompressed_offset);
				LittleEndianCodec::writeU32(bytes, offset + CHUNK_STORED_BYTES_OFFSET, chunk.stored_bytes);
				LittleEndianCodec::writeU32(bytes, offset + CHUNK_UNCOMPRESSED_BYTES_OFFSET, chunk.uncompressed_bytes);
				LittleEndianCodec::writeU32(bytes, offset + CHUNK_STORED_CRC_OFFSET, chunk.stored_crc);
				LittleEndianCodec::writeU32(bytes, offset + CHUNK_UNCOMPRESSED_CRC_OFFSET, chunk.uncompressed_crc);
				LittleEndianCodec::writeU64(bytes, offset + CHUNK_FIRST_PAGE_ID_OFFSET,
											chunk.first_data_page_id.value());
			}
		}
		LittleEndianCodec::writeU32(bytes, PAYLOAD_CRC_OFFSET,
									checksum(std::span(bytes).subspan(RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES)));
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, HEADER_CRC_OFFSET,
									checksum(std::span(bytes).first(RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES)));
		return bytes;
	}

	LargeValueManifest LargeValueManifestPageCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "LargeValue manifest page is truncated");
		}
		if (bytes.size() != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue manifest page has an invalid size");
		}
		std::array<std::byte, RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, HEADER_CRC_OFFSET, 0U);
		if (checksum(header) != expectedHeaderCrc) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "LargeValue manifest header checksum does not match");
		}
		const uint16_t mediaTypeBytes = LittleEndianCodec::readU16(bytes, MEDIA_TYPE_BYTES_OFFSET);
		const uint16_t entryBytes = LittleEndianCodec::readU16(bytes, ENTRY_BYTES_OFFSET);
		const uint32_t entryCount = LittleEndianCodec::readU32(bytes, CHUNK_COUNT_OFFSET);
		const LargeValueManifestNodeKind nodeKind =
				static_cast<LargeValueManifestNodeKind>(LittleEndianCodec::readU32(bytes, FLAGS_OFFSET));
		const bool childEntries = nodeKind == LargeValueManifestNodeKind::LVMNK_INTERNAL ||
								  (nodeKind == LargeValueManifestNodeKind::LVMNK_ROOT &&
								   LittleEndianCodec::readU16(bytes, NODE_LEVEL_OFFSET) != 0U);
		const uint16_t expectedEntryBytes =
				childEntries ? RSS_LARGE_VALUE_MANIFEST_CHILD_ENTRY_BYTES : RSS_LARGE_VALUE_MANIFEST_CHUNK_ENTRY_BYTES;
		const uint8_t invalid = static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) !=
													 RSS_LARGE_VALUE_MANIFEST_MAGIC) |
								static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) !=
													 RSS_LARGE_VALUE_MANIFEST_VERSION) |
								static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
													 RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES) |
								static_cast<uint8_t>(nodeKind != LargeValueManifestNodeKind::LVMNK_ROOT &&
													 nodeKind != LargeValueManifestNodeKind::LVMNK_INTERNAL &&
													 nodeKind != LargeValueManifestNodeKind::LVMNK_LEAF) |
								static_cast<uint8_t>(entryBytes != expectedEntryBytes) |
								static_cast<uint8_t>(mediaTypeBytes > RSS_LARGE_VALUE_MAX_MEDIA_TYPE_BYTES) |
								static_cast<uint8_t>(entryCount > (childEntries ? RSS_LARGE_VALUE_MANIFEST_MAX_CHILDREN
																				: RSS_LARGE_VALUE_MANIFEST_MAX_CHUNKS));
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue manifest header is invalid");
		}
		if (checksum(bytes.subspan(RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES)) !=
			LittleEndianCodec::readU32(bytes, PAYLOAD_CRC_OFFSET)) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "LargeValue manifest payload checksum does not match");
		}
		requireZero(bytes.subspan(MEDIA_TYPE_OFFSET + mediaTypeBytes,
								  RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES - MEDIA_TYPE_OFFSET - mediaTypeBytes),
					"LargeValue manifest media-type padding is nonzero");
		const size_t usedPayload = static_cast<size_t>(entryCount) * entryBytes;
		requireZero(bytes.subspan(RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES + usedPayload),
					"LargeValue manifest trailing bytes are nonzero");

		LargeValueManifest manifest;
		manifest.value_id = LargeValueId(LittleEndianCodec::readU64(bytes, VALUE_ID_OFFSET));
		manifest.page_id = PageId(LittleEndianCodec::readU64(bytes, PAGE_ID_OFFSET));
		manifest.generation = Generation(LittleEndianCodec::readU64(bytes, GENERATION_OFFSET));
		manifest.committed_lsn = LogSequenceNumber(LittleEndianCodec::readU64(bytes, LSN_OFFSET));
		manifest.logical_bytes = LittleEndianCodec::readU64(bytes, LOGICAL_BYTES_OFFSET);
		manifest.content_hash = readHash(bytes, CONTENT_HASH_OFFSET);
		manifest.kind = LargeValueKind(LittleEndianCodec::readU16(bytes, KIND_OFFSET));
		manifest.codec = LargeValueCodec(LittleEndianCodec::readU16(bytes, CODEC_OFFSET));
		manifest.node_kind = nodeKind;
		manifest.node_level = LittleEndianCodec::readU16(bytes, NODE_LEVEL_OFFSET);
		manifest.first_chunk_ordinal = LittleEndianCodec::readU32(bytes, NODE_FIRST_CHUNK_OFFSET);
		manifest.first_uncompressed_offset = LittleEndianCodec::readU64(bytes, NODE_FIRST_OFFSET_OFFSET);
		manifest.node_logical_bytes = LittleEndianCodec::readU64(bytes, NODE_LOGICAL_BYTES_OFFSET);
		const char *media = reinterpret_cast<const char *>(bytes.data() + MEDIA_TYPE_OFFSET);
		manifest.media_type.assign(media, mediaTypeBytes);
		if (childEntries) {
			manifest.children.reserve(entryCount);
			for (uint32_t index = 0U; index < entryCount; ++index) {
				const size_t offset =
						RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES + index * RSS_LARGE_VALUE_MANIFEST_CHILD_ENTRY_BYTES;
				LargeValueManifestChild child;
				child.page_id = PageId(LittleEndianCodec::readU64(bytes, offset + CHILD_PAGE_ID_OFFSET));
				child.first_chunk_ordinal = LittleEndianCodec::readU32(bytes, offset + CHILD_FIRST_CHUNK_OFFSET);
				child.chunk_count = LittleEndianCodec::readU32(bytes, offset + CHILD_FIRST_CHUNK_OFFSET + 4U);
				child.uncompressed_offset = LittleEndianCodec::readU64(bytes, offset + CHILD_FIRST_OFFSET_OFFSET);
				child.logical_bytes = LittleEndianCodec::readU64(bytes, offset + CHILD_LOGICAL_BYTES_OFFSET);
				manifest.children.push_back(child);
			}
		} else {
			manifest.chunks.reserve(entryCount);
			for (uint32_t index = 0U; index < entryCount; ++index) {
				const size_t offset = RSS_LARGE_VALUE_MANIFEST_HEADER_BYTES +
									  static_cast<size_t>(index) * RSS_LARGE_VALUE_MANIFEST_CHUNK_ENTRY_BYTES;
				LargeValueChunk chunk;
				chunk.ordinal = LittleEndianCodec::readU32(bytes, offset + CHUNK_ORDINAL_OFFSET);
				chunk.codec = LargeValueCodec(LittleEndianCodec::readU16(bytes, offset + CHUNK_CODEC_OFFSET));
				chunk.data_page_count = LittleEndianCodec::readU16(bytes, offset + CHUNK_PAGE_COUNT_OFFSET);
				chunk.uncompressed_offset = LittleEndianCodec::readU64(bytes, offset + CHUNK_UNCOMPRESSED_OFFSET);
				chunk.stored_bytes = LittleEndianCodec::readU32(bytes, offset + CHUNK_STORED_BYTES_OFFSET);
				chunk.uncompressed_bytes = LittleEndianCodec::readU32(bytes, offset + CHUNK_UNCOMPRESSED_BYTES_OFFSET);
				chunk.stored_crc = LittleEndianCodec::readU32(bytes, offset + CHUNK_STORED_CRC_OFFSET);
				chunk.uncompressed_crc = LittleEndianCodec::readU32(bytes, offset + CHUNK_UNCOMPRESSED_CRC_OFFSET);
				chunk.first_data_page_id =
						PageId(LittleEndianCodec::readU64(bytes, offset + CHUNK_FIRST_PAGE_ID_OFFSET));
				manifest.chunks.push_back(chunk);
			}
		}
		validateManifest(manifest);
		return manifest;
	}

	std::vector<std::byte> LargeValueDataPageCodec::encode(const LargeValueDataPage &page) {
		validateDataPage(page);
		std::vector<std::byte> bytes(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES);
		LittleEndianCodec::writeU32(bytes, MAGIC_OFFSET, RSS_LARGE_VALUE_DATA_PAGE_MAGIC);
		LittleEndianCodec::writeU16(bytes, VERSION_OFFSET, RSS_LARGE_VALUE_DATA_PAGE_VERSION);
		LittleEndianCodec::writeU16(bytes, HEADER_BYTES_OFFSET, RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES);
		LittleEndianCodec::writeU32(bytes, FLAGS_OFFSET, 0U);
		LittleEndianCodec::writeU64(bytes, DATA_VALUE_ID_OFFSET, page.value_id.value());
		LittleEndianCodec::writeU64(bytes, DATA_PAGE_ID_OFFSET, page.page_id.value());
		LittleEndianCodec::writeU64(bytes, DATA_GENERATION_OFFSET, page.generation.value());
		LittleEndianCodec::writeU32(bytes, DATA_CHUNK_ORDINAL_OFFSET, page.chunk_ordinal);
		LittleEndianCodec::writeU16(bytes, DATA_PAGE_ORDINAL_OFFSET, page.page_ordinal);
		LittleEndianCodec::writeU16(bytes, DATA_PAGE_COUNT_OFFSET, page.page_count);
		LittleEndianCodec::writeU32(bytes, DATA_STORED_CHUNK_BYTES_OFFSET, page.stored_chunk_bytes);
		LittleEndianCodec::writeU32(bytes, DATA_STORED_OFFSET_OFFSET, page.stored_offset);
		LittleEndianCodec::writeU32(bytes, DATA_PAYLOAD_BYTES_OFFSET, static_cast<uint32_t>(page.payload.size()));
		LittleEndianCodec::writeU32(bytes, DATA_PAYLOAD_CRC_OFFSET, checksum(page.payload));
		writeBytes(bytes, RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES, page.payload);
		LittleEndianCodec::writeU32(bytes, DATA_HEADER_CRC_OFFSET, 0U);
		LittleEndianCodec::writeU32(bytes, DATA_HEADER_CRC_OFFSET,
									checksum(std::span(bytes).first(RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES)));
		return bytes;
	}

	LargeValueDataPage LargeValueDataPageCodec::decode(std::span<const std::byte> bytes) {
		if (bytes.size() < RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			fail(FormatError::FERR_BUFFER_TOO_SMALL, "LargeValue data page is truncated");
		}
		if (bytes.size() != RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES) {
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue data page has an invalid size");
		}
		std::array<std::byte, RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES> header{};
		std::copy_n(bytes.begin(), header.size(), header.begin());
		const uint32_t expectedHeaderCrc = LittleEndianCodec::readU32(header, DATA_HEADER_CRC_OFFSET);
		LittleEndianCodec::writeU32(header, DATA_HEADER_CRC_OFFSET, 0U);
		if (checksum(header) != expectedHeaderCrc) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "LargeValue data page header checksum does not match");
		}
		const uint32_t payloadBytes = LittleEndianCodec::readU32(bytes, DATA_PAYLOAD_BYTES_OFFSET);
		const uint8_t invalid =
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, MAGIC_OFFSET) !=
									 RSS_LARGE_VALUE_DATA_PAGE_MAGIC) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, VERSION_OFFSET) !=
									 RSS_LARGE_VALUE_DATA_PAGE_VERSION) |
				static_cast<uint8_t>(LittleEndianCodec::readU16(bytes, HEADER_BYTES_OFFSET) !=
									 RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES) |
				static_cast<uint8_t>(LittleEndianCodec::readU32(bytes, FLAGS_OFFSET) != 0U) |
				static_cast<uint8_t>(payloadBytes == 0U || payloadBytes > RSS_LARGE_VALUE_DATA_PAGE_MAX_STORED_BYTES);
		if (invalid != 0U) {
			fail(FormatError::FERR_INVALID_HEADER, "LargeValue data page header is invalid");
		}
		const std::span<const std::byte> payload = bytes.subspan(RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES, payloadBytes);
		if (checksum(payload) != LittleEndianCodec::readU32(bytes, DATA_PAYLOAD_CRC_OFFSET)) {
			fail(FormatError::FERR_CHECKSUM_MISMATCH, "LargeValue data page payload checksum does not match");
		}
		requireZero(bytes.subspan(RSS_LARGE_VALUE_DATA_PAGE_HEADER_BYTES + payloadBytes),
					"LargeValue data page trailing bytes are nonzero");
		LargeValueDataPage page;
		page.value_id = LargeValueId(LittleEndianCodec::readU64(bytes, DATA_VALUE_ID_OFFSET));
		page.page_id = PageId(LittleEndianCodec::readU64(bytes, DATA_PAGE_ID_OFFSET));
		page.generation = Generation(LittleEndianCodec::readU64(bytes, DATA_GENERATION_OFFSET));
		page.chunk_ordinal = LittleEndianCodec::readU32(bytes, DATA_CHUNK_ORDINAL_OFFSET);
		page.page_ordinal = LittleEndianCodec::readU16(bytes, DATA_PAGE_ORDINAL_OFFSET);
		page.page_count = LittleEndianCodec::readU16(bytes, DATA_PAGE_COUNT_OFFSET);
		page.stored_chunk_bytes = LittleEndianCodec::readU32(bytes, DATA_STORED_CHUNK_BYTES_OFFSET);
		page.stored_offset = LittleEndianCodec::readU32(bytes, DATA_STORED_OFFSET_OFFSET);
		page.payload.assign(payload.begin(), payload.end());
		validateDataPage(page);
		return page;
	}

	LargeValueManifestPage::LargeValueManifestPage(std::vector<std::byte> bytes, LargeValueManifest manifest) :
		bytes_(std::move(bytes)), manifest_(std::move(manifest)) {}

	LargeValueManifestPage LargeValueManifestPage::create(const PageOwnerHeader &owner,
														  const LargeValueManifest &manifest) {
		validateManifest(manifest);
		validateManifestOwner(owner, manifest, false);
		return {LargeValueManifestPageCodec::encode(manifest), manifest};
	}

	LargeValueManifestPage LargeValueManifestPage::open(const PageOwnerHeader &owner,
														std::span<const std::byte> bytes) {
		LargeValueManifest manifest = LargeValueManifestPageCodec::decode(bytes);
		validateManifestOwner(owner, manifest, true);
		return {std::vector<std::byte>(bytes.begin(), bytes.end()), std::move(manifest)};
	}

	LargeValueManifestPage LargeValueManifestPage::rebindOwner(const PageOwnerHeader &owner) const {
		validateManifestOwner(owner, manifest_, false);
		return {bytes_, manifest_};
	}

	LargeValueDataFrame::LargeValueDataFrame(std::vector<std::byte> bytes, LargeValueDataPage page) :
		bytes_(std::move(bytes)), page_(std::move(page)) {}

	LargeValueDataFrame LargeValueDataFrame::create(const PageOwnerHeader &owner, const LargeValueDataPage &page) {
		validateDataPage(page);
		validateDataOwner(owner, page, false);
		return {LargeValueDataPageCodec::encode(page), page};
	}

	LargeValueDataFrame LargeValueDataFrame::open(const PageOwnerHeader &owner, std::span<const std::byte> bytes) {
		LargeValueDataPage page = LargeValueDataPageCodec::decode(bytes);
		validateDataOwner(owner, page, true);
		return {std::vector<std::byte>(bytes.begin(), bytes.end()), std::move(page)};
	}

	LargeValueDataFrame LargeValueDataFrame::rebindOwner(const PageOwnerHeader &owner) const {
		validateDataOwner(owner, page_, false);
		return {bytes_, page_};
	}

} // namespace graph::storage
