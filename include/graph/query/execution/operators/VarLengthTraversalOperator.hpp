/**
 * @file VarLengthTraversalOperator.hpp
 * @author Nexepic
 * @date 2025/12/22
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

	class VarLengthTraversalOperator : public PhysicalOperator {
	public:
		VarLengthTraversalOperator(std::shared_ptr<storage::DataManager> dm, std::unique_ptr<PhysicalOperator> child,
								   std::string sourceVar, std::string targetVar, std::string edgeLabel, int minHops,
								   int maxHops, std::string direction) :
			child_(std::move(child)), sourceVar_(std::move(sourceVar)), targetVar_(std::move(targetVar)),
			edgeLabel_(std::move(edgeLabel)), direction_(std::move(direction)), minHops_(minHops), maxHops_(maxHops) {
			algo_ = std::make_unique<algorithm::GraphAlgorithm>(dm);
		}

		void open() override {
			if (child_)
				child_->open();
		}

		std::optional<RecordBatch> next() override {
			auto batchOpt = child_->next();
			if (!batchOpt)
				return std::nullopt;

			RecordBatch outputBatch;
			// outputBatch.reserve(batchOpt->size() * 2); // Heuristic

			for (const auto &record: *batchOpt) {
				auto sourceNode = record.getNode(sourceVar_);
				if (!sourceNode)
					continue;

				// Call Algorithm to find all reachable targets within range
				std::vector<Node> targets =
						algo_->findAllPaths(sourceNode->getId(), minHops_, maxHops_, edgeLabel_, direction_);

				for (const auto &target: targets) {
					Record newRecord = record;
					newRecord.setNode(targetVar_, target);

					// Note: Current Record implementation might not support storing a List<Edge> for the path.
					// For now, we only bind the Target Node (b).
					// If 'r' variable was requested (e.g. [r*1..5]), we can't fully support it
					// until PropertyValue supports Lists/Paths.

					outputBatch.push_back(std::move(newRecord));
				}
			}

			return outputBatch;
		}

		void close() override {
			if (child_)
				child_->close();
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto v = child_->getOutputVariables();
			v.push_back(targetVar_);
			return v;
		}

		[[nodiscard]] std::string toString() const override {
			return "VarLengthTraversal(" + sourceVar_ + " -[*" + std::to_string(minHops_) + ".." +
				   std::to_string(maxHops_) + "]-> " + targetVar_ + ")";
		}

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override { return {child_.get()}; }

	private:
		std::unique_ptr<PhysicalOperator> child_;
		std::unique_ptr<algorithm::GraphAlgorithm> algo_;
		std::string sourceVar_, targetVar_, edgeLabel_, direction_;
		int minHops_, maxHops_;
	};

} // namespace graph::query::execution::operators
