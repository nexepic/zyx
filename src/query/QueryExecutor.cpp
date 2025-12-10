/**
 * @file QueryExecutor.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/QueryExecutor.hpp"

namespace graph::query {

	QueryResult QueryExecutor::execute(std::unique_ptr<execution::PhysicalOperator> plan) const {
		QueryResult result;

		if (!plan) return result;

		// 1. Initialize resources
		plan->open();

		// 2. Pipeline Execution Loop
		// We keep pulling batches until std::nullopt is returned.
		while (auto batchOpt = plan->next()) {
			const auto& batch = batchOpt.value();

			// 3. Materialize Results
			// In a real system, you might not materialize everything immediately.
			// Here we convert the internal Record format to the public QueryResult.
			for (const auto& record : batch) {
				// Determine what to add to the result based on the query.
				// For now, we dump all found nodes (mimicking "RETURN *").
				// A 'ProjectionOperator' would normally handle filtering columns here.

				auto vars = plan->getOutputVariables();
				for (const auto& var : vars) {
					auto nodeOpt = record.getNode(var);
					if (nodeOpt) {
						result.addNode(*nodeOpt);
					}

					auto edgeOpt = record.getEdge(var);
					if (edgeOpt) {
						result.addEdge(*edgeOpt);
					}
				}
			}
		}

		// 4. Cleanup
		plan->close();

		return result;
	}

} // namespace graph::query