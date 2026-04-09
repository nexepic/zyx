/**
 * @file TraversalOperator.cpp
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

#include "graph/query/execution/operators/TraversalOperator.hpp"

namespace graph::query::execution::operators {

void TraversalOperator::open() {
	if (child_) child_->open();

	// Resolve Edge Type ID (Optimization)
	edgeTypeId_ = 0;
	if (!edgeType_.empty()) {
		// Use getOrCreate or resolve. Since we are filtering, getOrCreate is fine.
		// If it creates a new ID, it won't match existing edges anyway.
		edgeTypeId_ = dm_->getOrCreateTokenId(edgeType_);
	}
}

std::optional<RecordBatch> TraversalOperator::next() {
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
			// 4. Filter by Edge Type (Using ID comparison)
			if (edgeTypeId_ != 0) {
				if (edge.getTypeId() != edgeTypeId_) {
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

void TraversalOperator::close() {
	if (child_) child_->close();
}

std::string TraversalOperator::toString() const {
	return "Traversal(" + sourceVar_ + " - [" + edgeVar_ + ":" + edgeType_ + "] -> " + targetVar_ + ")";
}

} // namespace graph::query::execution::operators
