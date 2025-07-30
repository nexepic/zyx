/**
 * @file QueryEngine.hpp
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
#include "QueryExecutor.hpp"
#include "QueryPlanner.hpp"
#include "QueryResult.hpp"
#include "graph/core/Edge.hpp"
#include "graph/core/Node.hpp"
#include "graph/storage/FileStorage.hpp"
#include "indexes/IndexManager.hpp"
#include "strategies/TraversalQuery.hpp"

namespace graph::query {

	class QueryEngine {
	public:
		explicit QueryEngine(std::shared_ptr<storage::FileStorage> storage);
		~QueryEngine();

		bool buildLabelIndex() const;

		bool buildPropertyIndex(const std::string &key) const;

		bool dropIndex(const std::string &indexType, const std::string &key) const;

		std::vector<std::pair<std::string, std::string>> listIndexes() const;

		// Query nodes by label
		QueryResult findNodesByLabel(const std::string &label) const;

		// Query nodes by property key and value
		QueryResult findNodesByProperty(const std::string &key, const std::string &value) const;

		// Combined query - nodes with specific label AND property
		QueryResult findNodesByLabelAndProperty(const std::string &label, const std::string &key,
												const std::string &value) const;

		// Range query for numeric properties
		QueryResult findNodesByPropertyRange(const std::string &key, double minValue, double maxValue) const;

		// Text search within properties (partial matching)
		QueryResult findNodesByTextSearch(const std::string &key, const std::string &searchText) const;

		// Find relationships between nodes
		QueryResult findRelationships(uint64_t nodeId, const std::string &edgeLabel = "") const;

		// Find nodes connected to a specified node
		QueryResult findConnectedNodes(uint64_t nodeId, const std::string &edgeLabel, const std::string &direction,
									   const std::string &nodeLabel = "") const;

		QueryResult findShortestPath(int64_t startNodeId, int64_t endNodeId, int maxDepth = 10,
									 const std::string &direction = "both") const;

		void breadthFirstTraversal(int64_t startNodeId, const std::function<bool(const Node &, int)> &visitFn,
								   int maxDepth = 10, const std::string &direction = "both") const;

		// Rebuild indexes - call when data has changed significantly
		void rebuildIndexes() const;

		void persistIndexState() const { indexManager_->persistState(); }

		[[nodiscard]] std::shared_ptr<indexes::IndexManager> getIndexManager() const { return indexManager_; }

	private:
		std::shared_ptr<storage::FileStorage> storage_;
		std::shared_ptr<indexes::IndexManager> indexManager_;
		std::unique_ptr<QueryPlanner> queryPlanner_;
		std::unique_ptr<QueryExecutor> queryExecutor_;
		std::shared_ptr<TraversalQuery> traversalQuery_;

		static QueryResult nodesToQueryResult(const std::vector<Node> &nodes);
	};

} // namespace graph::query
