/**
 * @file EntityLabelLayout.hpp
 * @brief Shared fixed-format constants for native Node label storage.
 *
 * @copyright Copyright (c) 2026 Nexepic
 * @license Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <cstdint>

namespace graph::storage {

	// These values are on-disk format choices, not inferred page capacities.
	constexpr size_t RSS_ENTITY_INLINE_LABEL_CAPACITY = 3U;
	constexpr size_t RSS_ENTITY_LABEL_TOKEN_BYTES = sizeof(uint64_t);

} // namespace graph::storage
