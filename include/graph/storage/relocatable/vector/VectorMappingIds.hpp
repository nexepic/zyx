/**
 * @file VectorMappingIds.hpp
 * @brief Disjoint metadata-owned B+Tree ID namespaces for vector mappings.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstdint>

namespace graph::storage {

	// Public index definitions never use the high bit. Derived vector mappings
	// use a typed 16-bit prefix so a damaged manifest cannot bind one cache or
	// semantic mapping tree as another kind of tree.
	constexpr uint64_t RSS_DERIVED_MAPPING_INDEX_TYPE_MASK = UINT64_C(0xFFFF000000000000);
	constexpr uint64_t RSS_PQ_MAPPING_INTERNAL_INDEX_PREFIX = UINT64_C(0xA501000000000000);
	constexpr uint64_t RSS_ADJACENCY_MAPPING_INTERNAL_INDEX_PREFIX = UINT64_C(0xA502000000000000);
	constexpr uint64_t RSS_NODE_MAPPING_INTERNAL_INDEX_PREFIX = UINT64_C(0xA503000000000000);
	constexpr uint64_t RSS_DELTA_MAPPING_INTERNAL_INDEX_PREFIX = UINT64_C(0xA504000000000000);

} // namespace graph::storage
