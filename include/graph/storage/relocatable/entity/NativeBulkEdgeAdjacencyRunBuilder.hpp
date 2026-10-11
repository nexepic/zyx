/**
 * @file NativeBulkEdgeAdjacencyRunBuilder.hpp
 * @brief Bounded sorted adjacency runs from sealed native Edge scratch.
 *
 * Copyright (c) 2026 Nexepic
 * License: Apache-2.0
 **/

#pragma once

#include <cstddef>
#include <functional>
#include <span>

#include "graph/storage/relocatable/adjacency/AdjacencyKeyCodec.hpp"
#include "graph/storage/relocatable/entity/NativeBulkEdgeRunStore.hpp"

namespace graph::storage {

	using NativeBulkEdgeAdjacencyRunVisitor = std::function<void(NativeBulkRunFragment &&)>;

	class NativeBulkEdgeAdjacencyRunBuilder final {
	public:
		// Rewinds a sealed Edge stream once for one direction and emits sorted
		// bounded runs. Edge ID append order is deliberately not treated as
		// adjacency order: each run is sorted by the canonical encoded key, and the
		// caller merges the resulting run set with bounded fan-in. `maximumFragments`
		// is independent from the in-memory sort budget: it bounds durable candidate
		// work even when one sorted run must be split at a segment boundary.
		static void buildRuns(const NativeBulkEdgeRecordSource &source,
						  std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
						  size_t maximumRecords, size_t maximumRunBytes, size_t maximumFragments,
						  GlobalAdjacencyDirection direction,
						  const NativeBulkEdgeAdjacencyRunVisitor &visitor);
		// Rewinds the sealed source once and emits two independent, bounded sorted
		// streams. The caller owns distinct candidate ranges for each direction.
		static void buildRunsPair(const NativeBulkEdgeRecordSource &source,
							  std::span<const NativeGraphLogicalIdLeaseRange> edgeIdRanges,
							  size_t maximumRecords, size_t maximumRunBytes, size_t maximumOutgoingFragments,
							  size_t maximumIncomingFragments, const NativeBulkEdgeAdjacencyRunVisitor &outgoingVisitor,
							  const NativeBulkEdgeAdjacencyRunVisitor &incomingVisitor);
	};

} // namespace graph::storage
