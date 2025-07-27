/**
 * @file TraversalQuery.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/strategies/TraversalQuery.hpp"
#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace graph::query {

	TraversalQuery::TraversalQuery(std::shared_ptr<storage::DataManager> dataManager) :
		dataManager_(std::move(dataManager)), traversal_(dataManager_->getRelationshipTraversal()) {}

	std::vector<Node> TraversalQuery::findConnectedNodes(int64_t startNodeId, const std::string &direction,
														 const std::string &edgeLabel,
														 const std::string &nodeLabel) const {
		std::vector<Node> result;
		std::vector<Edge> edges;

		// Get edges based on direction
		if (direction == "outgoing" || direction == "both") {
			auto outEdges = traversal_->getOutgoingEdges(startNodeId);
			edges.insert(edges.end(), outEdges.begin(), outEdges.end());
		}

		if (direction == "incoming" || direction == "both") {
			auto inEdges = traversal_->getIncomingEdges(startNodeId);
			edges.insert(edges.end(), inEdges.begin(), inEdges.end());
		}

		// Filter edges by label if specified
		if (!edgeLabel.empty()) {
			std::vector<Edge> filteredEdges;
			for (const auto &edge: edges) {
				if (edge.getLabel() == edgeLabel) {
					filteredEdges.push_back(edge);
				}
			}
			edges = std::move(filteredEdges);
		}

		// Get connected nodes
		std::unordered_set<int64_t> nodeIds;
		for (const auto &edge: edges) {
			int64_t connectedNodeId;
			if (edge.getSourceNodeId() == startNodeId) {
				connectedNodeId = edge.getTargetNodeId();
			} else {
				connectedNodeId = edge.getSourceNodeId();
			}

			if (nodeIds.insert(connectedNodeId).second) {
				Node connectedNode = dataManager_->getNode(connectedNodeId);

				// Filter by node label if specified
				if (nodeLabel.empty() || connectedNode.getLabel() == nodeLabel) {
					result.push_back(connectedNode);
				}
			}
		}

		return result;
	}

	std::vector<Node> TraversalQuery::findShortestPath(int64_t startNodeId, int64_t endNodeId,
													   const std::string &direction) const {
		// Use Dijkstra's algorithm to find shortest path
		std::unordered_map<int64_t, double> distances;
		std::unordered_map<int64_t, int64_t> parentMap;
		std::unordered_set<int64_t> visited;

		// Priority queue with pairs of (distance, nodeId)
		// Using greater for min-heap priority queue
		using NodeDistance = std::pair<double, int64_t>;
		std::priority_queue<NodeDistance, std::vector<NodeDistance>, std::greater<>> pq;

		// Initialize
		pq.emplace(0.0, startNodeId);
		distances[startNodeId] = 0.0;

		bool found = false;
		while (!pq.empty()) {
			auto [currentDist, currentNodeId] = pq.top();
			pq.pop();

			// Skip if we've already processed this node with a shorter path
			if (visited.contains(currentNodeId)) {
				continue;
			}

			visited.insert(currentNodeId);

			// Check if we found the destination
			if (currentNodeId == endNodeId) {
				found = true;
				break;
			}

			// Get connected nodes and their edges

			for (std::vector<Node> connectedNodes = findConnectedNodes(currentNodeId, direction);
				 const auto &node: connectedNodes) {
				int64_t neighborId = node.getId();
				if (visited.contains(neighborId)) {
					continue;
				}

				// Get edge between current node and neighbor
				// For simplicity, using weight 1.0 for all edges
				// TODO: Use actual edge weights if available
				constexpr double weight = 1.0;

				// If this is a shorter path, update
				if (double newDist = distances[currentNodeId] + weight;
					!distances.contains(neighborId) || newDist < distances[neighborId]) {
					distances[neighborId] = newDist;
					parentMap[neighborId] = currentNodeId;
					pq.emplace(newDist, neighborId);
				}
			}
		}

		// Reconstruct path if found
		std::vector<Node> path;
		if (found) {
			int64_t current = endNodeId;
			while (current != startNodeId) {
				path.push_back(dataManager_->getNode(current));
				current = parentMap[current];
			}
			path.push_back(dataManager_->getNode(startNodeId));

			// Reverse to get path from start to end
			std::ranges::reverse(path);
		}

		return path;
	}

	void TraversalQuery::breadthFirstTraversal(int64_t startNodeId,
											   const std::function<bool(const Node &, int)> &visitFn, int maxDepth,
											   const std::string &direction) const {
		std::queue<std::pair<int64_t, int>> queue; // Node ID and depth
		std::unordered_set<int64_t> visited;

		queue.emplace(startNodeId, 0);
		visited.insert(startNodeId);

		while (!queue.empty()) {
			auto [currentNodeId, depth] = queue.front();
			queue.pop();

			// Visit the current node
			Node currentNode = dataManager_->getNode(currentNodeId);
			bool continueTraversal = visitFn(currentNode, depth);

			if (!continueTraversal || depth >= maxDepth) {
				continue;
			}

			// Traverse connected nodes
			std::vector<Node> connectedNodes = findConnectedNodes(currentNodeId, direction);

			for (const auto &node: connectedNodes) {
				int64_t nodeId = node.getId();
				if (!visited.contains(nodeId)) {
					visited.insert(nodeId);
					queue.emplace(nodeId, depth + 1);
				}
			}
		}
	}

} // namespace graph::query
