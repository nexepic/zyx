/**
 * @file TraversalQuery.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/6/11
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

namespace graph::query {

    /**
     * Provides high-level query operations on the graph database.
     * Uses the GraphTraversal for efficient graph traversal operations.
     */
    class TraversalQuery {
    public:
        explicit TraversalQuery(std::shared_ptr<storage::DataManager> dataManager);

        /**
         * Finds nodes connected to the given node with optional filtering
         *
         * @param startNodeId The ID of the starting node
         * @param direction The direction of the relationship ("in", "out", or "both")
         * @param edgeLabel Optional edge label filter
         * @param nodeLabel Optional node label filter
         * @return A vector of nodes that match the criteria
         */
        std::vector<Node> findConnectedNodes(
            int64_t startNodeId,
            const std::string& direction = "both",
            const std::string& edgeLabel = "",
            const std::string& nodeLabel = ""
        ) const;

        /**
         * Finds the shortest path between two nodes
         *
         * @param startNodeId The ID of the starting node
         * @param endNodeId The ID of the destination node
         * @param direction The direction of relationships to follow
         * @return A vector of nodes representing the path, empty if no path found
         */
        std::vector<Node> findShortestPath(
            int64_t startNodeId,
            int64_t endNodeId,
            const std::string& direction = "both"
        ) const;

        /**
         * Performs a breadth-first traversal of the graph
         *
         * @param startNodeId The ID of the starting node
         * @param visitFn Function to call for each visited node
         * @param maxDepth Maximum depth to traverse
         * @param direction Direction of edges to follow
         */
        void breadthFirstTraversal(
            int64_t startNodeId,
            const std::function<bool(const Node&, int)>& visitFn,
            int maxDepth = 10,
            const std::string& direction = "both"
        ) const;

    private:
        std::shared_ptr<storage::DataManager> dataManager_;
        std::shared_ptr<traversal::RelationshipTraversal> traversal_;
    };

} // namespace graph::query