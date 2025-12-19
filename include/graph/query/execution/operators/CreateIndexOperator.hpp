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
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

    class CreateIndexOperator : public PhysicalOperator {
    public:
        CreateIndexOperator(std::shared_ptr<indexes::IndexManager> im,
                            std::string name,
                            std::string label,
                            std::string propertyKey)
            : indexManager_(std::move(im)), name_(std::move(name)),
              label_(std::move(label)), propertyKey_(std::move(propertyKey)) {}

        void open() override { executed_ = false; }

    	std::optional<RecordBatch> next() override {
        	if (executed_) return std::nullopt;

        	bool success = indexManager_->createIndex(name_, "node", label_, propertyKey_);

        	Record record;
        	record.setValue("result", PropertyValue(success ? "Index created" : "Failed or Exists"));
        	RecordBatch batch; batch.push_back(std::move(record));
        	executed_ = true;
        	return batch;
        }

        void close() override {}

        [[nodiscard]] std::vector<std::string> getOutputVariables() const override {
            return {"result"};
        }

    	[[nodiscard]] std::string toString() const override {
        	return "CreateIndex(label=" + label_ + ", prop=" + propertyKey_ + ")";
        }

    private:
        std::shared_ptr<indexes::IndexManager> indexManager_;
    	std::string name_, label_, propertyKey_;
        bool executed_ = false;
    };

} // namespace graph::query::execution::operators