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
#include "indexes/IndexManager.hpp"
#include "graph/storage/FileStorage.hpp"

namespace graph::query {

	class QueryExecutor {
	public:
		QueryExecutor(std::shared_ptr<indexes::IndexManager> indexManager,
					 std::shared_ptr<storage::FileStorage> storage);

		// Execute a query plan and return the result
		QueryResult execute(const QueryPlan& plan);

	private:
		std::shared_ptr<indexes::IndexManager> indexManager_;
		std::shared_ptr<storage::FileStorage> storage_;

		// Helper methods for different operation types
		QueryResult executeLabelScan(const QueryPlan& plan);
		QueryResult executePropertyScan(const QueryPlan& plan);
		QueryResult executeLabelPropertyScan(const QueryPlan& plan);
		QueryResult executePropertyRangeScan(const QueryPlan& plan);
		QueryResult executeTextSearch(const QueryPlan& plan);
		QueryResult executeRelationshipScan(const QueryPlan& plan);
		QueryResult executeTraversalConnectedNodes(const QueryPlan& plan);
		QueryResult executeTraversalShortestPath(const QueryPlan& plan);
	};

} // namespace graph::query