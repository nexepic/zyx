/**
 * @file GraphAlgorithm.cpp
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

#include "graph/query/algorithm/GraphAlgorithm.hpp"
#include <algorithm>
#include <cmath>
#include <numeric>
#include <queue>
#include <random>
#include <stack>
#include <unordered_map>
#include <unordered_set>

namespace graph::query::algorithm {

	GraphAlgorithm::GraphAlgorithm(std::shared_ptr<storage::DataManager> dataManager) : dm_(std::move(dataManager)) {}

	// --- Helper: Get Neighbor IDs (Fast, No IO for properties) ---
	std::vector<int64_t> GraphAlgorithm::getNeighbors(int64_t nodeId, const std::string &direction,
													  const std::string &edgeType) const {
		std::vector<int64_t> neighbors;

		// This only reads Edge Headers (Small IO)
		auto edges = dm_->findEdgesByNode(nodeId, direction);

		// OPTIMIZATION: Resolve edge type ID ONCE before the loop
		int64_t targetTypeId = 0;
		if (!edgeType.empty()) {
			// Use getOrCreateTokenId (safe for reads, returns existing ID or new unused one)
			targetTypeId = dm_->getOrCreateTokenId(edgeType);
		}

		neighbors.reserve(edges.size());
		for (const auto &edge: edges) {
			// Filter by edge type in memory (cheap ID comparison)
			if (targetTypeId != 0) {
				if (edge.getTypeId() != targetTypeId) {
					continue;
				}
			}

			int64_t neighborId = (edge.getSourceNodeId() == nodeId) ? edge.getTargetNodeId() : edge.getSourceNodeId();
			neighbors.push_back(neighborId);
		}
		return neighbors;
	}

	// --- Shortest Path ---
	std::vector<Node> GraphAlgorithm::findShortestPath(int64_t startNodeId, int64_t endNodeId,
													   const std::string &direction, int maxDepth) const {
		// Trivial case
		if (startNodeId == endNodeId) {
			Node n = dm_->getNode(startNodeId);
			if (n.getId() != 0)
				n.setProperties(dm_->getNodeProperties(startNodeId));
			return {n};
		}

		// BFS Structures
		std::queue<int64_t> q;
		q.push(startNodeId);

		std::unordered_map<int64_t, int64_t> parent;
		std::unordered_map<int64_t, int> depthMap;

		parent[startNodeId] = -1;
		depthMap[startNodeId] = 0;

		bool found = false;

		// 1. Search Phase (ID only, Fast)
		while (!q.empty()) {
			int64_t current = q.front();
			q.pop();

			if (current == endNodeId) {
				found = true;
				break;
			}

			int currentDepth = depthMap[current];
			if (currentDepth >= maxDepth)
				continue;

			auto neighbors = getNeighbors(current, direction);
			for (int64_t next: neighbors) {
				if (parent.find(next) == parent.end()) {
					parent[next] = current;
					depthMap[next] = currentDepth + 1;
					q.push(next);
				}
			}
		}

		// 2. Hydration Phase (Load Properties only for result path)
		std::vector<Node> path;
		if (found) {
			int64_t curr = endNodeId;
			while (curr != -1) {
				Node n = dm_->getNode(curr);
				// Load heavy properties now
				n.setProperties(dm_->getNodeProperties(curr));
				path.push_back(n);

				curr = parent[curr];
			}
			std::ranges::reverse(path);
		}
		return path;
	}

	// --- Variable Length Paths ---
	std::vector<Node> GraphAlgorithm::findAllPaths(int64_t startNodeId, int minDepth, int maxDepth,
												   const std::string &edgeType, const std::string &direction) const {
		std::vector<int64_t> resultIds;
		std::vector<int64_t> visitedPath;

		// DFS
		dfsVariableLength(startNodeId, 0, minDepth, maxDepth, edgeType, direction, visitedPath, resultIds);

		// Hydrate Results
		std::vector<Node> nodes;
		nodes.reserve(resultIds.size());
		for (int64_t id: resultIds) {
			Node n = dm_->getNode(id);
			n.setProperties(dm_->getNodeProperties(id));
			nodes.push_back(n);
		}
		return nodes;
	}

	void GraphAlgorithm::dfsVariableLength(int64_t currentId, int currentDepth, int minDepth, int maxDepth,
										   const std::string &edgeType, const std::string &direction,
										   std::vector<int64_t> &visitedPath, std::vector<int64_t> &resultIds) const {
		// Cycle Check
		for (int64_t id: visitedPath) {
			if (id == currentId)
				return;
		}

		visitedPath.push_back(currentId);

		if (currentDepth >= minDepth) {
			resultIds.push_back(currentId);
		}

		if (currentDepth < maxDepth) {
			auto neighbors = getNeighbors(currentId, direction, edgeType);
			for (int64_t next: neighbors) {
				dfsVariableLength(next, currentDepth + 1, minDepth, maxDepth, edgeType, direction, visitedPath,
								  resultIds);
			}
		}

		visitedPath.pop_back();
	}

	// --- BFS Traversal (Callback) ---
	void GraphAlgorithm::breadthFirstTraversal(int64_t startNodeId, std::function<bool(int64_t, int)> visitor,
											   const std::string &direction) const {
		std::queue<std::pair<int64_t, int>> q;
		std::unordered_set<int64_t> visited;

		// Validity check (Cheap)
		if (dm_->getNode(startNodeId).getId() == 0)
			return;

		q.push({startNodeId, 0});
		visited.insert(startNodeId);

		while (!q.empty()) {
			auto [currId, depth] = q.front();
			q.pop();

			// Callback (User decides if they need properties)
			if (!visitor(currId, depth))
				return;

			auto neighbors = getNeighbors(currId, direction);
			for (int64_t next: neighbors) {
				if (!visited.contains(next)) {
					visited.insert(next);
					q.push({next, depth + 1});
				}
			}
		}
	}

	// --- DFS Traversal (Callback) ---
	void GraphAlgorithm::depthFirstTraversal(int64_t startNodeId, std::function<bool(int64_t, int)> visitor,
											 const std::string &direction) const {
		std::stack<std::pair<int64_t, int>> s;
		std::unordered_set<int64_t> visited;

		if (dm_->getNode(startNodeId).getId() == 0)
			return;

		s.push({startNodeId, 0});

		while (!s.empty()) {
			auto [currId, depth] = s.top();
			s.pop();

			if (!visited.contains(currId)) {
				visited.insert(currId);

				if (!visitor(currId, depth))
					return;

				auto neighbors = getNeighbors(currId, direction);
				// Reverse to maintain natural order in stack
				for (auto it = neighbors.rbegin(); it != neighbors.rend(); ++it) {
					if (!visited.contains(*it)) {
						s.push({*it, depth + 1});
					}
				}
			}
		}
	}

	// ============================================================
	// GDS Algorithms (GraphProjection-based)
	// ============================================================

	// --- Dijkstra's Weighted Shortest Path ---
	WeightedPath GraphAlgorithm::dijkstra(const GraphProjection &graph, int64_t startId, int64_t endId) const {
		if (!graph.getNodeIds().contains(startId) || !graph.getNodeIds().contains(endId)) {
			return {};
		}

		if (startId == endId) {
			Node n = dm_->getNode(startId);
			if (n.getId() != 0) {
				n.setProperties(dm_->getNodeProperties(startId));
			}
			return {{n}, 0.0};
		}

		// Min-heap: {cost, nodeId}
		using Entry = std::pair<double, int64_t>;
		std::priority_queue<Entry, std::vector<Entry>, std::greater<>> pq;

		std::unordered_map<int64_t, double> dist;
		std::unordered_map<int64_t, int64_t> parent;

		dist[startId] = 0.0;
		parent[startId] = -1;
		pq.push({0.0, startId});

		while (!pq.empty()) {
			auto [cost, current] = pq.top();
			pq.pop();

			if (current == endId) {
				break;
			}

			// Skip stale entries
			if (cost > dist[current]) {
				continue;
			}

			for (const auto &edge : graph.getOutNeighbors(current)) {
				double newCost = cost + edge.weight;
				auto it = dist.find(edge.targetId);
				if (it == dist.end() || newCost < it->second) {
					dist[edge.targetId] = newCost;
					parent[edge.targetId] = current;
					pq.push({newCost, edge.targetId});
				}
			}
		}

		// Reconstruct path
		if (dist.find(endId) == dist.end()) {
			return {};
		}

		WeightedPath result;
		result.totalWeight = dist[endId];

		std::vector<int64_t> pathIds;
		for (int64_t curr = endId; curr != -1; curr = parent[curr]) {
			pathIds.push_back(curr);
		}
		std::ranges::reverse(pathIds);

		result.nodes.reserve(pathIds.size());
		for (int64_t id : pathIds) {
			Node n = dm_->getNode(id);
			n.setProperties(dm_->getNodeProperties(id));
			result.nodes.push_back(n);
		}

		return result;
	}

	// --- PageRank (Power Iteration) ---
	std::vector<NodeScore> GraphAlgorithm::pageRank(const GraphProjection &graph, int maxIterations,
													double dampingFactor, double tolerance) const {
		const auto &nodeIds = graph.getNodeIds();
		size_t n = nodeIds.size();
		if (n == 0) {
			return {};
		}

		// Map node IDs to contiguous indices for efficient computation
		std::vector<int64_t> idList(nodeIds.begin(), nodeIds.end());
		std::unordered_map<int64_t, size_t> idToIdx;
		idToIdx.reserve(n);
		for (size_t i = 0; i < n; ++i) {
			idToIdx[idList[i]] = i;
		}

		// Initialize scores
		double initScore = 1.0 / static_cast<double>(n);
		std::vector<double> scores(n, initScore);
		std::vector<double> newScores(n);

		// Precompute out-degrees
		std::vector<size_t> outDegree(n);
		for (size_t i = 0; i < n; ++i) {
			outDegree[i] = graph.getOutNeighbors(idList[i]).size();
		}

		double baseTeleport = (1.0 - dampingFactor) / static_cast<double>(n);

		for (int iter = 0; iter < maxIterations; ++iter) {
			std::fill(newScores.begin(), newScores.end(), 0.0);

			// Accumulate dangling node mass (nodes with no outgoing edges)
			double danglingSum = 0.0;
			for (size_t i = 0; i < n; ++i) {
				if (outDegree[i] == 0) {
					danglingSum += scores[i];
				}
			}
			double danglingContrib = dampingFactor * danglingSum / static_cast<double>(n);

			// Distribute rank along edges
			for (size_t i = 0; i < n; ++i) {
				if (outDegree[i] == 0) {
					continue;
				}
				double contribution = dampingFactor * scores[i] / static_cast<double>(outDegree[i]);
				for (const auto &edge : graph.getOutNeighbors(idList[i])) {
					auto it = idToIdx.find(edge.targetId);
					if (it != idToIdx.end()) {
						newScores[it->second] += contribution;
					}
				}
			}

			// Add teleportation and dangling contribution
			for (size_t i = 0; i < n; ++i) {
				newScores[i] += baseTeleport + danglingContrib;
			}

			// Check convergence (L1 norm)
			double diff = 0.0;
			for (size_t i = 0; i < n; ++i) {
				diff += std::abs(newScores[i] - scores[i]);
			}

			scores.swap(newScores);

			if (diff < tolerance) {
				break;
			}
		}

		// Build result sorted by score descending
		std::vector<NodeScore> result;
		result.reserve(n);
		for (size_t i = 0; i < n; ++i) {
			result.push_back({idList[i], scores[i]});
		}
		std::sort(result.begin(), result.end(), [](const NodeScore &a, const NodeScore &b) {
			return a.score > b.score;
		});

		return result;
	}

	// --- Weakly Connected Components (Union-Find) ---
	std::vector<NodeComponent> GraphAlgorithm::connectedComponents(const GraphProjection &graph) const {
		const auto &nodeIds = graph.getNodeIds();
		if (nodeIds.empty()) {
			return {};
		}

		// Union-Find with path compression and union by rank
		std::unordered_map<int64_t, int64_t> parent;
		std::unordered_map<int64_t, int> rank;

		for (int64_t id : nodeIds) {
			parent[id] = id;
			rank[id] = 0;
		}

		// Find with path compression
		std::function<int64_t(int64_t)> find = [&](int64_t x) -> int64_t {
			if (parent[x] != x) {
				parent[x] = find(parent[x]);
			}
			return parent[x];
		};

		// Union by rank
		auto unite = [&](int64_t a, int64_t b) {
			int64_t ra = find(a);
			int64_t rb = find(b);
			if (ra == rb) return;
			if (rank[ra] < rank[rb]) std::swap(ra, rb);
			parent[rb] = ra;
			if (rank[ra] == rank[rb]) rank[ra]++;
		};

		// Process all edges (treat as undirected)
		for (int64_t nodeId : nodeIds) {
			for (const auto &edge : graph.getOutNeighbors(nodeId)) {
				if (nodeIds.contains(edge.targetId)) {
					unite(nodeId, edge.targetId);
				}
			}
		}

		// Build result
		std::vector<NodeComponent> result;
		result.reserve(nodeIds.size());
		for (int64_t id : nodeIds) {
			result.push_back({id, find(id)});
		}

		// Sort by componentId then nodeId for deterministic output
		std::sort(result.begin(), result.end(), [](const NodeComponent &a, const NodeComponent &b) {
			return a.componentId != b.componentId ? a.componentId < b.componentId : a.nodeId < b.nodeId;
		});

		return result;
	}

	// --- Betweenness Centrality (Brandes' Algorithm) ---
	std::vector<NodeScore> GraphAlgorithm::betweennessCentrality(const GraphProjection &graph,
																 int samplingSize) const {
		const auto &nodeIds = graph.getNodeIds();
		size_t n = nodeIds.size();
		if (n == 0) {
			return {};
		}

		std::vector<int64_t> idList(nodeIds.begin(), nodeIds.end());
		std::unordered_map<int64_t, double> centrality;
		for (int64_t id : idList) {
			centrality[id] = 0.0;
		}

		// Determine source nodes (all or sampled)
		std::vector<int64_t> sources = idList;
		if (samplingSize > 0 && samplingSize < static_cast<int>(n)) {
			std::mt19937 rng(42); // Fixed seed for reproducibility
			std::shuffle(sources.begin(), sources.end(), rng);
			sources.resize(static_cast<size_t>(samplingSize));
		}

		// Brandes' algorithm: BFS from each source
		for (int64_t s : sources) {
			// BFS
			std::stack<int64_t> stack;
			std::unordered_map<int64_t, std::vector<int64_t>> predecessors;
			std::unordered_map<int64_t, double> sigma; // number of shortest paths
			std::unordered_map<int64_t, int> dist;

			for (int64_t id : idList) {
				sigma[id] = 0.0;
				dist[id] = -1;
			}

			sigma[s] = 1.0;
			dist[s] = 0;

			std::queue<int64_t> queue;
			queue.push(s);

			while (!queue.empty()) {
				int64_t v = queue.front();
				queue.pop();
				stack.push(v);

				// Traverse both out and in neighbors (undirected betweenness)
				auto processNeighbor = [&](int64_t w) {
					if (dist[w] < 0) {
						dist[w] = dist[v] + 1;
						queue.push(w);
					}
					if (dist[w] == dist[v] + 1) {
						sigma[w] += sigma[v];
						predecessors[w].push_back(v);
					}
				};

				for (const auto &edge : graph.getOutNeighbors(v)) {
					if (nodeIds.contains(edge.targetId)) {
						processNeighbor(edge.targetId);
					}
				}
				for (const auto &edge : graph.getInNeighbors(v)) {
					if (nodeIds.contains(edge.targetId)) {
						processNeighbor(edge.targetId);
					}
				}
			}

			// Back-propagation
			std::unordered_map<int64_t, double> delta;
			for (int64_t id : idList) {
				delta[id] = 0.0;
			}

			while (!stack.empty()) {
				int64_t w = stack.top();
				stack.pop();
				for (int64_t v : predecessors[w]) {
					delta[v] += (sigma[v] / sigma[w]) * (1.0 + delta[w]);
				}
				if (w != s) {
					centrality[w] += delta[w];
				}
			}
		}

		// Normalize: divide by 2 for undirected graph
		for (auto &[id, score] : centrality) {
			score /= 2.0;
		}

		// Scale if sampling was used
		if (samplingSize > 0 && samplingSize < static_cast<int>(n)) {
			double scale = static_cast<double>(n) / static_cast<double>(samplingSize);
			for (auto &[id, score] : centrality) {
				score *= scale;
			}
		}

		// Build result sorted by score descending
		std::vector<NodeScore> result;
		result.reserve(n);
		for (const auto &[id, score] : centrality) {
			result.push_back({id, score});
		}
		std::sort(result.begin(), result.end(), [](const NodeScore &a, const NodeScore &b) {
			return a.score > b.score;
		});

		return result;
	}

	// --- Closeness Centrality ---
	std::vector<NodeScore> GraphAlgorithm::closenessCentrality(const GraphProjection &graph) const {
		const auto &nodeIds = graph.getNodeIds();
		size_t n = nodeIds.size();
		if (n == 0) {
			return {};
		}

		std::vector<int64_t> idList(nodeIds.begin(), nodeIds.end());
		std::vector<NodeScore> result;
		result.reserve(n);

		for (int64_t source : idList) {
			// BFS from source (undirected: use both out and in neighbors)
			std::unordered_map<int64_t, int> dist;
			dist[source] = 0;

			std::queue<int64_t> queue;
			queue.push(source);

			double totalDist = 0.0;
			int reachable = 0;

			while (!queue.empty()) {
				int64_t v = queue.front();
				queue.pop();

				// Traverse both directions for undirected closeness
				auto processNeighbor = [&](int64_t w) {
					if (!dist.contains(w) && nodeIds.contains(w)) {
						dist[w] = dist[v] + 1;
						totalDist += dist[w];
						reachable++;
						queue.push(w);
					}
				};

				for (const auto &edge : graph.getOutNeighbors(v)) {
					processNeighbor(edge.targetId);
				}
				for (const auto &edge : graph.getInNeighbors(v)) {
					processNeighbor(edge.targetId);
				}
			}

			// Closeness = (reachable) / totalDist (Wasserman-Faust normalization)
			double closeness = 0.0;
			if (reachable > 0 && totalDist > 0.0) {
				closeness = static_cast<double>(reachable) / totalDist;
			}

			result.push_back({source, closeness});
		}

		std::sort(result.begin(), result.end(), [](const NodeScore &a, const NodeScore &b) {
			return a.score > b.score;
		});

		return result;
	}

} // namespace graph::query::algorithm
