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
	aggregateReaders_.clear();
	aggregateReaders_.reserve(aggregates_.size());
	for (const auto& agg : aggregates_) {
		accumulators_.push_back(createAccumulator(agg.functionType, agg.distinct, agg.percentileArg));
		aggregateReaders_.push_back(ExpressionValueReader::compile(agg.expression));
	}

	groupByReaders_.clear();
	groupByReaders_.reserve(groupByItems_.size());
	for (const auto& item : groupByItems_) {
		groupByReaders_.push_back(ExpressionValueReader::compile(item.expression));
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
				auto groupKey = evaluateGroupKey(record);
				auto [it, inserted] = groups_.try_emplace(std::move(groupKey));
				if (inserted) {
					// First time seeing this group - create accumulators and store key values
					for (const auto& agg : aggregates_) {
						it->second.accumulators.push_back(
								createAccumulator(agg.functionType, agg.distinct, agg.percentileArg));
					}
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
				if (i < groupKey.values.size()) {
					outputRecord.setValue(groupByItems_[i].alias, groupKey.values[i]);
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
	aggregateReaders_.clear();
	groupByReaders_.clear();
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
			case AggregateFunctionType::AGG_STDEV: s += "stDev("; break;
			case AggregateFunctionType::AGG_STDEVP: s += "stDevP("; break;
			case AggregateFunctionType::AGG_PERCENTILE_DISC: s += "percentileDisc("; break;
			case AggregateFunctionType::AGG_PERCENTILE_CONT: s += "percentileCont("; break;
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

const std::unordered_map<std::string, PropertyValue> *AggregateOperator::parameters() const {
	if (queryContext_ && !queryContext_->parameters.empty()) {
		return &queryContext_->parameters;
	}
	return nullptr;
}

PropertyValue AggregateOperator::evaluateAggregateValue(size_t index, const Record& record) const {
	if (index < aggregateReaders_.size()) {
		return aggregateReaders_[index].evaluate(record, dataManager_, parameters());
	}
	const auto &agg = aggregates_[index];
	return graph::query::expressions::ExpressionEvaluationHelper::evaluate(
		agg.expression.get(), record, dataManager_, parameters());
}

void AggregateOperator::updateAccumulators(const Record& record,
                                           std::vector<std::unique_ptr<AggregateAccumulator>>& accums) {
	for (size_t i = 0; i < aggregates_.size(); ++i) {
		const auto& agg = aggregates_[i];

		if (agg.functionType == AggregateFunctionType::AGG_COUNT) {
			if (!agg.expression) {
				accums[i]->update(PropertyValue(static_cast<int64_t>(1)));
			} else {
				PropertyValue value = evaluateAggregateValue(i, record);
				accums[i]->update(value);
			}
		} else {
			if (agg.expression) {
				PropertyValue value = evaluateAggregateValue(i, record);
				accums[i]->update(value);
			}
		}
	}
}

void AggregateOperator::updateAccumulators(const Record& record) {
	updateAccumulators(record, accumulators_);
}

AggregateOperator::GroupKey AggregateOperator::evaluateGroupKey(const Record& record) {
	GroupKey key;
	key.values.reserve(groupByItems_.size());
	key.typedValues.reserve(groupByItems_.size());
	key.hash = groupByItems_.size();
	for (size_t i = 0; i < groupByItems_.size(); ++i) {
		const auto& item = groupByItems_[i];
		if (item.expression) {
			PropertyValue value = i < groupByReaders_.size()
				? groupByReaders_[i].evaluate(record, dataManager_, parameters())
				: graph::query::expressions::ExpressionEvaluationHelper::evaluate(
					item.expression.get(), record, dataManager_, parameters());
			auto typedValue = TypedEqualityKey::from(value);
			key.hash ^= typedValue.hash() + 0x9e3779b9 + (key.hash << 6) + (key.hash >> 2);
			key.typedValues.push_back(std::move(typedValue));
			key.values.push_back(std::move(value));
		} else {
			PropertyValue value;
			auto typedValue = TypedEqualityKey::from(value);
			key.hash ^= typedValue.hash() + 0x9e3779b9 + (key.hash << 6) + (key.hash >> 2);
			key.typedValues.push_back(std::move(typedValue));
			key.values.push_back(std::move(value));
		}
	}
	return key;
}

} // namespace graph::query::execution::operators
