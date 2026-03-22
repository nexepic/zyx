/**
 * @file ProjectOperator.cpp
 * @author Nexepic
 * @date 2025/12/12
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

#include "graph/query/execution/operators/ProjectOperator.hpp"
#include <sstream>
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"

namespace graph::query::execution::operators {

void ProjectOperator::open() {
	if (child_)
		child_->open();
}

std::optional<RecordBatch> ProjectOperator::next() {
	using namespace graph::query::expressions;

	auto batchOpt = child_->next();
	if (!batchOpt)
		return std::nullopt;

	RecordBatch &inputBatch = *batchOpt;
	RecordBatch outputBatch;
	outputBatch.reserve(inputBatch.size());

	for (const auto &record: inputBatch) {
		Record newRecord;

		for (const auto &item: items_) {
			std::string key = item.alias;

			// Check if this is a simple variable reference (no property access)
			if (item.expression && item.expression->getExpressionType() == ExpressionType::PROPERTY_ACCESS) {
				auto* varRef = static_cast<VariableReferenceExpression*>(item.expression.get());
				if (!varRef->hasProperty()) {
					// Simple variable reference - try to copy node/edge directly
					const std::string& varName = varRef->getVariableName();

					// Check if it's a node
					if (auto node = record.getNode(varName)) {
						newRecord.setNode(key, *node);
						continue;
					}

					// Check if it's an edge
					if (auto edge = record.getEdge(varName)) {
						newRecord.setEdge(key, *edge);
						continue;
					}

					// Check if it's a value
					if (auto value = record.getValue(varName)) {
						newRecord.setValue(key, *value);
						continue;
					}
				}
			}

			// Evaluate the expression to get the value
			PropertyValue value;
			if (item.expression) {
				try {
					value = ExpressionEvaluationHelper::evaluate(
					    item.expression.get(), record, dataManager_);
				} catch (const UndefinedVariableException&) {
					// Undefined variable → NULL (Cypher semantics)
					value = PropertyValue();
				}
			} else {
				// Fallback to NULL for expressions not yet migrated
				value = PropertyValue();
			}

			// Store the value in the new record
			newRecord.setValue(key, value);
		}

		// DISTINCT: Check if we've seen this record before
		if (distinct_) {
			auto fp = buildFingerprint(newRecord);
			if (seenRecords_.insert(std::move(fp)).second) {
				outputBatch.push_back(std::move(newRecord));
			}
			// If duplicate, skip adding to outputBatch
		} else {
			outputBatch.push_back(std::move(newRecord));
		}
	}

	return outputBatch;
}

void ProjectOperator::close() {
	if (child_)
		child_->close();
}

std::string ProjectOperator::toString() const {
	std::ostringstream oss;
	oss << "Project(";
	if (distinct_) oss << "DISTINCT ";
	for (size_t i = 0; i < items_.size(); ++i) {
		if (items_[i].expression) {
			oss << items_[i].expression->toString();
		}
		if (items_[i].alias != (items_[i].expression ? items_[i].expression->toString() : "")) {
			oss << " AS " << items_[i].alias;
		}
		if (i < items_.size() - 1)
			oss << ", ";
	}
	oss << ")";
	return oss.str();
}

std::vector<const PhysicalOperator *> ProjectOperator::getChildren() const {
	if (child_)
		return {child_.get()};
	return {};
}

ProjectOperator::RecordFingerprint ProjectOperator::buildFingerprint(const Record &record) {
	RecordFingerprint fp;
	fp.values.reserve(items_.size());
	for (const auto &item: items_) {
		if (auto val = record.getValue(item.alias)) {
			fp.values.push_back(*val);
		} else {
			fp.values.emplace_back(); // NULL
		}
	}
	return fp;
}

} // namespace graph::query::execution::operators
