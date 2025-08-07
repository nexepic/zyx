/**
 * @file QueryExecutor.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <memory>
#include "QueryPlanner.hpp"
#include "QueryResult.hpp"
#include "UnifiedQueryView.hpp"
#include "indexes/IndexManager.hpp"

namespace graph::query {

	class QueryExecutor {
	public:
		QueryExecutor(std::shared_ptr<indexes::IndexManager> indexManager,
					 std::shared_ptr<storage::DataManager> dataManager);

		// Execute a query plan and return the result
		QueryResult execute(const QueryPlan& plan) const;

	private:
		std::shared_ptr<indexes::IndexManager> indexManager_;
		std::shared_ptr<storage::DataManager> dataManager_;
		std::unique_ptr<UnifiedQueryView> unifiedQueryView_;

		// --- Private Execution Helpers (now specific to entity type) ---

		// Node execution helpers
		QueryResult executeNodeLabelScan(const QueryPlan& plan) const;
		QueryResult executeNodePropertyScan(const QueryPlan& plan) const;
		QueryResult executeNodeLabelPropertyScan(const QueryPlan& plan) const;
		// Note: PropertyRangeScan can be re-added here if needed.

		// Edge execution helpers
		QueryResult executeEdgeLabelScan(const QueryPlan& plan) const;
		QueryResult executeEdgePropertyScan(const QueryPlan& plan) const;

		// Full scan helpers
		QueryResult executeFullNodeLabelScan(const QueryPlan& plan) const;
		QueryResult executeFullNodePropertyScan(const QueryPlan& plan) const;
		QueryResult executeFullNodeLabelPropertyScan(const QueryPlan& plan) const;
		QueryResult executeFullEdgeLabelScan(const QueryPlan& plan) const;
		QueryResult executeFullEdgePropertyScan(const QueryPlan& plan) const;

		QueryResult executeTraversalConnectedNodes(const QueryPlan& plan) const;
		QueryResult executeTraversalShortestPath(const QueryPlan& plan) const;
	};

} // namespace graph::query