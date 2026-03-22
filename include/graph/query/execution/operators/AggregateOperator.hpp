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
#include "AggregateAccumulator.hpp"

namespace graph::storage { class DataManager; }

namespace graph::query::execution::operators {

/**
 * @struct AggregateItem
 * @brief Defines a single aggregation in an aggregate query.
 */
struct AggregateItem {
	AggregateFunctionType functionType;
	std::shared_ptr<graph::query::expressions::Expression> expression; // Expression to aggregate
	std::string alias; // Output variable name
	bool distinct = false; // DISTINCT modifier (e.g., count(DISTINCT expr))

	AggregateItem(AggregateFunctionType type,
	              std::shared_ptr<graph::query::expressions::Expression> expr,
	              std::string alias,
	              bool distinct = false)
		: functionType(type), expression(std::move(expr)), alias(std::move(alias)), distinct(distinct) {}
};

struct GroupByItem {
	std::shared_ptr<graph::query::expressions::Expression> expression;
	std::string alias;

	GroupByItem(std::shared_ptr<graph::query::expressions::Expression> expr, std::string alias)
		: expression(std::move(expr)), alias(std::move(alias)) {}
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
	                 std::vector<GroupByItem> groupByItems = {},
	                 storage::DataManager *dataManager = nullptr)
		: child_(std::move(child)),
		  aggregates_(std::move(aggregates)),
		  groupByItems_(std::move(groupByItems)),
		  dataManager_(dataManager) {}

	void open() override;
	std::optional<RecordBatch> next() override;
	void close() override;

	[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
		std::vector<std::string> vars;
		for (const auto& item : groupByItems_) {
			vars.push_back(item.alias);
		}
		for (const auto& agg : aggregates_) {
			vars.push_back(agg.alias);
		}
		return vars;
	}

	[[nodiscard]] std::string toString() const override;

	[[nodiscard]] std::vector<const PhysicalOperator*> getChildren() const override {
		return {child_.get()};
	}

private:
	struct GroupData {
		std::vector<std::unique_ptr<AggregateAccumulator>> accumulators;
		std::vector<PropertyValue> keyValues; // Actual values for group-by keys
	};

	std::unique_ptr<PhysicalOperator> child_;
	std::vector<AggregateItem> aggregates_;
	std::vector<GroupByItem> groupByItems_;
	storage::DataManager *dataManager_ = nullptr;
	std::vector<std::unique_ptr<AggregateAccumulator>> accumulators_;
	std::unordered_map<std::string, GroupData> groups_;
	bool emitted_ = false;

	void updateAccumulators(const Record& record,
	                       std::vector<std::unique_ptr<AggregateAccumulator>>& accums);
	void updateAccumulators(const Record& record);
	std::string computeGroupKey(const Record& record);
	std::vector<PropertyValue> evaluateGroupKeyValues(const Record& record);
};

} // namespace graph::query::execution::operators
