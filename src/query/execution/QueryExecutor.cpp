/**
 * @file QueryExecutor.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "../../../include/graph/query/execution/QueryExecutor.hpp"
#include "debug/PlanVisualizer.hpp"
#include "graph/log/Log.hpp"

namespace graph::query {

	QueryResult QueryExecutor::execute(std::unique_ptr<execution::PhysicalOperator> plan) const {
		QueryResult result;

		if (!plan) return result;

		std::string planTree = debug::PlanVisualizer::visualize(plan.get());
		log::Log::debug("{}", planTree);

		// 1. Initialize resources
		plan->open();

		// 2. Pipeline Execution Loop
		// We keep pulling batches until std::nullopt is returned.
		while (auto batchOpt = plan->next()) {
			const auto& batch = batchOpt.value();
			auto vars = plan->getOutputVariables();

			// 3. Materialize Results
			// In a real system, you might not materialize everything immediately.
			// Here we convert the internal Record format to the public QueryResult.
			for (const auto& record : batch) {
				// Determine what to add to the result based on the query.
				// For now, we dump all found nodes (mimicking "RETURN *").
				// A 'ProjectionOperator' would normally handle filtering columns here.

				for (const auto& var : vars) {
					if (auto node = record.getNode(var)) {
						result.addNode(*node);
					}
					else if (auto edge = record.getEdge(var)) {
						result.addEdge(*edge);
					}
				}

				// If this Record contains non-graph data, save it as a row
				std::unordered_map<std::string, PropertyValue> row;
				bool hasValues = false;

				for (const auto& var : vars) {
					if (auto val = record.getValue(var)) {
						row[var] = *val;
						hasValues = true;
					}
				}

				if (hasValues) {
					result.addRow(std::move(row));
				}
			}
		}

		// 4. Cleanup
		plan->close();

		return result;
	}

} // namespace graph::query