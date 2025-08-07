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

	/**
	 * @class QueryEngine
	 * @brief The main entry point for executing queries and managing indexes.
	 *
	 * This class provides a high-level API for all database operations,
	 * coordinating the planner, executor, and index manager.
	 */
	class QueryEngine {
	public:
		explicit QueryEngine(std::shared_ptr<storage::FileStorage> storage);
		~QueryEngine();

		// --- Index Management API (Updated) ---

		/**
		 * @brief Builds all available indexes for a given entity type.
		 * @param entityType "node" or "edge".
		 * @return true if successful, false otherwise.
		 */
		bool buildIndexes(const std::string &entityType) const;

		/**
		 * @brief Builds a property index for a specific key on a given entity type.
		 * @param entityType "node" or "edge".
		 * @param key The property key to index.
		 * @return true if successful, false otherwise.
		 */
		bool buildPropertyIndex(const std::string &entityType, const std::string &key) const;

		/**
		 * @brief Drops an index.
		 * @param entityType "node" or "edge".
		 * @param indexType "label" or "property".
		 * @param key The property key to drop (if indexType is "property").
		 * @return true if successful, false otherwise.
		 */
		bool dropIndex(const std::string &entityType, const std::string &indexType, const std::string &key = "") const;

		/**
		 * @brief Lists all active indexes for a given entity type.
		 * @param entityType "node" or "edge".
		 * @return A vector of pairs, where the first element is the index type ("label", "property")
		 *         and the second is the property key (or empty for label indexes).
		 */
		std::vector<std::pair<std::string, std::string>> listIndexes(const std::string &entityType) const;

		/**
		 * @brief Persists the state of all indexes to disk.
		 */
		void persistIndexState() const { indexManager_->persistState(); }

		// --- Node Query API ---
		QueryResult findNodesByLabel(const std::string &label) const;
		QueryResult findNodesByProperty(const std::string &key, const std::string &value) const;
		QueryResult findNodesByLabelAndProperty(const std::string &label, const std::string &key,
												const std::string &value) const;

		// --- Edge Query API ---
		QueryResult findEdgesByLabel(const std::string &label) const;

		QueryResult findEdgesByProperty(const std::string &key, const std::string &value) const;

		QueryResult findConnectedNodes(uint64_t nodeId, const std::string &edgeLabel, const std::string &direction,
									   const std::string &nodeLabel = "") const;

		QueryResult findShortestPath(int64_t startNodeId, int64_t endNodeId, int maxDepth = 10,
									 const std::string &direction = "both") const;

		// --- Traversal API ---
		void breadthFirstTraversal(int64_t startNodeId, const std::function<bool(const Node &, int)> &visitFn,
								   int maxDepth = 10, const std::string &direction = "both") const;

		// --- Accessors ---
		[[nodiscard]] std::shared_ptr<indexes::IndexManager> getIndexManager() const { return indexManager_; }

	private:
		std::shared_ptr<storage::FileStorage> storage_;
		std::shared_ptr<indexes::IndexManager> indexManager_;
		std::unique_ptr<QueryPlanner> queryPlanner_;
		std::unique_ptr<QueryExecutor> queryExecutor_;
		std::shared_ptr<TraversalQuery> traversalQuery_;
	};

} // namespace graph::query
