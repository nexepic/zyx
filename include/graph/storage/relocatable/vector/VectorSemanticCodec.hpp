/**
 * @file VectorSemanticCodec.hpp
 * @brief Canonical logical Vector encodings shared by migration and online writes.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "graph/core/PropertyTypes.hpp"
#include "graph/storage/relocatable/schema/IndexDefinition.hpp"
#include "graph/storage/relocatable/vector/VectorManifestPage.hpp"

namespace graph::storage {

	// A raw vector must fit one slotted Vector page after its header, one bitmap
	// byte, one slot-generation entry, and the durable Vector-ID record prefix
	// have been reserved.
	constexpr uint32_t RSS_VECTOR_SEMANTIC_MAX_DIMENSIONS =
			(RSS_PAGE_OWNER_MAX_PAYLOAD_BYTES - RSS_VECTOR_PAGE_HEADER_BYTES - 1U - sizeof(uint32_t) -
			 sizeof(uint64_t)) /
			2U;

	// This codec is the only conversion boundary between logical Properties and
	// durable raw-vector bytes. Offline migration and future online graph writes
	// use it so their exact-value representation cannot silently diverge.
	class VectorSemanticCodec final {
	public:
		[[nodiscard]] static std::vector<std::byte> encodeBFloat16(const PropertyValue &value, uint32_t dimensions);
		// Logical writes receive caller-owned Property values, so malformed vector
		// input is an argument error rather than evidence of a corrupt on-disk format.
		[[nodiscard]] static std::vector<std::byte> encodeBFloat16ForLogicalWrite(const PropertyValue &value,
																	  uint32_t dimensions);
		// A cosine index has no defined score for the zero vector. This validates
		// the already-canonical BF16 representation at logical-write and rebuild
		// boundaries without changing its durable identity.
		[[nodiscard]] static bool hasNonZeroFiniteL2Norm(std::span<const std::byte> rawBfloat16);
		[[nodiscard]] static std::vector<std::byte> rawRecord(uint64_t vectorId,
																	  std::span<const std::byte> rawBfloat16);
		// Big-endian keys retain Node-ID numeric order in the bytewise B+Tree comparator.
		[[nodiscard]] static std::vector<std::byte> nodeMappingKey(uint64_t nodeId);
		// Big-endian keys retain Vector-ID numeric order in every typed derived
		// mapping tree (PQ, adjacency, and Vector-to-Node semantic metadata).
		[[nodiscard]] static std::vector<std::byte> vectorMappingKey(uint64_t vectorId);
		[[nodiscard]] static VectorDistanceMetric metricFor(const IndexDefinition &definition);
	};

} // namespace graph::storage
