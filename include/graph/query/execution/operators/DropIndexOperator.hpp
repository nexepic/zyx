/**
 * @file DropIndexOperator.hpp
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

	class DropIndexOperator : public PhysicalOperator {
	public:
		DropIndexOperator(std::shared_ptr<indexes::IndexManager> indexManager,
						  std::string label,
						  std::string propertyKey)
			: indexManager_(std::move(indexManager)),
			  label_(std::move(label)),
			  propertyKey_(std::move(propertyKey)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			// 1. Drop the index
			// Note: Assuming "property" index type for specific keys.
			// If key is empty, it might drop a label index depending on implementation.
			bool success = indexManager_->dropIndex("node", "property", propertyKey_);

			// 2. Return result
			Record record;
			std::string msg = success ? "Index dropped" : "Index not found";
			record.setValue("result", PropertyValue(msg));

			RecordBatch batch;
			batch.push_back(std::move(record));

			executed_ = true;
			return batch;
		}

		void close() override {}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"result"};
		}

		[[nodiscard]] std::string toString() const override {
			return "DropIndex(label=" + label_ + ", key=" + propertyKey_ + ")";
		}

	private:
		std::shared_ptr<indexes::IndexManager> indexManager_;
		std::string label_;
		std::string propertyKey_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators