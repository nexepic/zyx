/**
 * @file AggregateOperator.hpp
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

#pragma once

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"
#include "graph/query/expressions/EvaluationContext.hpp"
#include "graph/query/expressions/ExpressionEvaluator.hpp"
#include "AggregateAccumulator.hpp"

namespace graph::query::execution::operators {

/**
 * @struct AggregateItem
 * @brief Defines a single aggregation in an aggregate query.
 */
struct AggregateItem {
	AggregateFunctionType functionType;
	std::shared_ptr<graph::query::expressions::Expression> expression; // Expression to aggregate
	std::string alias; // Output variable name

	AggregateItem(AggregateFunctionType type,
	              std::shared_ptr<graph::query::expressions::Expression> expr,
	              std::string alias)
		: functionType(type), expression(std::move(expr)), alias(std::move(alias)) {}
};

/**
 * @class AggregateOperator
 * @brief Operator for aggregate queries (COUNT, SUM, AVG, MIN, MAX, collect).
 *
 * This operator performs aggregation over all input records and produces
 * a single output record with the aggregated results.
 *
 * Example:
 *   MATCH (n) RETURN count(n) AS count, sum(n.value) AS total
 */
class AggregateOperator : public PhysicalOperator {
public:
	/**
	 * @brief Constructs an AggregateOperator.
	 * @param child Upstream operator
	 * @param aggregates List of aggregate items (function + expression + alias)
	 * @param groupByKeys Variables to group by (empty for global aggregation)
	 */
	AggregateOperator(std::unique_ptr<PhysicalOperator> child,
	                 std::vector<AggregateItem> aggregates,
	                 std::vector<std::string> groupByKeys = {})
		: child_(std::move(child)),
		  aggregates_(std::move(aggregates)),
		  groupByKeys_(std::move(groupByKeys)) {}

	void open() override {
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

	std::optional<RecordBatch> next() override {
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

	void close() override {
		if (child_) {
			child_->close();
		}
		accumulators_.clear();
		groups_.clear();
	}

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		std::vector<std::string> vars;
		for (const auto& agg : aggregates_) {
			vars.push_back(agg.alias);
		}
		return vars;
	}

	[[nodiscard]] std::string toString() const override {
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

	[[nodiscard]] std::vector<const PhysicalOperator*> getChildren() const override {
		return {child_.get()};
	}

private:
	std::unique_ptr<PhysicalOperator> child_;
	std::vector<AggregateItem> aggregates_;
	std::vector<std::string> groupByKeys_;
	std::vector<std::unique_ptr<AggregateAccumulator>> accumulators_;
	std::unordered_map<std::string, std::vector<std::unique_ptr<AggregateAccumulator>>> groups_;
	bool emitted_ = false;

	void updateAccumulators(const Record& record,
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

	void updateAccumulators(const Record& record) {
		updateAccumulators(record, accumulators_);
	}

	std::string computeGroupKey(const Record& record) {
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
};

} // namespace graph::query::execution::operators
