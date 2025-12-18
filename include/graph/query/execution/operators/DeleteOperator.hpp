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

namespace graph::query::execution::operators {

    class DeleteOperator : public PhysicalOperator {
    public:
        /**
         * @param detach If true, delete relationships attached to the node first.
         * @param variables List of variables (nodes/edges) to delete.
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

            // Delete operations are side-effects.
            // We usually pass the record through (so queries like DELETE n RETURN count(*) work),
            // but the entities in the record will be effectively "dead".

            for (const auto& record : *batchOpt) {
                for (const auto& var : variables_) {
                    // 1. Try to delete as Node
                    if (auto nodeOpt = record.getNode(var)) {
                        Node node = *nodeOpt;
                        if (detach_) {
                            // TODO: Implement detach logic (find edges and delete them)
                            // For now, assuming DataManager or user handles it via RelationshipTraversal
                        }
                        dm_->deleteNode(node);
                    }
                    // 2. Try to delete as Edge
                    else if (auto edgeOpt = record.getEdge(var)) {
                        Edge edge = *edgeOpt;
                        dm_->deleteEdge(edge);
                    }
                }
            }

            // Pass the batch through (Standard Volcano behavior for updates)
            return batchOpt;
        }

        void close() override { if(child_) child_->close(); }

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            return child_->getOutputVariables();
        }

        [[nodiscard]] std::string toString() const override {
            std::string s = detach_ ? "DetachDelete(" : "Delete(";
            for(const auto& v : variables_) s += v + " ";
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

}