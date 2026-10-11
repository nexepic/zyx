/**
 * @file AdjacencyKeyCodec.hpp
 * @brief Canonical key-only Packed B+Tree keys for ordinary graph adjacency.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "graph/storage/relocatable/format/Format.hpp"

namespace graph::storage {

	// Internal B+Tree IDs use the high-bit namespace. This identity is reserved
	// for the one global ordinary-graph adjacency tree and cannot be a user index.
	constexpr uint64_t RSS_GLOBAL_ADJACENCY_INDEX_ID = 0x8000000000000002ULL;
	constexpr size_t RSS_GLOBAL_ADJACENCY_KEY_BYTES = 1U + 2U * sizeof(uint64_t);

	// Prefixes avoid Windows macro collisions in persisted enums.
	enum class GlobalAdjacencyDirection : uint8_t {
		GAD_OUT = 1U,
		GAD_IN = 2U,
	};

	struct GlobalAdjacencyKey {
		GlobalAdjacencyDirection direction = GlobalAdjacencyDirection::GAD_OUT;
		uint64_t node_id = 0U;
		uint64_t edge_id = 0U;

		constexpr bool operator==(const GlobalAdjacencyKey &) const = default;
	};

	// The byte encoding is order-preserving for the fixed comparator
	// (direction, unsigned NodeId, unsigned EdgeId). The generic disk format
	// still uses explicit codecs; this big-endian key encoding exists solely for
	// lexicographic B+Tree comparison.
	class AdjacencyKeyCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encode(const GlobalAdjacencyKey &key);
		[[nodiscard]] static GlobalAdjacencyKey decode(std::span<const std::byte> bytes);
		[[nodiscard]] static std::array<std::byte, RSS_GLOBAL_ADJACENCY_KEY_BYTES>
		lowerBoundFixed(GlobalAdjacencyDirection direction, uint64_t nodeId);
		[[nodiscard]] static std::vector<std::byte> lowerBound(GlobalAdjacencyDirection direction, uint64_t nodeId);
		[[nodiscard]] static bool matchesNodeDirection(std::span<const std::byte> bytes,
															 GlobalAdjacencyDirection direction, uint64_t nodeId);
	};

} // namespace graph::storage
