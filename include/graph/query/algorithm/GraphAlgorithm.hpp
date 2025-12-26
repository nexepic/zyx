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

#include <vector>
#include <string>
#include <functional>
#include <memory>
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
        std::vector<Node> findShortestPath(
            int64_t startNodeId,
            int64_t endNodeId,
            const std::string& direction = "both",
            int maxDepth = 15
        ) const;

        /**
         * @brief Finds all reachable target nodes within depth range.
         *        Used for variable length patterns: (a)-[*min..max]->(b).
         */
        std::vector<Node> findAllPaths(
            int64_t startNodeId,
            int minDepth,
            int maxDepth,
            const std::string& edgeLabel = "",
            const std::string& direction = "out"
        ) const;

        /**
         * @brief Performs BFS traversal.
         *        CHANGED: The visitor now receives (int64_t nodeId, int depth).
         *        REASON: Passing 'Node' object implies loading properties from disk,
         *                which makes traversal 10x-100x slower.
         *                The caller should hydrate only if needed.
         */
        void breadthFirstTraversal(
            int64_t startNodeId,
            std::function<bool(int64_t nodeId, int depth)> visitor,
            const std::string& direction = "both"
        ) const;

        // DFS implementation (optional, good to have)
        void depthFirstTraversal(
            int64_t startNodeId,
            std::function<bool(int64_t nodeId, int depth)> visitor,
            const std::string& direction = "both"
        ) const;

    private:
        std::shared_ptr<storage::DataManager> dm_;

        // Internal helper: Fast neighbor lookup (ID only)
        std::vector<int64_t> getNeighbors(int64_t nodeId, const std::string& direction, const std::string& edgeLabel = "") const;

        // Internal helper: Recursive DFS
        void dfsVariableLength(
            int64_t currentId,
            int currentDepth,
            int minDepth,
            int maxDepth,
            const std::string& edgeLabel,
            const std::string& direction,
            std::vector<int64_t>& visitedPath,
            std::vector<int64_t>& resultIds
        ) const;
    };

} // namespace graph::query::algorithm