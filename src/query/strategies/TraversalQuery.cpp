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
		dataManager_(std::move(dataManager)),
		traversal_(std::make_shared<traversal::RelationshipTraversal>(dataManager_)) {}

	std::vector<Node> TraversalQuery::findConnectedNodes(int64_t startNodeId, const std::string &direction,
														 const std::string &edgeLabel, const std::string &nodeLabel) {
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

	std::vector<Node> TraversalQuery::findShortestPath(int64_t startNodeId, int64_t endNodeId, int maxDepth,
													   const std::string &direction) {
		// Use breadth-first search to find shortest path
		std::queue<int64_t> queue;
		std::unordered_set<int64_t> visited;
		std::unordered_map<int64_t, int64_t> parentMap;

		queue.push(startNodeId);
		visited.insert(startNodeId);

		bool found = false;
		while (!queue.empty() && !found) {
			int64_t currentNodeId = queue.front();
			queue.pop();

			// Find connected nodes
			std::vector<Node> connectedNodes = findConnectedNodes(currentNodeId, direction);

			for (const auto &node: connectedNodes) {
				int64_t nodeId = node.getId();
				if (visited.find(nodeId) == visited.end()) {
					visited.insert(nodeId);
					parentMap[nodeId] = currentNodeId;
					queue.push(nodeId);

					// Check if we found the destination
					if (nodeId == endNodeId) {
						found = true;
						break;
					}
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
			std::reverse(path.begin(), path.end());
		}

		return path;
	}

	void TraversalQuery::breadthFirstTraversal(int64_t startNodeId,
											   const std::function<bool(const Node &, int)> &visitFn, int maxDepth,
											   const std::string &direction) {
		std::queue<std::pair<int64_t, int>> queue; // Node ID and depth
		std::unordered_set<int64_t> visited;

		queue.push({startNodeId, 0});
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
				if (visited.find(nodeId) == visited.end()) {
					visited.insert(nodeId);
					queue.push({nodeId, depth + 1});
				}
			}
		}
	}

} // namespace graph::query
