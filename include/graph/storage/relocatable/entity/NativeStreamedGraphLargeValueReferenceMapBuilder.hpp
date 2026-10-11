/**
 * @file NativeStreamedGraphLargeValueReferenceMapBuilder.hpp
 * @brief Bounded provisional-to-canonical LargeValue mapping runs.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>

#include "graph/storage/relocatable/entity/NativeStreamedGraphLargeValueContentRunBuilder.hpp"

namespace graph::storage {

	using NativeStreamedGraphLargeValueReferenceMapRunVisitor = std::function<void(NativeBulkRunFragment &&)>;

	struct NativeStreamedGraphLargeValueReferenceMapRunPlan {
		size_t initial_fragment_count = 0U;
		size_t candidate_segment_count = 0U;
	};

	// Converts the content-sorted run into a second run ordered by provisional
	// LargeValue ID. The final publisher reopens it for each source replay,
	// which keeps duplicate lookup bounded without retaining a batch-sized map.
	class NativeStreamedGraphLargeValueReferenceMapBuilder final {
	public:
		[[nodiscard]] static NativeStreamedGraphLargeValueReferenceMapRunPlan
		derivePlanForValueCount(size_t valueCount, size_t maximumRunBytes, size_t maximumOpenRuns);
		static void buildRuns(const OrderedBTreeEntrySource &contentEntries, const LargeValueStore &store,
							  LargeValueId firstValueId, size_t referenceCount, size_t maximumRunBytes,
							  size_t maximumFragments,
							  const NativeStreamedGraphLargeValueReferenceMapRunVisitor &visitor);
	};

} // namespace graph::storage
