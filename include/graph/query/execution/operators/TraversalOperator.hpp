/**
 * @file TraversalOperator.hpp
 * @author Nexepic
 * @date 2025/12/11
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

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution::operators {

	class TraversalOperator : public PhysicalOperator {
	public:
		/**
		 * @brief Constructs a traversal operator.
		 *
		 * @param dm DataManager for retrieving edges and nodes.
		 * @param child Upstream operator providing source nodes.
		 * @param sourceVar Variable name of the start node (already in record).
		 * @param edgeVar Variable name for the edge to be found.
		 * @param targetVar Variable name for the target node to be found.
		 * @param edgeLabel Filter by edge label (empty means all labels).
		 * @param direction "out", "in", or "both".
		 */
		TraversalOperator(std::shared_ptr<storage::DataManager> dm, std::unique_ptr<PhysicalOperator> child,
						  std::string sourceVar, std::string edgeVar, std::string targetVar, std::string edgeLabel,
						  std::string direction) :
			dm_(std::move(dm)), child_(std::move(child)), sourceVar_(std::move(sourceVar)),
			edgeVar_(std::move(edgeVar)), targetVar_(std::move(targetVar)), edgeLabel_(std::move(edgeLabel)),
			direction_(std::move(direction)) {}

		void open() override {
			if (child_) child_->open();

			// Resolve Edge Label ID (Optimization)
			edgeLabelId_ = 0;
			if (!edgeLabel_.empty()) {
				// Use getOrCreate or resolve. Since we are filtering, getOrCreate is fine.
				// If it creates a new ID, it won't match existing edges anyway.
				edgeLabelId_ = dm_->getOrCreateLabelId(edgeLabel_);
			}
		}

		std::optional<RecordBatch> next() override {
			auto batchOpt = child_->next();
			if (!batchOpt) return std::nullopt;

			RecordBatch &inputBatch = *batchOpt;
			RecordBatch outputBatch;
			outputBatch.reserve(inputBatch.size() * 2);

			for (const auto &record: inputBatch) {
				auto sourceNodeOpt = record.getNode(sourceVar_);
				if (!sourceNodeOpt) continue;
				int64_t sourceId = sourceNodeOpt->getId();

				// Low-level traversal
				std::vector<Edge> edges = dm_->findEdgesByNode(sourceId, direction_);

				for (auto &edge: edges) {
					// 4. Filter by Edge Label (Using ID comparison)
					if (edgeLabelId_ != 0) {
						if (edge.getLabelId() != edgeLabelId_) {
							continue;
						}
					}

					// 5. Hydrate Edge Properties
					auto edgeProps = dm_->getEdgeProperties(edge.getId());
					edge.setProperties(std::move(edgeProps));

					// 6. Resolve Target Node
					int64_t targetNodeId =
							(edge.getSourceNodeId() == sourceId) ? edge.getTargetNodeId() : edge.getSourceNodeId();

					Node targetNode = dm_->getNode(targetNodeId);
					if (!targetNode.isActive()) continue;

					// 7. Hydrate Target Node Properties
					auto nodeProps = dm_->getNodeProperties(targetNodeId);
					targetNode.setProperties(std::move(nodeProps));

					// 8. Create Extended Record
					Record newRecord = record;
					newRecord.setEdge(edgeVar_, edge);
					newRecord.setNode(targetVar_, targetNode);

					outputBatch.push_back(std::move(newRecord));
				}
			}
			return outputBatch;
		}

		void close() override { if (child_) child_->close(); }

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto vars = child_->getOutputVariables();
			vars.push_back(edgeVar_);
			vars.push_back(targetVar_);
			return vars;
		}

		[[nodiscard]] std::string toString() const override {
			return "Traversal(" + sourceVar_ + " - [" + edgeVar_ + ":" + edgeLabel_ + "] -> " + targetVar_ + ")";
		}

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (child_) return {child_.get()};
			return {};
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::unique_ptr<PhysicalOperator> child_;

		std::string sourceVar_;
		std::string edgeVar_;
		std::string targetVar_;
		std::string edgeLabel_;
		std::string direction_;

		int64_t edgeLabelId_ = 0; // Cached ID
	};
}