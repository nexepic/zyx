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

            // Use the detailed list API from IndexManager
            auto indexes = indexManager_->listIndexesDetailed();

            for (const auto& [name, type, label, prop] : indexes) {
                Record r;
                // Set the values in the Record
                r.setValue("name", PropertyValue(name));
                r.setValue("type", PropertyValue(type));
                r.setValue("entity", PropertyValue(type == "node" ? "NODE" : "EDGE"));
                r.setValue("name", PropertyValue(name));
                r.setValue("type", PropertyValue(prop.empty() ? "LABEL" : "PROPERTY")); // Infer type from property
                r.setValue("label", PropertyValue(label));
                r.setValue("properties", PropertyValue(prop));

                batch.push_back(std::move(r));
            }

            executed_ = true;

            if (batch.empty()) {
                return std::nullopt;
            }
            return batch;
        }

        void close() override {}

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            return {"name", "type", "label", "properties"};
        }

        [[nodiscard]] std::string toString() const override {
            return "ShowIndexes()";
        }

    private:
        std::shared_ptr<indexes::IndexManager> indexManager_;
        bool executed_ = false;
    };

} // namespace graph::query::execution::operators