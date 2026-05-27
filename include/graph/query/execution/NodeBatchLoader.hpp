#pragma once

#include <memory>
#include <vector>

#include "graph/concurrent/ThreadPool.hpp"
#include "graph/core/Node.hpp"
#include "graph/query/execution/NodeColumnBatch.hpp"
#include "graph/query/execution/NodeScanRequirements.hpp"
#include "graph/query/execution/ScanConfigs.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution {

	class NodeBatchLoader {
	public:
		explicit NodeBatchLoader(std::shared_ptr<storage::DataManager> dm,
		                         concurrent::ThreadPool *threadPool = nullptr);

		[[nodiscard]] NodeColumnBatch load(const std::vector<int64_t> &candidateIds,
		                                  size_t begin,
		                                  size_t end,
		                                  const NodeScanConfig &config,
		                                  const NodeScanRequirements &requirements) const;

	private:
		[[nodiscard]] bool matchesLabels(const Node &node, const NodeScanConfig &config) const;

		std::shared_ptr<storage::DataManager> dm_;
		concurrent::ThreadPool *threadPool_ = nullptr;
	};

} // namespace graph::query::execution
