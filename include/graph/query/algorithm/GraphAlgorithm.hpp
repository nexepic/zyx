/**
 * @file GraphAlgorithm.hpp
 * @author Nexepic
 * @date 2025/12/17
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

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "graph/core/Node.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::algorithm {

	class GraphAlgorithm {
	public:
		explicit GraphAlgorithm(std::shared_ptr<storage::DataManager> dataManager);

		/**
		 * @brief Finds the shortest path between two nodes (Unweighted BFS).
		 *        OPTIMIZED: Uses ID-only traversal.
		 * @return Path as a list of fully hydrated Nodes.
		 */
		std::vector<Node> findShortestPath(int64_t startNodeId, int64_t endNodeId,
										   const std::string &direction = "both", int maxDepth = 15) const;

		/**
		 * @brief Finds all reachable target nodes within depth range.
		 *        Used for variable length patterns: (a)-[*min..max]->(b).
		 */
		std::vector<Node> findAllPaths(int64_t startNodeId, int minDepth, int maxDepth,
									   const std::string &edgeLabel = "", const std::string &direction = "out") const;

		/**
		 * @brief Performs BFS traversal.
		 *        CHANGED: The visitor now receives (int64_t nodeId, int depth).
		 *        REASON: Passing 'Node' object implies loading properties from disk,
		 *                which makes traversal 10x-100x slower.
		 *                The caller should hydrate only if needed.
		 */
		void breadthFirstTraversal(int64_t startNodeId, std::function<bool(int64_t nodeId, int depth)> visitor,
								   const std::string &direction = "both") const;

		// DFS implementation (optional, good to have)
		void depthFirstTraversal(int64_t startNodeId, std::function<bool(int64_t nodeId, int depth)> visitor,
								 const std::string &direction = "both") const;

	private:
		std::shared_ptr<storage::DataManager> dm_;

		// Internal helper: Fast neighbor lookup (ID only)
		std::vector<int64_t> getNeighbors(int64_t nodeId, const std::string &direction,
										  const std::string &edgeLabel = "") const;

		// Internal helper: Recursive DFS
		void dfsVariableLength(int64_t currentId, int currentDepth, int minDepth, int maxDepth,
							   const std::string &edgeLabel, const std::string &direction,
							   std::vector<int64_t> &visitedPath, std::vector<int64_t> &resultIds) const;
	};

} // namespace graph::query::algorithm
