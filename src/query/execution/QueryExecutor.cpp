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
#include <chrono>
#include <cstdint>
#include "debug/PlanVisualizer.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/log/Log.hpp"
#include "graph/query/api/QueryResult.hpp"

namespace graph::query {

	QueryResult QueryExecutor::execute(std::unique_ptr<execution::PhysicalOperator> plan) {
		using Clock = std::chrono::steady_clock;

		QueryResult result;

		if (!plan)
			return result;

		std::string planTree = debug::PlanVisualizer::visualize(plan.get());
		log::Log::debug("{}", planTree);

		// 1. Initialize resources
		auto openStart = Clock::now();
		plan->open();
		debug::PerfTrace::addDuration(
				"executor.open", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
																   openStart)
														  .count()));

		// Ensure columns are set explicitly from the plan
		auto outputVars = plan->getOutputVariables();
		result.setColumns(outputVars);

		// 2. Pipeline Execution Loop
		uint64_t pullNsTotal = 0;
		uint64_t materializeNsTotal = 0;
		while (true) {
			auto pullStart = Clock::now();
			auto batchOpt = plan->next();
			pullNsTotal += static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - pullStart).count());

			if (!batchOpt) {
				break;
			}

			auto materializeStart = Clock::now();
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

			materializeNsTotal += static_cast<uint64_t>(
					std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - materializeStart).count());
		}
		debug::PerfTrace::addDuration("executor.pull", pullNsTotal);
		debug::PerfTrace::addDuration("materialize", materializeNsTotal);

		// 4. Cleanup
		auto closeStart = Clock::now();
		plan->close();
		debug::PerfTrace::addDuration(
				"executor.close", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
																    closeStart)
														   .count()));

		return result;
	}

} // namespace graph::query
