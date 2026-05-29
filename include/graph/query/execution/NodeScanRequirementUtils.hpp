#pragma once

#include <algorithm>
#include <cstddef>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/execution/NodeCandidateSource.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"

namespace graph::query::execution {

	inline NodeScanRequirements relaxSatisfiedCandidateChecks(NodeScanRequirements requirements,
	                                                          const NodeCandidateSet &candidateSet) {
		if (candidateSet.activeOnly) {
			requirements.needsActiveCheck = false;
		}
		if (candidateSet.labelsSatisfied) {
			requirements.needsLabels = false;
		}
		return requirements;
	}

	inline size_t chooseColumnarNodeBatchSize(size_t remaining,
	                                         const concurrent::ThreadPool *threadPool,
	                                         size_t defaultBatchSize) {
		static constexpr size_t kWideBatchThreshold = 4096;
		static constexpr size_t kMaxColumnarBatchSize = 65536;
		// Columnar loaders amortize segment preads and property-entity scans best with
		// wide batches; cap the size to keep memory bounded and guard checks regular.
		if (remaining >= kWideBatchThreshold) {
			return std::min(remaining, kMaxColumnarBatchSize);
		}
		(void) threadPool;
		return std::min(remaining, defaultBatchSize);
	}

} // namespace graph::query::execution
