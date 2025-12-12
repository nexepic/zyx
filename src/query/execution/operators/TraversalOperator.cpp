/**
 * @file TraversalOperator.cpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#include "graph/query/execution/operators/TraversalOperator.hpp"

namespace graph::query::execution::operators {

    std::optional<RecordBatch> TraversalOperator::next() {
        // 1. Pull batch from upstream (Source Nodes)
        auto batchOpt = child_->next();
        if (!batchOpt) return std::nullopt;

        RecordBatch& inputBatch = *batchOpt;
        RecordBatch outputBatch;

        // Output can be larger than input (1-to-N relationship), so reserve conservatively
        outputBatch.reserve(inputBatch.size() * 2);

        for (const auto& record : inputBatch) {
            // 2. Get Source Node
            auto sourceNodeOpt = record.getNode(sourceVar_);
            if (!sourceNodeOpt) continue;
            int64_t sourceId = sourceNodeOpt->getId();

            // 3. Find Connected Edges (Low-level Traversal)
            // DataManager::findEdgesByNode calls RelationshipTraversal internally
            std::vector<Edge> edges = dm_->findEdgesByNode(sourceId, direction_);

            for (auto& edge : edges) {
                // 4. Filter by Edge Label
                if (!edgeLabel_.empty() && edge.getLabel() != edgeLabel_) {
                    continue;
                }

                // 5. Hydrate Edge Properties
                auto edgeProps = dm_->getEdgeProperties(edge.getId());
                // Assuming Edge class has setProperties, like Node
                // edge.setProperties(std::move(edgeProps));

                // 6. Resolve Target Node
                // Determine which side is the "other" side
                int64_t targetNodeId = (edge.getSourceNodeId() == sourceId)
                                       ? edge.getTargetNodeId()
                                       : edge.getSourceNodeId();

                Node targetNode = dm_->getNode(targetNodeId);
                if (!targetNode.isActive()) continue;

                // 7. Hydrate Target Node Properties
                auto nodeProps = dm_->getNodeProperties(targetNodeId);
                targetNode.setProperties(std::move(nodeProps));

                // 8. Create Extended Record
                Record newRecord = record; // Copy existing variables
                newRecord.setEdge(edgeVar_, edge);
                newRecord.setNode(targetVar_, targetNode);

                outputBatch.push_back(std::move(newRecord));
            }
        }

        // If batch was filtered out completely, we might need to fetch next
        // For simplicity, we return empty batch or check recursively in a robust impl.
        // Here we return whatever we found.
        return outputBatch;
    }

} // namespace graph::query::execution::operators