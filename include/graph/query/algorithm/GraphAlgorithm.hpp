/**
 * @file GraphAlgorithm.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/17
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "graph/core/Node.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/traversal/RelationshipTraversal.hpp"

namespace graph::query::algorithm {

	class GraphAlgorithm {
	public:
		explicit GraphAlgorithm(std::shared_ptr<storage::DataManager> dataManager);

		/**
		 * Finds nodes connected to the given node with optional filtering.
		 * (Original implementation preserved)
		 */
		std::vector<Node> findConnectedNodes(
			int64_t startNodeId,
			const std::string& direction = "both",
			const std::string& edgeLabel = "",
			const std::string& nodeLabel = ""
		) const;

		/**
		 * Finds the shortest path between two nodes using Dijkstra (unweighted).
		 * (Original implementation preserved)
		 */
		std::vector<Node> findShortestPath(
			int64_t startNodeId,
			int64_t endNodeId,
			const std::string& direction = "both"
		) const;

		/**
		 * Performs a breadth-first traversal of the graph.
		 * (Original implementation preserved)
		 */
		void breadthFirstTraversal(
			int64_t startNodeId,
			const std::function<bool(const Node&, int)>& visitFn,
			int maxDepth = 10,
			const std::string& direction = "both"
		) const;

		/**
		 * @brief Finds all paths from a start node within a depth range.
		 *        Used for variable length pattern matching: (a)-[*min..max]->(b).
		 *
		 * @param startNodeId Source node.
		 * @param minDepth Minimum hops (e.g., 1).
		 * @param maxDepth Maximum hops (e.g., 5).
		 * @param edgeLabel Filter edges by this label (empty for any).
		 * @param direction Direction of traversal.
		 * @return A list of found Target Nodes (b).
		 *         (Future improvement: Return full Path objects).
		 */
		std::vector<Node> findAllPaths(
			int64_t startNodeId,
			int minDepth,
			int maxDepth,
			const std::string& edgeLabel = "",
			const std::string& direction = "out"
		) const;

	private:
		std::shared_ptr<storage::DataManager> dataManager_;
		std::shared_ptr<traversal::RelationshipTraversal> traversal_;

		// Internal recursive DFS helper
		void dfsVariableLength(
			int64_t currentId,
			int currentDepth,
			int minDepth,
			int maxDepth,
			const std::string& edgeLabel,
			const std::string& direction,
			std::vector<int64_t>& visitedPath, // To prevent loops
			std::vector<Node>& results
		) const;
	};

} // namespace graph::query::algorithm