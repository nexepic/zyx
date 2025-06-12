/**
 * @file QueryEngine.h
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/3/20
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include "QueryExecutor.h"
#include "QueryPlanner.h"
#include "QueryResult.h"
#include "graph/core/Edge.h"
#include "graph/core/Node.h"
#include "graph/storage/FileStorage.h"
#include "indexes/IndexManager.h"
#include "strategies/TraversalQuery.h"

namespace graph::query {

	class QueryEngine {
	public:
		explicit QueryEngine(std::shared_ptr<storage::FileStorage> storage);
		~QueryEngine();

		// Initialize the query engine and build indexes
		bool initialize();

		// Query nodes by label
		QueryResult findNodesByLabel(const std::string &label);

		// Query nodes by property key and value
		QueryResult findNodesByProperty(const std::string &key, const std::string &value);

		// Combined query - nodes with specific label AND property
		QueryResult findNodesByLabelAndProperty(const std::string &label, const std::string &key,
												const std::string &value);

		// Range query for numeric properties
		QueryResult findNodesByPropertyRange(const std::string &key, double minValue, double maxValue);

		// Text search within properties (partial matching)
		QueryResult findNodesByTextSearch(const std::string &key, const std::string &searchText);

		// Find relationships between nodes
		QueryResult findRelationships(uint64_t nodeId, const std::string &edgeLabel = "");

		// Find nodes connected to a specified node
		QueryResult findConnectedNodes(uint64_t nodeId, const std::string &edgeLabel = "",
									   const std::string &direction = "both");

		QueryResult findConnectedNodesTraversal(int64_t startNodeId, const std::string &direction = "both",
												const std::string &edgeLabel = "", const std::string &nodeLabel = "");

		QueryResult findShortestPath(int64_t startNodeId, int64_t endNodeId, int maxDepth = 10,
									 const std::string &direction = "both");

		void breadthFirstTraversal(int64_t startNodeId, const std::function<bool(const Node &, int)> &visitFn,
								   int maxDepth = 10, const std::string &direction = "both");

		// Rebuild indexes - call when data has changed significantly
		void rebuildIndexes();

	private:
		std::shared_ptr<storage::FileStorage> storage_;
		std::shared_ptr<indexes::IndexManager> indexManager_;
		std::unique_ptr<QueryPlanner> queryPlanner_;
		std::unique_ptr<QueryExecutor> queryExecutor_;
		std::shared_ptr<TraversalQuery> traversalQuery_;

		QueryResult nodesToQueryResult(const std::vector<Node>& nodes);
	};

} // namespace graph::query
