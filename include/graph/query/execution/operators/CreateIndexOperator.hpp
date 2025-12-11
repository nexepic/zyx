/**
 * @file CreateIndexOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include "graph/query/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

    class CreateIndexOperator : public PhysicalOperator {
    public:
        /**
         * @brief Constructs the operator.
         * @param indexManager The manager responsible for building physical indexes.
         * @param label The node label (e.g., "User").
         * @param propertyKey The property to index (e.g., "name").
         */
        CreateIndexOperator(std::shared_ptr<indexes::IndexManager> indexManager,
                            std::string label,
                            std::string propertyKey)
            : indexManager_(std::move(indexManager)),
              label_(std::move(label)),
              propertyKey_(std::move(propertyKey)) {}

        void open() override { executed_ = false; }

        std::optional<RecordBatch> next() override {
            if (executed_) return std::nullopt;

            // 1. Build the index via IndexManager
            // Note: Currently IndexManager distinguishes "node" vs "edge".
            // Since Cypher CREATE INDEX ON :Label implies a Node index, we pass "node".
            // If your IndexManager supports Label-Specific indexes later, pass label_ here.
            bool success = indexManager_->buildPropertyIndex("node", propertyKey_);

            // 2. Return a summary record
            Record record;
            record.setValue("result", PropertyValue(success ? "Index created" : "Index already exists or failed"));

            RecordBatch batch;
            batch.push_back(std::move(record));

            executed_ = true;
            return batch;
        }

        void close() override {}

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            return {"result"};
        }

    private:
        std::shared_ptr<indexes::IndexManager> indexManager_;
        std::string label_;
        std::string propertyKey_;
        bool executed_ = false;
    };

} // namespace graph::query::execution::operators