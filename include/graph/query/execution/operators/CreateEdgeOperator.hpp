/**
 * @file CreateEdgeOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/10
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution::operators {

    /**
     * @class CreateEdgeOperator
     * @brief Creates a relationship between two existing nodes in the pipeline.
     */
    class CreateEdgeOperator : public PhysicalOperator {
    public:
        CreateEdgeOperator(std::shared_ptr<storage::DataManager> dm,
                           std::string variable,
                           std::string label,
                           std::unordered_map<std::string, PropertyValue> props,
                           std::string sourceVar,
                           std::string targetVar)
            : dm_(std::move(dm)), variable_(std::move(variable)),
              label_(std::move(label)), props_(std::move(props)),
              sourceVar_(std::move(sourceVar)), targetVar_(std::move(targetVar)) {}

        void open() override {
            if(child_) child_->open();
            // If this is a root operator (unlikely for edges), handle accordingly.
        }

        std::optional<RecordBatch> next() override {
            // Edges usually depend on previous nodes (MATCH (a), (b) CREATE (a)-[e]->(b))
            // So we act as a Pipe here.

            // 1. Get input batch
            // Note: In a real system, CreateEdge MUST have a child.
            // If child_ is null, this is a logic error (cannot connect nulls).
            if (!child_) return std::nullopt;

            auto batchOpt = child_->next();
            if (!batchOpt) return std::nullopt;

            RecordBatch& batch = *batchOpt;
            RecordBatch outputBatch;
            outputBatch.reserve(batch.size());

            for (auto& record : batch) {
                // 2. Resolve endpoints
                auto srcNode = record.getNode(sourceVar_);
                auto tgtNode = record.getNode(targetVar_);

                if (srcNode && tgtNode) {
                    // 3. Persist Edge
                    Edge newEdge(0, srcNode->getId(), tgtNode->getId(), label_);
                    dm_->addEdge(newEdge);

                    if (!props_.empty()) {
                        dm_->addEdgeProperties(newEdge.getId(), props_);
                    }

                    // 4. Update Record
                    record.setEdge(variable_, newEdge);
                    outputBatch.push_back(std::move(record));
                } else {
                    // If endpoints are missing, strictly we should fail or skip.
                    // Skipping for now.
                }
            }

            return outputBatch;
        }

        void close() override { if(child_) child_->close(); }

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};
            vars.push_back(variable_);
            return vars;
        }

        // Setter for chaining (Crucial for Pipe operators)
        void setChild(std::unique_ptr<PhysicalOperator> child) {
            child_ = std::move(child);
        }

    private:
        std::shared_ptr<storage::DataManager> dm_;
        std::unique_ptr<PhysicalOperator> child_; // Upstream operator

        std::string variable_;
        std::string label_;
        std::unordered_map<std::string, PropertyValue> props_;
        std::string sourceVar_;
        std::string targetVar_;
    };
}