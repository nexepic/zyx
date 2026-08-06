/**
 * @file VectorCodebookExtent.hpp
 * @brief Immutable inline and multi-extent Vector Codebook layout for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "graph/storage/relocatable/vector/VectorCodebook.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_VECTOR_CODEBOOK_EXTENT_MAGIC = 0x4556595AU; // Little-endian bytes: ZYVE
	constexpr uint16_t RSS_VECTOR_CODEBOOK_EXTENT_VERSION = 1;
	constexpr uint16_t RSS_VECTOR_CODEBOOK_EXTENT_HEADER_BYTES = 64;
	constexpr uint32_t RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES =
			RSS_VECTOR_CODEBOOK_HEADER_BYTES + 8U;
	constexpr uint32_t RSS_VECTOR_CODEBOOK_MANIFEST_ENTRY_BYTES = 16U;
	constexpr uint32_t RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES =
			RSS_SEGMENT_SIZE - RSS_ENVELOPE_BYTES - RSS_VECTOR_CODEBOOK_EXTENT_HEADER_BYTES;
	constexpr uint32_t RSS_VECTOR_CODEBOOK_INLINE_PAYLOAD_BYTES =
			RSS_VECTOR_CODEBOOK_MAX_BLOCK_BYTES - RSS_VECTOR_CODEBOOK_MANIFEST_PREFIX_BYTES;
	constexpr uint32_t RSS_VECTOR_CODEBOOK_MANIFEST_ORDINAL = UINT32_MAX;
	// Codebooks are the sole remaining full-segment extent owner. Reserve the
	// high bit for data extents and keep manifest IDs in the low-half namespace.
	// This gives each codebook and data segment a disjoint, self-validating
	// directory identity without carrying retired object namespaces forward.
	constexpr uint64_t RSS_VECTOR_CODEBOOK_DATA_EXTENT_TAG = UINT64_C(1) << 63U;
	constexpr uint64_t RSS_VECTOR_CODEBOOK_EXTENT_PAYLOAD_MASK = ~RSS_VECTOR_CODEBOOK_DATA_EXTENT_TAG;

	// Prefixes avoid Windows macro collisions.
	enum class VectorCodebookExtentKind : uint16_t {
		VCEK_MANIFEST = 1,
		VCEK_DATA = 2,
	};

	struct VectorCodebookExtentHeader {
		VectorCodebookExtentKind kind = VectorCodebookExtentKind::VCEK_MANIFEST;
		uint64_t codebook_id = 0U;
		Generation codebook_generation{};
		ExtentId extent_id{};
		uint32_t ordinal = RSS_VECTOR_CODEBOOK_MANIFEST_ORDINAL;
		uint32_t block_bytes = 0U;
		uint32_t block_crc = 0U;

		constexpr bool operator==(const VectorCodebookExtentHeader &) const = default;
	};

	struct VectorCodebookManifestEntry {
		ExtentId extent_id{};
		uint32_t block_bytes = 0U;
		uint32_t block_crc = 0U;

		constexpr bool operator==(const VectorCodebookManifestEntry &) const = default;
	};

	struct VectorCodebookManifest {
		VectorCodebookHeader header;
		std::vector<VectorCodebookManifestEntry> entries;
		std::vector<std::byte> inline_payload;

		[[nodiscard]] bool isInline() const { return entries.empty(); }
		bool operator==(const VectorCodebookManifest &) const = default;
	};

	class VectorCodebookExtentIdCodec final {
	public:
		[[nodiscard]] static constexpr bool canEncodeCodebookId(uint64_t codebookId) {
			return codebookId != 0U && codebookId <= RSS_VECTOR_CODEBOOK_EXTENT_PAYLOAD_MASK;
		}
		[[nodiscard]] static constexpr ExtentId manifestExtentId(uint64_t codebookId) {
			return canEncodeCodebookId(codebookId) ? ExtentId(codebookId) : ExtentId{};
		}
		[[nodiscard]] static constexpr bool isManifestExtentForCodebook(ExtentId extentId, uint64_t codebookId) {
			return canEncodeCodebookId(codebookId) && extentId == manifestExtentId(codebookId);
		}
		[[nodiscard]] static constexpr bool canEncodeDataSegment(SegmentId segmentId) {
			return segmentId.isValid() && segmentId.value() <= RSS_VECTOR_CODEBOOK_EXTENT_PAYLOAD_MASK;
		}
		[[nodiscard]] static constexpr ExtentId dataExtentId(SegmentId segmentId) {
			return canEncodeDataSegment(segmentId)
					   ? ExtentId(RSS_VECTOR_CODEBOOK_DATA_EXTENT_TAG | segmentId.value())
					   : ExtentId{};
		}
		[[nodiscard]] static constexpr bool isDataExtentId(ExtentId extentId) {
			return extentId.isValid() && (extentId.value() & RSS_VECTOR_CODEBOOK_DATA_EXTENT_TAG) != 0U &&
				   (extentId.value() & RSS_VECTOR_CODEBOOK_EXTENT_PAYLOAD_MASK) != 0U;
		}
		[[nodiscard]] static constexpr bool isDataExtentForSegment(ExtentId extentId, SegmentId segmentId) {
			return canEncodeDataSegment(segmentId) && extentId == dataExtentId(segmentId);
		}
	};

	class VectorCodebookExtentHeaderCodec final {
	public:
		using Bytes = std::array<std::byte, RSS_VECTOR_CODEBOOK_EXTENT_HEADER_BYTES>;

		[[nodiscard]] static Bytes encode(const VectorCodebookExtentHeader &header);
		[[nodiscard]] static VectorCodebookExtentHeader decode(std::span<const std::byte> bytes);
		static void validateBlock(const VectorCodebookExtentHeader &header, std::span<const std::byte> block);
	};

	class VectorCodebookManifestCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const VectorCodebookManifest &manifest);
		[[nodiscard]] static VectorCodebookManifest decode(std::span<const std::byte> bytes);
		[[nodiscard]] static size_t requiredDataExtentCount(size_t payloadBytes);
	};

} // namespace graph::storage
