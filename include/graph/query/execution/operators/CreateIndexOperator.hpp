/**
 * @file CreateIndexOperator.hpp
 * @author Nexepic
 * @date 2025/12/11
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

	class CreateIndexOperator : public PhysicalOperator {
	public:
		// Single-property constructor
		CreateIndexOperator(std::shared_ptr<indexes::IndexManager> im, std::string name, std::string label,
							std::string propertyKey) :
			indexManager_(std::move(im)), name_(std::move(name)), label_(std::move(label)),
			propertyKeys_({std::move(propertyKey)}) {}

		// Multi-property (composite) constructor
		CreateIndexOperator(std::shared_ptr<indexes::IndexManager> im, std::string name, std::string label,
							std::vector<std::string> propertyKeys) :
			indexManager_(std::move(im)), name_(std::move(name)), label_(std::move(label)),
			propertyKeys_(std::move(propertyKeys)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;

			bool success = false;
			if (propertyKeys_.size() > 1) {
				// Composite index
				success = indexManager_->createCompositeIndex(name_, "node", label_, propertyKeys_);
			} else {
				// Single property index
				success = indexManager_->createIndex(name_, "node", label_, propertyKeys_[0]);
			}

			Record record;
			record.setValue("result", PropertyValue(success ? "Index created" : "Failed or Exists"));
			RecordBatch batch;
			batch.push_back(std::move(record));
			executed_ = true;
			return batch;
		}

		void close() override {}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"result"}; }

		[[nodiscard]] std::string toString() const override {
			std::string props;
			for (size_t i = 0; i < propertyKeys_.size(); ++i) {
				if (i > 0) props += ",";
				props += propertyKeys_[i];
			}
			return "CreateIndex(label=" + label_ + ", props=" + props + ")";
		}

	private:
		std::shared_ptr<indexes::IndexManager> indexManager_;
		std::string name_, label_;
		std::vector<std::string> propertyKeys_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
