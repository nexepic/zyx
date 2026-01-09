/**
 * @file DropIndexOperator.hpp
 * @author Nexepic
 * @date 2025/12/12
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

	class DropIndexOperator : public PhysicalOperator {
	public:
		// Constructor for Drop by Name
		DropIndexOperator(std::shared_ptr<indexes::IndexManager> indexManager, std::string name) :
			indexManager_(std::move(indexManager)), name_(std::move(name)) {}

		// Constructor for Drop by Definition
		DropIndexOperator(std::shared_ptr<indexes::IndexManager> indexManager, std::string label,
						  std::string propertyKey) :
			indexManager_(std::move(indexManager)), label_(std::move(label)), propertyKey_(std::move(propertyKey)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;

			bool success = false;

			if (!name_.empty()) {
				// Case 1: DROP INDEX index_name
				success = indexManager_->dropIndexByName(name_);
			} else {
				// Case 2: DROP INDEX ON :Label(prop)
				// Defaulting entityType to "node" as per Cypher standard context
				success = indexManager_->dropIndexByDefinition(label_, propertyKey_);
			}

			Record record;
			record.setValue("result", PropertyValue(success ? "Index dropped" : "Index not found"));

			RecordBatch batch;
			batch.push_back(std::move(record));

			executed_ = true;
			return batch;
		}

		void close() override {}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"result"}; }

		[[nodiscard]] std::string toString() const override {
			if (!name_.empty())
				return "DropIndex(name=" + name_ + ")";
			return "DropIndex(label=" + label_ + ", key=" + propertyKey_ + ")";
		}

	private:
		std::shared_ptr<indexes::IndexManager> indexManager_;
		std::string name_;
		std::string label_;
		std::string propertyKey_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
