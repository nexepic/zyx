/**
 * @file SetOperator.cpp
 * @author Nexepic
 * @date 2025/12/18
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

#include "graph/query/execution/operators/SetOperator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"

namespace graph::query::execution::operators {

void SetOperator::open() {
	if (child_)
		child_->open();
}

std::optional<RecordBatch> SetOperator::next() {
	auto batchOpt = child_->next();
	if (!batchOpt)
		return std::nullopt;

	RecordBatch outputBatch;
	outputBatch.reserve(batchOpt->size());

	for (auto record: *batchOpt) {
		for (const auto &item: items_) {

			// Skip label-only items (no expression evaluation needed)
			if (item.type == SetActionType::PROPERTY && item.expression) {
				// Evaluate the expression to get the value
				PropertyValue value = graph::query::expressions::ExpressionEvaluationHelper::evaluate(
				    item.expression.get(), record);

				// --- Node Logic ---
				if (auto nodeOpt = record.getNode(item.variable)) {
					Node node = *nodeOpt;
					int64_t id = node.getId();

					// Property Update (Read-Modify-Write)
					auto props = dm_->getNodeProperties(id);
					props[item.key] = value;
					dm_->addNodeProperties(id, props);
					node.setProperties(std::move(props));

					record.setNode(item.variable, node);
				}
				// --- Edge Logic ---
				else if (auto edgeOpt = record.getEdge(item.variable)) {
					Edge edge = *edgeOpt;
					int64_t id = edge.getId();

					auto props = dm_->getEdgeProperties(id);
					props[item.key] = value;
					dm_->addEdgeProperties(id, props);
					edge.setProperties(std::move(props));
					record.setEdge(item.variable, edge);
				}
			} else if (item.type == SetActionType::LABEL) {
				// --- Label Update ---
				if (auto nodeOpt = record.getNode(item.variable)) {
					Node node = *nodeOpt;
					[[maybe_unused]] int64_t id = node.getId();

					// Resolve string label to ID
					int64_t newLabelId = dm_->getOrCreateLabelId(item.key);
					node.setLabelId(newLabelId);
					dm_->updateNode(node); // Persist label ID change

					record.setNode(item.variable, node);
				}
			}
		}
		outputBatch.push_back(std::move(record));
	}

	return outputBatch;
}

void SetOperator::close() {
	if (child_)
		child_->close();
}

std::string SetOperator::toString() const {
	return "Set(" + std::to_string(items_.size()) + " items)";
}

} // namespace graph::query::execution::operators
