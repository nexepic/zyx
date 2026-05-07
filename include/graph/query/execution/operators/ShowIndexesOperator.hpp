/**
 * @file ShowIndexesOperator.hpp
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

	class ShowIndexesOperator : public PhysicalOperator {
	public:
		explicit ShowIndexesOperator(std::shared_ptr<indexes::IndexManager> indexManager) :
			indexManager_(std::move(indexManager)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;

			RecordBatch batch;

			// Use the detailed list API from IndexManager
			auto indexes = indexManager_->listIndexesDetailed();

			for (const auto &[name, entityType, indexType, label, prop]: indexes) {
				Record r;
				// Set the values in the Record
				r.setValue("name", PropertyValue(name));
				r.setValue("type", PropertyValue(indexType));
				r.setValue("entity", PropertyValue(entityType == "node" ? "NODE" : "EDGE"));
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

		[[nodiscard]] std::string toString() const override { return "ShowIndexes()"; }

	private:
		std::shared_ptr<indexes::IndexManager> indexManager_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
