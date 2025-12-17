/**
 * @file GraphAlgorithm.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/17
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/algorithm/GraphAlgorithm.hpp"
#include <algorithm>
#include <queue>
#include <unordered_map>
#include <unordered_set>

namespace graph::query::algorithm {

    GraphAlgorithm::GraphAlgorithm(std::shared_ptr<storage::DataManager> dataManager) :
        dataManager_(std::move(dataManager)),
        traversal_(dataManager_->getRelationshipTraversal()) {}

    std::vector<Node> GraphAlgorithm::findConnectedNodes(int64_t startNodeId, const std::string &direction,
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
                // Ensure hydration if your DataManager doesn't do it automatically
                Node connectedNode = dataManager_->getNode(connectedNodeId);
                auto props = dataManager_->getNodeProperties(connectedNodeId);
                // Assuming Node has setProperties based on previous fixes
                // connectedNode.setProperties(std::move(props));

                // Filter by node label if specified
                if (nodeLabel.empty() || connectedNode.getLabel() == nodeLabel) {
                    result.push_back(connectedNode);
                }
            }
        }

        return result;
    }

    std::vector<Node> GraphAlgorithm::findShortestPath(int64_t startNodeId, int64_t endNodeId,
                                                       const std::string &direction) const {
        // Use Dijkstra's algorithm to find shortest path
        std::unordered_map<int64_t, double> distances;
        std::unordered_map<int64_t, int64_t> parentMap;
        std::unordered_set<int64_t> visited;

        using NodeDistance = std::pair<double, int64_t>;
        std::priority_queue<NodeDistance, std::vector<NodeDistance>, std::greater<>> pq;

        // Initialize
        pq.emplace(0.0, startNodeId);
        distances[startNodeId] = 0.0;

        bool found = false;
        while (!pq.empty()) {
            auto [currentDist, currentNodeId] = pq.top();
            pq.pop();

            if (visited.contains(currentNodeId)) {
                continue;
            }

            visited.insert(currentNodeId);

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

                constexpr double weight = 1.0;

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
                // Hydrate node for result
                Node n = dataManager_->getNode(current);
                n.setProperties(dataManager_->getNodeProperties(current));
                path.push_back(n);
                current = parentMap[current];
            }
            Node start = dataManager_->getNode(startNodeId);
            start.setProperties(dataManager_->getNodeProperties(startNodeId));
            path.push_back(start);

            std::ranges::reverse(path);
        }

        return path;
    }

    void GraphAlgorithm::breadthFirstTraversal(int64_t startNodeId,
                                               const std::function<bool(const Node &, int)> &visitFn, int maxDepth,
                                               const std::string &direction) const {
        std::queue<std::pair<int64_t, int>> queue;
        std::unordered_set<int64_t> visited;

        try {
            Node startNode = dataManager_->getNode(startNodeId);
            if (startNode.getId() == 0 && startNodeId != 0) {
                return;
            }
        } catch (...) {
            return;
        }

        queue.emplace(startNodeId, 0);
        visited.insert(startNodeId);

        while (!queue.empty()) {
            auto [currentNodeId, depth] = queue.front();
            queue.pop();

            Node currentNode = dataManager_->getNode(currentNodeId);
            // Optional: Hydrate if visitor needs props
            bool continueTraversal = visitFn(currentNode, depth);

            if (!continueTraversal || depth >= maxDepth) {
                continue;
            }

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

} // namespace graph::query::algorithm