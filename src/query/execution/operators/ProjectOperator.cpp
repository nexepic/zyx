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
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "graph/query/QueryContext.hpp"

namespace graph::query::execution::operators {

void ProjectOperator::open() {
	if (child_)
		child_->open();
}

Record ProjectOperator::projectRecord(const Record &record) const {
	using namespace graph::query::expressions;

	Record newRecord;

	for (const auto &item: items_) {
		std::string key = item.alias;

		// Check if this is a simple variable reference (no property access)
		if (item.expression && (item.expression->getExpressionType() == ExpressionType::VARIABLE_REFERENCE ||
		                        item.expression->getExpressionType() == ExpressionType::PROPERTY_ACCESS)) {
			auto* varRef = static_cast<VariableReferenceExpression*>(item.expression.get());
			if (!varRef->hasProperty()) {
				const std::string& varName = varRef->getVariableName();

				if (auto node = record.getNode(varName)) {
					newRecord.setNode(key, *node);
					continue;
				}
				if (auto edge = record.getEdge(varName)) {
					newRecord.setEdge(key, *edge);
					continue;
				}
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
				const std::unordered_map<std::string, PropertyValue> *params = nullptr;
				if (queryContext_ && !queryContext_->parameters.empty()) {
					params = &queryContext_->parameters;
				}
				value = ExpressionEvaluationHelper::evaluate(
				    item.expression.get(), record, dataManager_, params);
			} catch (const UndefinedVariableException&) {
				value = PropertyValue();
			}
		} else {
			value = PropertyValue();
		}

		newRecord.setValue(key, value);
	}

	return newRecord;
}

std::optional<RecordBatch> ProjectOperator::next() {
	auto batchOpt = child_->next();
	if (!batchOpt)
		return std::nullopt;

	RecordBatch &inputBatch = *batchOpt;

	// Parallel path for non-DISTINCT projections on large batches
	if (!distinct_ && threadPool_ && !threadPool_->isSingleThreaded()
		&& inputBatch.size() >= PARALLEL_PROJECT_THRESHOLD) {
		RecordBatch outputBatch(inputBatch.size());
		threadPool_->parallelFor(0, inputBatch.size(), [&](size_t i) {
			outputBatch[i] = projectRecord(inputBatch[i]);
		});
		return outputBatch;
	}

	// Sequential path
	RecordBatch outputBatch;
	outputBatch.reserve(inputBatch.size());

	for (const auto &record: inputBatch) {
		Record newRecord = projectRecord(record);

		if (distinct_) {
			auto fp = buildFingerprint(newRecord);
			if (seenRecords_.insert(std::move(fp)).second) {
				outputBatch.push_back(std::move(newRecord));
			}
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
