/**
 * @file AlgoShortestPathOperator.hpp
 * @author Nexepic
 * @date 2025/12/17
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include "graph/query/algorithm/GraphAlgorithm.hpp"

namespace graph::query::execution::operators {

	class AlgoShortestPathOperator : public PhysicalOperator {
	public:
		AlgoShortestPathOperator(std::shared_ptr<storage::DataManager> dm, int64_t startId, int64_t endId,
								 std::string direction = "both") :
			startId_(startId), endId_(endId), direction_(std::move(direction)) {
			// Initialize algorithm instance
			algo_ = std::make_unique<algorithm::GraphAlgorithm>(dm);
		}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;

			// 1. Execute Algorithm
			std::vector<Node> path = algo_->findShortestPath(startId_, endId_, direction_);

			if (path.empty())
				return std::nullopt;

			// 2. Format Output
			// We output each node in the path as a separate record (unwind style)
			// or one record describing the path.
			// For simple verification, let's output the sequence of nodes.
			RecordBatch batch;
			int step = 0;

			for (const auto &node: path) {
				Record r;
				r.setNode("node", node);
				r.setValue("step", PropertyValue((int64_t) step++));
				r.setValue("path_length", PropertyValue((int64_t) path.size()));
				batch.push_back(std::move(r));
			}

			executed_ = true;
			return batch;
		}

		void close() override {}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"step", "node", "path_length"};
		}

		[[nodiscard]] std::string toString() const override {
			return "AlgoShortestPath(" + std::to_string(startId_) + " -> " + std::to_string(endId_) + ")";
		}

	private:
		std::unique_ptr<algorithm::GraphAlgorithm> algo_;
		int64_t startId_;
		int64_t endId_;
		std::string direction_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
