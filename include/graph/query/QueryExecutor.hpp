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

		// Helper methods for different operation types
		QueryResult executeLabelScan(const QueryPlan& plan) const;
		QueryResult executePropertyScan(const QueryPlan& plan) const;
		QueryResult executeLabelPropertyScan(const QueryPlan& plan) const;
		QueryResult executePropertyRangeScan(const QueryPlan& plan) const;
		QueryResult executeTextSearch(const QueryPlan& plan) const;
		QueryResult executeRelationshipScan(const QueryPlan& plan) const;
		QueryResult executeTraversalConnectedNodes(const QueryPlan& plan) const;
		QueryResult executeTraversalShortestPath(const QueryPlan& plan) const;
		QueryResult executeFullNodeScan(const QueryPlan &plan) const;
		QueryResult executeFullNodeLabelScan(const QueryPlan &plan) const;
		QueryResult executeFullNodePropertyScan(const QueryPlan &plan) const;
		QueryResult executeFullNodeLabelPropertyScan(const QueryPlan &plan) const;
		QueryResult executeFullRelationshipScan(const QueryPlan &plan) const;
	};

} // namespace graph::query