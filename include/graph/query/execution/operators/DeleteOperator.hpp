/**
 * @file DeleteOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/18
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include <vector>
#include <string>
#include <stdexcept>

namespace graph::query::execution::operators {

    class DeleteOperator : public PhysicalOperator {
    public:
        /**
         * @brief Constructs the DeleteOperator.
         * @param dm DataManager to perform deletion.
         * @param child Upstream operator.
         * @param variables List of variables (nodes/edges) to delete.
         * @param detach If true, delete attached relationships first.
         */
        DeleteOperator(std::shared_ptr<storage::DataManager> dm,
                       std::unique_ptr<PhysicalOperator> child,
                       std::vector<std::string> variables,
                       bool detach)
            : dm_(std::move(dm)), child_(std::move(child)),
              variables_(std::move(variables)), detach_(detach) {}

        void open() override { if(child_) child_->open(); }

        std::optional<RecordBatch> next() override {
            auto batchOpt = child_->next();
            if (!batchOpt) return std::nullopt;

            // Pass-through the batch for potential RETURN clauses later
            RecordBatch& batch = *batchOpt;

            for (const auto& record : batch) {
                for (const auto& var : variables_) {

                    // --- Case 1: Delete Node ---
                    if (auto nodeOpt = record.getNode(var)) {
                        Node node = *nodeOpt;
                        int64_t nodeId = node.getId();

                        // 1. Check relationships using Traversal
                        // DataManager::findEdgesByNode internally calls RelationshipTraversal::getAllConnectedEdges
                        std::vector<Edge> connectedEdges = dm_->findEdgesByNode(nodeId, "both");

                        if (detach_) {
                            // DETACH DELETE: Remove all connected edges first
                            for (auto& edge : connectedEdges) {
                                // deleteEdge should internally call unlinkEdge to fix pointers
                                dm_->deleteEdge(edge);
                            }
                        } else {
                            // Standard DELETE: Constraint Check
                            if (!connectedEdges.empty()) {
                                throw std::runtime_error(
                                    "Cannot DELETE node " + std::to_string(nodeId) +
                                    " because it still has relationships. Use DETACH DELETE."
                                );
                            }
                        }

                        // 2. Delete the Node
                        dm_->deleteNode(node);
                    }

                    // --- Case 2: Delete Edge ---
                    else if (auto edgeOpt = record.getEdge(var)) {
                        Edge edge = *edgeOpt;
                        // For edges, we just delete them.
                        // DataManager ensures unlinkEdge is called.
                        dm_->deleteEdge(edge);
                    }
                }
            }

            return batch;
        }

        void close() override { if(child_) child_->close(); }

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            return child_->getOutputVariables();
        }

        [[nodiscard]] std::string toString() const override {
            std::string s = detach_ ? "DetachDelete(" : "Delete(";
            for(size_t i=0; i<variables_.size(); ++i) {
                s += variables_[i];
                if(i < variables_.size()-1) s += ", ";
            }
            return s + ")";
        }

        [[nodiscard]] std::vector<const PhysicalOperator*> getChildren() const override {
            return {child_.get()};
        }

    private:
        std::shared_ptr<storage::DataManager> dm_;
        std::unique_ptr<PhysicalOperator> child_;
        std::vector<std::string> variables_;
        bool detach_;
    };

} // namespace graph::query::execution::operators