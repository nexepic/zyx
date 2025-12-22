/**
 * @file RemoveOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/21
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution::operators {

    enum class RemoveActionType { PROPERTY, LABEL };

    struct RemoveItem {
        RemoveActionType type;
        std::string variable;
        std::string key; // Property Key OR Label Name
    };

    class RemoveOperator : public PhysicalOperator {
    public:
        RemoveOperator(std::shared_ptr<storage::DataManager> dm,
                       std::unique_ptr<PhysicalOperator> child,
                       std::vector<RemoveItem> items)
            : dm_(std::move(dm)), child_(std::move(child)), items_(std::move(items)) {}

        void open() override { if(child_) child_->open(); }

        std::optional<RecordBatch> next() override {
            auto batchOpt = child_->next();
            if (!batchOpt) return std::nullopt;

            RecordBatch outputBatch;
            outputBatch.reserve(batchOpt->size());

            for (auto record : *batchOpt) {
                for (const auto& item : items_) {

                    // --- Node Logic ---
                    if (auto nodeOpt = record.getNode(item.variable)) {
                        Node node = *nodeOpt;
                        int64_t id = node.getId();

                        if (item.type == RemoveActionType::PROPERTY) {
                            // 1. Remove Property
                            dm_->removeNodeProperty(id, item.key);

                            // Hydrate update
                            auto props = dm_->getNodeProperties(id);
                            node.setProperties(std::move(props));
                        }
                        else if (item.type == RemoveActionType::LABEL) {
                            // 2. Remove Label
                            // Only remove if it matches current label
                            if (node.getLabel() == item.key) {
                                node.setLabel(""); // Clear label
                                dm_->updateNode(node);
                            }
                        }
                        record.setNode(item.variable, node);
                    }

                    // --- Edge Logic ---
                    else if (auto edgeOpt = record.getEdge(item.variable)) {
                        Edge edge = *edgeOpt;
                        int64_t id = edge.getId();

                        if (item.type == RemoveActionType::PROPERTY) {
                            dm_->removeEdgeProperty(id, item.key);
                            auto props = dm_->getEdgeProperties(id);
                            edge.setProperties(std::move(props));
                            record.setEdge(item.variable, edge);
                        }
                    }
                }
                outputBatch.push_back(std::move(record));
            }
            return outputBatch;
        }

        void close() override { if(child_) child_->close(); }

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            return child_->getOutputVariables();
        }

        [[nodiscard]] std::string toString() const override {
            return "Remove(" + std::to_string(items_.size()) + " items)";
        }

        [[nodiscard]] std::vector<const PhysicalOperator*> getChildren() const override {
            return {child_.get()};
        }

    private:
        std::shared_ptr<storage::DataManager> dm_;
        std::unique_ptr<PhysicalOperator> child_;
        std::vector<RemoveItem> items_;
    };

}