/**
 * @file QueryExecutor.cpp
 * @author Nexepic
 * @date 2025/3/21
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

#include "graph/query/execution/QueryExecutor.hpp"
#include "debug/PlanVisualizer.hpp"
#include "graph/log/Log.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace graph::query {

	QueryResult QueryExecutor::execute(std::unique_ptr<execution::PhysicalOperator> plan) {
		QueryResult result;

		if (!plan)
			return result;

		std::string planTree = debug::PlanVisualizer::visualize(plan.get());
		log::Log::debug("{}", planTree);

		// 1. Initialize resources
		plan->open();

		// Ensure columns are set explicitly from the plan
		auto outputVars = plan->getOutputVariables();
		result.setColumns(outputVars);

		// 2. Pipeline Execution Loop
		while (auto batchOpt = plan->next()) {
			const auto &batch = batchOpt.value();

			// Reserve space for rows in this batch to avoid reallocations
			// Note: QueryResult::rows_ is a vector, we could batch append if we exposed an interface,
			// but loop append is fine.

			for (const auto &record: batch) {
				QueryResult::Row row;

				// Populate the row with ALL projected variables
				for (const auto &var: outputVars) {
					// Check order optimized for most likely types
					if (auto node = record.getNode(var)) {
						row[var] = ResultValue(*node);
					} else if (auto val = record.getValue(var)) {
						row[var] = ResultValue(*val);
					} else if (auto edge = record.getEdge(var)) {
						row[var] = ResultValue(*edge);
					} else {
						// Missing value -> Null
						row[var] = ResultValue();
					}
				}

				// Always add the row. If variables were projected, we need a row
				// (even if values are null) to maintain cardinality.
				if (!outputVars.empty()) {
					result.addRow(std::move(row));
				}
			}
		}

		// 4. Cleanup
		plan->close();

		return result;
	}

} // namespace graph::query
