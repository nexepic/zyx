/**
 * @file MergeEdgeOperator.cpp
 * @author ZYX Contributors
 * @date 2025
 *
 * @copyright Copyright (c) 2025
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

#include "graph/query/execution/operators/MergeEdgeOperator.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/QueryContext.hpp"
#include <stdexcept>
#include <utility>

namespace graph::query::execution::operators {

void MergeEdgeOperator::open() {
	executed_ = false;
	if (child_) child_->open();
	edgeTypeId_ = 0;
	if (!edgeType_.empty()) {
		edgeTypeId_ = dm_->getOrCreateTokenId(edgeType_);
	}
}

std::optional<RecordBatch> MergeEdgeOperator::next() {
	if (child_) {
		auto batchOpt = child_->next();
		if (!batchOpt) return std::nullopt;

		RecordBatch outputBatch;
		outputBatch.reserve(batchOpt->size());

		for (const auto &record : *batchOpt) {
			Record newRecord = record;
			processMergeEdge(newRecord);
			outputBatch.push_back(std::move(newRecord));
		}
		return outputBatch;
	}

	if (executed_) return std::nullopt;
	executed_ = true;

	Record record;
	processMergeEdge(record);

	RecordBatch batch;
	batch.push_back(std::move(record));
	return batch;
}

void MergeEdgeOperator::close() {
	if (child_) child_->close();
}

std::string MergeEdgeOperator::toString() const {
	return "MergeEdge(" + sourceVar_ + ")-[" + edgeVar_ + ":" + edgeType_ + "]->(" + targetVar_ + ")";
}

void MergeEdgeOperator::processMergeEdge(Record &record) {
	// Get source and target node IDs from the record
	auto sourceNode = record.getNode(sourceVar_);
	auto targetNode = record.getNode(targetVar_);

	if (!sourceNode || !targetNode) {
		throw std::runtime_error("MERGE edge requires source (" + sourceVar_ +
		                         ") and target (" + targetVar_ + ") nodes to be resolved");
	}

	int64_t sourceId = sourceNode->getId();
	int64_t targetId = targetNode->getId();

	// Try to find existing edge
	int64_t foundEdgeId = 0;
	auto edges = dm_->findEdgesByNode(sourceId, "both");

	for (const auto &edge : edges) {
		if (queryContext_) queryContext_->checkGuard();
		if (!edge.isActive()) continue;

		// Check direction
		bool dirMatch = false;
		if (direction_ == "out") {
			dirMatch = (edge.getSourceNodeId() == sourceId && edge.getTargetNodeId() == targetId);
		} else if (direction_ == "in") {
			dirMatch = (edge.getSourceNodeId() == targetId && edge.getTargetNodeId() == sourceId);
		} else {
			dirMatch = (edge.getSourceNodeId() == sourceId && edge.getTargetNodeId() == targetId) ||
			           (edge.getSourceNodeId() == targetId && edge.getTargetNodeId() == sourceId);
		}
		if (!dirMatch) continue;

		// Check type
		if (edgeTypeId_ != 0 && edge.getTypeId() != edgeTypeId_) continue;

		// Check properties
		auto edgeProps = dm_->getEdgeProperties(edge.getId());
		bool match = true;
		for (const auto &[k, v] : matchProps_) {
			auto it = edgeProps.find(k);
			if (it == edgeProps.end() || it->second != v) {
				match = false;
				break;
			}
		}

		if (match) {
			foundEdgeId = edge.getId();
			if (!edgeVar_.empty()) {
				Edge matchedEdge = edge;
				matchedEdge.setProperties(std::move(edgeProps));
				record.setEdge(edgeVar_, matchedEdge);
			}
			break;
		}
	}

	if (foundEdgeId != 0) {
		// MATCHED - apply ON MATCH
		applyEdgeUpdates(foundEdgeId, onMatchItems_, record);
	} else {
		// NOT MATCHED - CREATE
		int64_t realSourceId = sourceId;
		int64_t realTargetId = targetId;
		if (direction_ == "in") {
			std::swap(realSourceId, realTargetId);
		}

		Edge newEdge(0, realSourceId, realTargetId, edgeTypeId_);
		dm_->addEdge(newEdge);

		if (!matchProps_.empty()) {
			dm_->addEdgeProperties(newEdge.getId(), matchProps_);
		}

		newEdge.setProperties(matchProps_);
		if (!edgeVar_.empty()) {
			record.setEdge(edgeVar_, newEdge);
		}

		applyEdgeUpdates(newEdge.getId(), onCreateItems_, record);
	}
}

void MergeEdgeOperator::applyEdgeUpdates(int64_t edgeId, const std::vector<SetItem> &items, Record &record) {
	if (items.empty()) return;

	auto props = dm_->getEdgeProperties(edgeId);
	bool changed = false;

	for (const auto &item : items) {
		if (item.variable == edgeVar_ && item.type == SetActionType::PROPERTY && item.expression) {
			auto makeContext = [&]() {
				if (queryContext_ && !queryContext_->parameters.empty())
					return graph::query::expressions::EvaluationContext(record, queryContext_->parameters);
				return graph::query::expressions::EvaluationContext(record);
			};
			auto context = makeContext();
			graph::query::expressions::ExpressionEvaluator evaluator(context);
			PropertyValue value = evaluator.evaluate(item.expression.get());
			props[item.key] = value;
			changed = true;
		}
	}

	if (changed) {
		dm_->addEdgeProperties(edgeId, props);
		if (!edgeVar_.empty()) {
			if (auto e = record.getEdge(edgeVar_)) {
				Edge updatedEdge = *e;
				updatedEdge.setProperties(std::move(props));
				record.setEdge(edgeVar_, updatedEdge);
			}
		}
	}
}

} // namespace graph::query::execution::operators
