/**
 * @file AggregateOperator.cpp
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

#include "graph/query/execution/operators/AggregateOperator.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"

namespace graph::query::execution::operators {

void AggregateOperator::open() {
	if (child_) {
		child_->open();
	}
	accumulators_.clear();
	groups_.clear();
	emitted_ = false;

	// Initialize accumulators for each aggregate
	for (const auto& agg : aggregates_) {
		accumulators_.push_back(createAccumulator(agg.functionType));
	}
}

std::optional<RecordBatch> AggregateOperator::next() {
	if (emitted_) {
		return std::nullopt; // Only emit one batch
	}

	// Consume all input records and accumulate
	while (auto batchOpt = child_->next()) {
		for (const auto& record : *batchOpt) {
			if (groupByKeys_.empty()) {
				// Global aggregation - update all accumulators
				updateAccumulators(record);
			} else {
				// Grouped aggregation
				std::string groupKey = computeGroupKey(record);
				auto& groupAccumulators = groups_[groupKey];
				if (groupAccumulators.empty()) {
					// First time seeing this group
					for (const auto& agg : aggregates_) {
						groupAccumulators.push_back(createAccumulator(agg.functionType));
					}
				}
				updateAccumulators(record, groupAccumulators);
			}
		}
	}

	// Produce output records
	RecordBatch outputBatch;

	if (groupByKeys_.empty()) {
		// Global aggregation - single output record
		Record outputRecord;
		for (size_t i = 0; i < aggregates_.size(); ++i) {
			outputRecord.setValue(aggregates_[i].alias, accumulators_[i]->getResult());
		}
		outputBatch.push_back(std::move(outputRecord));
	} else {
		// Grouped aggregation - one output record per group
		for (auto& [groupKey, groupAccumulators] : groups_) {
			Record outputRecord;
			// Add group by keys
			// TODO: Parse group key and extract values
			// For now, we'd need to store the original record or group key values
			for (size_t i = 0; i < aggregates_.size(); ++i) {
				outputRecord.setValue(aggregates_[i].alias, groupAccumulators[i]->getResult());
			}
			outputBatch.push_back(std::move(outputRecord));
		}
	}

	emitted_ = true;
	return outputBatch;
}

void AggregateOperator::close() {
	if (child_) {
		child_->close();
	}
	accumulators_.clear();
	groups_.clear();
}

std::string AggregateOperator::toString() const {
	std::string s = "Aggregate(";
	for (size_t i = 0; i < aggregates_.size(); ++i) {
		const auto& agg = aggregates_[i];
		switch (agg.functionType) {
			case AggregateFunctionType::AGG_COUNT: s += "count("; break;
			case AggregateFunctionType::AGG_SUM: s += "sum("; break;
			case AggregateFunctionType::AGG_AVG: s += "avg("; break;
			case AggregateFunctionType::AGG_MIN: s += "min("; break;
			case AggregateFunctionType::AGG_MAX: s += "max("; break;
			case AggregateFunctionType::AGG_COLLECT: s += "collect("; break;
		}
		if (agg.expression) {
			s += agg.expression->toString();
		}
		s += ")";
		if (!agg.alias.empty() && agg.expression && agg.alias != agg.expression->toString()) {
			s += " AS " + agg.alias;
		}
		if (i < aggregates_.size() - 1) {
			s += ", ";
		}
	}
	s += ")";
	return s;
}

void AggregateOperator::updateAccumulators(const Record& record,
                                           std::vector<std::unique_ptr<AggregateAccumulator>>& accums) {
	for (size_t i = 0; i < aggregates_.size(); ++i) {
		const auto& agg = aggregates_[i];

		if (agg.functionType == AggregateFunctionType::AGG_COUNT) {
			// COUNT(*) - count all records
			if (!agg.expression) {
				accums[i]->update(PropertyValue(static_cast<int64_t>(1)));
			} else {
				// COUNT(expr) - evaluate expression and count non-NULL results
				PropertyValue value = graph::query::expressions::ExpressionEvaluationHelper::evaluate(
				    agg.expression.get(), record);
				accums[i]->update(value);
			}
		} else {
			// Other aggregates require an expression
			if (agg.expression) {
				PropertyValue value = graph::query::expressions::ExpressionEvaluationHelper::evaluate(
				    agg.expression.get(), record);
				accums[i]->update(value);
			}
		}
	}
}

void AggregateOperator::updateAccumulators(const Record& record) {
	updateAccumulators(record, accumulators_);
}

std::string AggregateOperator::computeGroupKey(const Record& record) {
	// Simple implementation: concatenate group by variable values
	// TODO: Handle NULL values and complex expressions
	std::string key;
	for (const auto& var : groupByKeys_) {
		if (auto node = record.getNode(var)) {
			key += "N" + std::to_string(node->getId()) + "|";
		} else if (auto edge = record.getEdge(var)) {
			key += "E" + std::to_string(edge->getId()) + "|";
		} else if (auto val = record.getValue(var)) {
			key += val->toString() + "|";
		}
	}
	return key;
}

} // namespace graph::query::execution::operators
