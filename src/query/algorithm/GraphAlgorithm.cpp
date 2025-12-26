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

    GraphAlgorithm::GraphAlgorithm(std::shared_ptr<storage::DataManager> dataManager)
        : dm_(std::move(dataManager)) {}

    // --- Helper: Get Neighbor IDs (Fast, No IO for properties) ---
    std::vector<int64_t> GraphAlgorithm::getNeighbors(int64_t nodeId, const std::string& direction, const std::string& edgeLabel) const {
        std::vector<int64_t> neighbors;

        // This only reads Edge Headers (Small IO)
        auto edges = dm_->findEdgesByNode(nodeId, direction);

        neighbors.reserve(edges.size());
        for (const auto& edge : edges) {
            // Filter by edge label in memory (cheap)
            if (!edgeLabel.empty() && edge.getLabel() != edgeLabel) {
                continue;
            }

            int64_t neighborId = (edge.getSourceNodeId() == nodeId)
                                 ? edge.getTargetNodeId()
                                 : edge.getSourceNodeId();
            neighbors.push_back(neighborId);
        }
        return neighbors;
    }

    // --- Shortest Path ---
    std::vector<Node> GraphAlgorithm::findShortestPath(int64_t startNodeId, int64_t endNodeId,
                                                       const std::string& direction, int maxDepth) const {
        // Trivial case
        if (startNodeId == endNodeId) {
            Node n = dm_->getNode(startNodeId);
            if (n.getId() != 0) n.setProperties(dm_->getNodeProperties(startNodeId));
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
            if (currentDepth >= maxDepth) continue;

            auto neighbors = getNeighbors(current, direction);
            for (int64_t next : neighbors) {
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
    std::vector<Node> GraphAlgorithm::findAllPaths(
        int64_t startNodeId,
        int minDepth,
        int maxDepth,
        const std::string& edgeLabel,
        const std::string& direction
    ) const {
        std::vector<int64_t> resultIds;
        std::vector<int64_t> visitedPath;

        // DFS
        dfsVariableLength(startNodeId, 0, minDepth, maxDepth, edgeLabel, direction, visitedPath, resultIds);

        // Hydrate Results
        std::vector<Node> nodes;
        nodes.reserve(resultIds.size());
        for (int64_t id : resultIds) {
            Node n = dm_->getNode(id);
            n.setProperties(dm_->getNodeProperties(id));
            nodes.push_back(n);
        }
        return nodes;
    }

    void GraphAlgorithm::dfsVariableLength(
        int64_t currentId,
        int currentDepth,
        int minDepth,
        int maxDepth,
        const std::string& edgeLabel,
        const std::string& direction,
        std::vector<int64_t>& visitedPath,
        std::vector<int64_t>& resultIds
    ) const {
        // Cycle Check
        for (int64_t id : visitedPath) {
            if (id == currentId) return;
        }

        visitedPath.push_back(currentId);

        if (currentDepth >= minDepth) {
            resultIds.push_back(currentId);
        }

        if (currentDepth < maxDepth) {
            auto neighbors = getNeighbors(currentId, direction, edgeLabel);
            for (int64_t next : neighbors) {
                dfsVariableLength(next, currentDepth + 1, minDepth, maxDepth,
                                  edgeLabel, direction, visitedPath, resultIds);
            }
        }

        visitedPath.pop_back();
    }

    // --- BFS Traversal (Callback) ---
    void GraphAlgorithm::breadthFirstTraversal(
        int64_t startNodeId,
        std::function<bool(int64_t, int)> visitor,
        const std::string& direction
    ) const {
        std::queue<std::pair<int64_t, int>> q;
        std::unordered_set<int64_t> visited;

        // Validity check (Cheap)
        if (dm_->getNode(startNodeId).getId() == 0) return;

        q.push({startNodeId, 0});
        visited.insert(startNodeId);

        while (!q.empty()) {
            auto [currId, depth] = q.front();
            q.pop();

            // Callback (User decides if they need properties)
            if (!visitor(currId, depth)) return;

            auto neighbors = getNeighbors(currId, direction);
            for (int64_t next : neighbors) {
                if (!visited.contains(next)) {
                    visited.insert(next);
                    q.push({next, depth + 1});
                }
            }
        }
    }

    // --- DFS Traversal (Callback) ---
    void GraphAlgorithm::depthFirstTraversal(
        int64_t startNodeId,
        std::function<bool(int64_t, int)> visitor,
        const std::string& direction
    ) const {
        std::stack<std::pair<int64_t, int>> s;
        std::unordered_set<int64_t> visited;

        if (dm_->getNode(startNodeId).getId() == 0) return;

        s.push({startNodeId, 0});

        while (!s.empty()) {
            auto [currId, depth] = s.top();
            s.pop();

            if (!visited.contains(currId)) {
                visited.insert(currId);

                if (!visitor(currId, depth)) return;

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

} // namespace graph::query::algorithm