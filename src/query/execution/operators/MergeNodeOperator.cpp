/**
 * @file MergeNodeOperator.cpp
 * @author ZYX Graph Database Team
 * @date 2025
 *
 * @copyright Copyright (c) 2025 ZYX Graph Database
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

#include "graph/query/execution/operators/MergeNodeOperator.hpp"
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"

namespace graph::query::execution::operators {

void MergeNodeOperator::open() {
	executed_ = false;
	if (child_) child_->open();

	// Resolve Label ID
	targetLabelId_ = 0;
	if (!label_.empty()) {
		targetLabelId_ = dm_->getOrCreateLabelId(label_);
	}
}

std::optional<RecordBatch> MergeNodeOperator::next() {
	if (child_) {
		auto batchOpt = child_->next();
		if (!batchOpt) return std::nullopt;

		RecordBatch outputBatch;
		outputBatch.reserve(batchOpt->size());

		for (const auto &record: *batchOpt) {
			Record newRecord = record;
			processMerge(newRecord);
			outputBatch.push_back(std::move(newRecord));
		}
		return outputBatch;
	}

	if (executed_) return std::nullopt;

	Record record;
	processMerge(record);

	RecordBatch batch;
	batch.push_back(std::move(record));
	executed_ = true;
	return batch;
}

void MergeNodeOperator::close() {
	if (child_) child_->close();
}

std::string MergeNodeOperator::toString() const {
	return "MergeNode(" + variable_ + ":" + label_ + ")";
}

void MergeNodeOperator::processMerge(Record &record) {
	if (record.getNode(variable_)) return;

	// 2. Try to find existing node
	std::vector<int64_t> candidates;
	bool indexUsed = false;

	// Try to find a property to index scan
	for (const auto &[key, val]: matchProps_) {
		if (im_->hasPropertyIndex("node", key)) {
			candidates = im_->findNodeIdsByProperty(key, val);
			indexUsed = true;
			break;
		}
	}

	// If no property index, try label index
	if (!indexUsed && im_->hasLabelIndex("node") && !label_.empty()) {
		candidates = im_->findNodeIdsByLabel(label_);
		indexUsed = true;
	}

	if (!indexUsed) {
		int64_t maxId = dm_->getIdAllocator()->getCurrentMaxNodeId();
		for (int64_t i = 1; i <= maxId; ++i) candidates.push_back(i);
	}

	// Filter candidates
	int64_t foundId = 0;
	for (int64_t id: candidates) {
		Node n = dm_->getNode(id);
		if (!n.isActive()) continue;

		// Label Check using ID
		if (targetLabelId_ != 0) {
			if (n.getLabelId() != targetLabelId_) continue;
		}

		// Hydrate & Check Properties
		auto props = dm_->getNodeProperties(id);
		bool match = true;
		for (const auto &[k, v]: matchProps_) {
			auto it = props.find(k);
			if (it == props.end() || it->second != v) {
				match = false;
				break;
			}
		}

		if (match) {
			foundId = id;
			n.setProperties(std::move(props));
			record.setNode(variable_, n);
			break;
		}
	}

	// 3. Action
	if (foundId != 0) {
		// === MATCHED ===
		applyUpdates(foundId, onMatchItems_, record);
	} else {
		// === NOT MATCHED -> CREATE ===
		// Use ID Constructor
		Node newNode(0, targetLabelId_);

		dm_->addNode(newNode);

		if (!matchProps_.empty()) {
			dm_->addNodeProperties(newNode.getId(), matchProps_);
		}

		newNode.setProperties(matchProps_);
		record.setNode(variable_, newNode);

		applyUpdates(newNode.getId(), onCreateItems_, record);
	}
}

void MergeNodeOperator::applyUpdates(int64_t nodeId, const std::vector<SetItem> &items, Record &record) {
	if (items.empty()) return;

	auto props = dm_->getNodeProperties(nodeId);
	bool changed = false;

	for (const auto &item: items) {
		if (item.variable == variable_) {
			if (item.type == SetActionType::PROPERTY && item.expression) {
				// Evaluate the expression to get the value
				graph::query::expressions::EvaluationContext context(record);
				graph::query::expressions::ExpressionEvaluator evaluator(context);
				PropertyValue value = evaluator.evaluate(item.expression.get());

				props[item.key] = value;
				changed = true;
			}
		}
	}

	if (changed) {
		dm_->addNodeProperties(nodeId, props);
		if (auto n = record.getNode(variable_)) {
			Node updatedNode = *n;
			updatedNode.setProperties(std::move(props));
			record.setNode(variable_, updatedNode);
		}
	}
}

} // namespace graph::query::execution::operators
