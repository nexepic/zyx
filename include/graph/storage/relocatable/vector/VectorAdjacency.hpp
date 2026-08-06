/**
 * @file VectorAdjacency.hpp
 * @brief Explicit variable-length DiskANN adjacency-record codec for relocatable.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "graph/storage/relocatable/format/Format.hpp"
#include "graph/storage/relocatable/allocation/SharedPagePool.hpp"

namespace graph::storage {

	constexpr uint32_t RSS_VECTOR_ADJACENCY_MAGIC = 0x4156595AU; // Little-endian bytes: ZYVA
	constexpr uint16_t RSS_VECTOR_ADJACENCY_VERSION = 1;
	constexpr uint16_t RSS_VECTOR_ADJACENCY_HEADER_BYTES = 32;
	constexpr uint16_t RSS_VECTOR_ADJACENCY_NEIGHBOR_BYTES = 8;
	constexpr uint16_t RSS_VECTOR_ADJACENCY_MAX_NEIGHBORS =
			(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_VECTOR_ADJACENCY_HEADER_BYTES) /
			RSS_VECTOR_ADJACENCY_NEIGHBOR_BYTES;

	struct VectorAdjacencyRecord {
		uint64_t vector_id = 0;
		std::vector<uint64_t> neighbors;

		constexpr bool operator==(const VectorAdjacencyRecord &) const = default;
	};

	class VectorAdjacencyCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const VectorAdjacencyRecord &record);
		[[nodiscard]] static VectorAdjacencyRecord decode(std::span<const std::byte> bytes);
	};

} // namespace graph::storage
