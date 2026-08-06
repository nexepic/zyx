/**
 * @file VectorCodebook.hpp
 * @brief Explicit immutable codebook-extent header codec for relocatable vectors.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_VECTOR_CODEBOOK_MAGIC = 0x4356595AU; // Little-endian bytes: ZYVC
	constexpr uint16_t RSS_VECTOR_CODEBOOK_VERSION = 3;
	constexpr uint16_t RSS_VECTOR_CODEBOOK_HEADER_BYTES = 64;

	// All enumerators use prefixes to remain safe in Windows builds.
	enum class VectorCodebookElementType : uint16_t {
		VCET_FLOAT32 = 1,
		VCET_UINT8 = 2,
	};

	struct VectorCodebookHeader {
		uint64_t codebook_id = 0;
		// Immutable logical codebook version. It intentionally survives physical
		// Catalog/Extent relocation, whose generation is stored in the directory.
		Generation generation{};
		VectorCodebookElementType element_type = VectorCodebookElementType::VCET_FLOAT32;
		uint32_t dimensions = 0;
		// A flattened PQ payload contains `subspace_count * centroid_count`
		// centroids, each with `dimensions / subspace_count` coordinates.
		uint32_t subspace_count = 0;
		uint32_t centroid_count = 0;
		uint32_t element_bytes = 0;
		uint32_t payload_bytes = 0;
		uint32_t payload_crc = 0;

		constexpr bool operator==(const VectorCodebookHeader &) const = default;
	};

	class VectorCodebookHeaderCodec final {
	public:
		using Bytes = std::array<std::byte, RSS_VECTOR_CODEBOOK_HEADER_BYTES>;

		[[nodiscard]] static Bytes encode(const VectorCodebookHeader &header);
		[[nodiscard]] static VectorCodebookHeader decode(std::span<const std::byte> bytes);
		static void validatePayload(const VectorCodebookHeader &header, std::span<const std::byte> payload);
	};

} // namespace graph::storage
