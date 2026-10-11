/**
 * @file GlobalAdjacencyMutationMerger.hpp
 * @brief Rewindable merge of one global adjacency tree and tagged final-state updates.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>

#include "graph/storage/relocatable/index/BTreePageStore.hpp"

namespace graph::storage {

	// The scratch stream uses the existing native graph convention: value one is
	// UPSERT and value two is ERASE. Output is always a key-only leaf stream.
	// This keeps the final-state merge independent from where the ordered updates
	// originated (ordinary overlay or streamed bulk candidate).
	class GlobalAdjacencyMutationMerger final {
	public:
		[[nodiscard]] static OrderedBTreeEntrySource
		merge(const OrderedBTreeEntrySource &existing, const OrderedBTreeEntrySource &updates, size_t maximumEntries);
	};

} // namespace graph::storage
