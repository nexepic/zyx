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
#include "graph/query/algorithm/GraphProjection.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::algorithm {

	// --- Result Types for Graph Algorithms ---

	struct WeightedPath {
		std::vector<Node> nodes;
		double totalWeight = 0.0;
	};

	struct NodeScore {
		int64_t nodeId;
		double score;
	};

	struct NodeComponent {
		int64_t nodeId;
		int64_t componentId;
	};

	class GraphAlgorithm {
	public:
		explicit GraphAlgorithm(std::shared_ptr<storage::DataManager> dataManager);

		// ============================================================
		// Existing Methods (DataManager-based, for Cypher MATCH/CALL)
		// ============================================================

		std::vector<Node> findShortestPath(int64_t startNodeId, int64_t endNodeId,
										   const std::string &direction = "both", int maxDepth = 15) const;

		std::vector<Node> findAllPaths(int64_t startNodeId, int minDepth, int maxDepth,
									   const std::string &edgeType = "", const std::string &direction = "out") const;

		void breadthFirstTraversal(int64_t startNodeId, std::function<bool(int64_t nodeId, int depth)> visitor,
								   const std::string &direction = "both") const;

		void depthFirstTraversal(int64_t startNodeId, std::function<bool(int64_t nodeId, int depth)> visitor,
								 const std::string &direction = "both") const;

		// ============================================================
		// GDS Algorithms (GraphProjection-based)
		// ============================================================

		/**
		 * @brief Dijkstra's weighted shortest path on a graph projection.
		 * @return WeightedPath with hydrated nodes and total weight. Empty if no path.
		 */
		WeightedPath dijkstra(const GraphProjection &graph, int64_t startId, int64_t endId) const;

		/**
		 * @brief PageRank via power iteration.
		 * @param maxIterations Maximum iterations before stopping
		 * @param dampingFactor Damping factor (typically 0.85)
		 * @param tolerance L1 convergence tolerance
		 * @return Scores sorted descending by score
		 */
		std::vector<NodeScore> pageRank(const GraphProjection &graph, int maxIterations = 20,
										double dampingFactor = 0.85, double tolerance = 1e-6) const;

		/**
		 * @brief Weakly Connected Components via Union-Find.
		 * @return Each node with its component ID (smallest node ID in component)
		 */
		std::vector<NodeComponent> connectedComponents(const GraphProjection &graph) const;

		/**
		 * @brief Betweenness Centrality via Brandes' algorithm.
		 * @param samplingSize If > 0, use only this many random source nodes (approximate)
		 * @return Scores sorted descending by score
		 */
		std::vector<NodeScore> betweennessCentrality(const GraphProjection &graph, int samplingSize = 0) const;

		/**
		 * @brief Closeness Centrality via BFS.
		 * @return Scores sorted descending by score
		 */
		std::vector<NodeScore> closenessCentrality(const GraphProjection &graph) const;

	private:
		std::shared_ptr<storage::DataManager> dm_;

		std::vector<int64_t> getNeighbors(int64_t nodeId, const std::string &direction,
										  const std::string &edgeType = "") const;

		void dfsVariableLength(int64_t currentId, int currentDepth, int minDepth, int maxDepth,
							   const std::string &edgeType, const std::string &direction,
							   std::vector<int64_t> &visitedPath, std::vector<int64_t> &resultIds) const;
	};

} // namespace graph::query::algorithm
