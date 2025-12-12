/**
 * @file ShowIndexesOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/12
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

    class ShowIndexesOperator : public PhysicalOperator {
    public:
        explicit ShowIndexesOperator(std::shared_ptr<indexes::IndexManager> indexManager)
            : indexManager_(std::move(indexManager)) {}

        void open() override { executed_ = false; }

        std::optional<RecordBatch> next() override {
            if (executed_) return std::nullopt;

            RecordBatch batch;

            // 1. Fetch Node Indexes
            auto nodeIndexes = indexManager_->listIndexes("node");
            for (const auto& [type, key] : nodeIndexes) {
                Record r;
                r.setValue("entity_type", PropertyValue("node"));
                r.setValue("type", PropertyValue(type));
                r.setValue("key", PropertyValue(key.empty() ? "<Label Index>" : key));
                batch.push_back(std::move(r));
            }

            // 2. Fetch Edge Indexes
            auto edgeIndexes = indexManager_->listIndexes("edge");
            for (const auto& [type, key] : edgeIndexes) {
                Record r;
                r.setValue("entity_type", PropertyValue("edge"));
                r.setValue("type", PropertyValue(type));
                r.setValue("key", PropertyValue(key.empty() ? "<Label Index>" : key));
                batch.push_back(std::move(r));
            }

            executed_ = true;

            if (batch.empty()) {
                // Return a single record indicating no indexes found (optional, or just return empty batch)
                return std::nullopt;
            }
            return batch;
        }

        void close() override {}

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            return {"entity_type", "type", "key"};
        }

        [[nodiscard]] std::string toString() const override {
            return "ShowIndexes()";
        }

    private:
        std::shared_ptr<indexes::IndexManager> indexManager_;
        bool executed_ = false;
    };

} // namespace graph::query::execution::operators