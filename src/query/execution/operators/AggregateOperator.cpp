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
#include "graph/query/QueryContext.hpp"

namespace graph::query::execution::operators {

void AggregateOperator::open() {
	if (child_) {
		child_->open();
	}
	accumulators_.clear();
	groups_.clear();
	emitted_ = false;

	// Initialize accumulators for each aggregate (with distinct flag)
	for (const auto& agg : aggregates_) {
		accumulators_.push_back(createAccumulator(agg.functionType, agg.distinct));
	}
}

std::optional<RecordBatch> AggregateOperator::next() {
	if (emitted_) {
		return std::nullopt; // Only emit one batch
	}

	// Consume all input records and accumulate
	while (auto batchOpt = child_->next()) {
		if (queryContext_) queryContext_->checkGuard();
		for (const auto& record : *batchOpt) {
			if (groupByItems_.empty()) {
				// Global aggregation - update all accumulators
				updateAccumulators(record);
			} else {
				// Grouped aggregation
				std::string groupKey = computeGroupKey(record);
				auto it = groups_.find(groupKey);
				if (it == groups_.end()) {
					// First time seeing this group - create accumulators and store key values
					GroupData data;
					for (const auto& agg : aggregates_) {
						data.accumulators.push_back(createAccumulator(agg.functionType, agg.distinct));
					}
					data.keyValues = evaluateGroupKeyValues(record);
					it = groups_.emplace(groupKey, std::move(data)).first;
				}
				updateAccumulators(record, it->second.accumulators);
			}
		}
	}

	// Produce output records
	RecordBatch outputBatch;

	if (groupByItems_.empty()) {
		// Global aggregation - single output record
		Record outputRecord;
		for (size_t i = 0; i < aggregates_.size(); ++i) {
			outputRecord.setValue(aggregates_[i].alias, accumulators_[i]->getResult());
		}
		outputBatch.push_back(std::move(outputRecord));
	} else {
		// Grouped aggregation - one output record per group
		for (auto& [groupKey, groupData] : groups_) {
			Record outputRecord;
			// Add group-by key values to the output record
			for (size_t i = 0; i < groupByItems_.size(); ++i) {
				if (i < groupData.keyValues.size()) {
					outputRecord.setValue(groupByItems_[i].alias, groupData.keyValues[i]);
				}
			}
			// Add aggregate results
			for (size_t i = 0; i < aggregates_.size(); ++i) {
				outputRecord.setValue(aggregates_[i].alias, groupData.accumulators[i]->getResult());
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
			if (!agg.expression) {
				accums[i]->update(PropertyValue(static_cast<int64_t>(1)));
			} else {
				const std::unordered_map<std::string, PropertyValue> *params = nullptr;
				if (queryContext_ && !queryContext_->parameters.empty())
					params = &queryContext_->parameters;
				PropertyValue value = graph::query::expressions::ExpressionEvaluationHelper::evaluate(
				    agg.expression.get(), record, dataManager_, params);
				accums[i]->update(value);
			}
		} else {
			if (agg.expression) {
				const std::unordered_map<std::string, PropertyValue> *params = nullptr;
				if (queryContext_ && !queryContext_->parameters.empty())
					params = &queryContext_->parameters;
				PropertyValue value = graph::query::expressions::ExpressionEvaluationHelper::evaluate(
				    agg.expression.get(), record, dataManager_, params);
				accums[i]->update(value);
			}
		}
	}
}

void AggregateOperator::updateAccumulators(const Record& record) {
	updateAccumulators(record, accumulators_);
}

std::string AggregateOperator::computeGroupKey(const Record& record) {
	std::string key;
	for (const auto& item : groupByItems_) {
		if (item.expression) {
			const std::unordered_map<std::string, PropertyValue> *params = nullptr;
			if (queryContext_ && !queryContext_->parameters.empty())
				params = &queryContext_->parameters;
			PropertyValue val = graph::query::expressions::ExpressionEvaluationHelper::evaluate(
			    item.expression.get(), record, dataManager_, params);
			key += val.toString() + "|";
		}
	}
	return key;
}

std::vector<PropertyValue> AggregateOperator::evaluateGroupKeyValues(const Record& record) {
	std::vector<PropertyValue> values;
	values.reserve(groupByItems_.size());
	for (const auto& item : groupByItems_) {
		if (item.expression) {
			const std::unordered_map<std::string, PropertyValue> *params = nullptr;
			if (queryContext_ && !queryContext_->parameters.empty())
				params = &queryContext_->parameters;
			values.push_back(graph::query::expressions::ExpressionEvaluationHelper::evaluate(
			    item.expression.get(), record, nullptr, params));
		} else {
			values.emplace_back(); // NULL
		}
	}
	return values;
}

} // namespace graph::query::execution::operators
