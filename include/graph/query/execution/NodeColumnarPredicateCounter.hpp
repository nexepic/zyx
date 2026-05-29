#pragma once

#include <cstdint>
#include <memory>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/execution/NodeCandidateSource.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/query/execution/VectorizedPredicate.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	struct NodeColumnarPredicateCountResult {
		int64_t count = 0;
		bool available = false;
	};

	class NodeColumnarPredicateCounter {
	public:
		NodeColumnarPredicateCounter(std::shared_ptr<storage::DataManager> dm,
		                             concurrent::ThreadPool *threadPool = nullptr);

		[[nodiscard]] NodeColumnarPredicateCountResult count(const std::vector<int64_t> &candidateIds,
		                                                    const NodeCandidateSet &candidateSet,
		                                                    const NodeScanConfig &config,
		                                                    const NodeScanRequirements &requirements,
		                                                    const std::vector<VectorizedPropertyPredicate> &predicates) const;

	private:
		std::shared_ptr<storage::DataManager> dm_;
		concurrent::ThreadPool *threadPool_ = nullptr;
	};

} // namespace graph::query::execution
