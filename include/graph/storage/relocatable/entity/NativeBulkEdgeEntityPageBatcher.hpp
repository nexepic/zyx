/**
 * @file NativeBulkEdgeEntityPageBatcher.hpp
 * @brief Page-local batching for streamed native Edge materialization.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>
#include <span>
#include <vector>

#include "graph/storage/relocatable/entity/NativeBulkEdgeMaterializer.hpp"

namespace graph::storage {

	// One batch contains no more records than one fixed Edge Entity page can
	// encode. It is a transient publication unit and intentionally has no PageId
	// or physical placement before the metadata writer owns the final fence.
	struct NativeBulkEdgeEntityPageBatch {
		std::vector<NativeBulkEdgeMaterializedRecord> records;
	};

	using NativeBulkEdgeEntityPageBatchVisitor =
			std::function<void(NativeBulkEdgeEntityPageBatch &&)>;

	class NativeBulkEdgeEntityPageBatcher final {
	public:
		// Resolves and emits one Edge-page-sized batch at a time. The maximum
		// batch length derives from the Entity-page codec, so a layout change cannot
		// silently leave this streaming boundary with a stale hard-coded capacity.
		static void materializePages(const NativeBulkEdgeRecordSource &source,
								 std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
								 size_t maximumRecords,
								 const NativeBulkEdgeMaterializerResolvers &resolvers,
								 const NativeBulkEdgeEntityPageBatchVisitor &visitor);
	};

} // namespace graph::storage
