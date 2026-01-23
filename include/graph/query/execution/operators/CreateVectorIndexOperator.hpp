/**
 * @file CreateVectorIndexOperator.hpp
 * @author Nexepic
 * @date 2026/1/23
 *
 * @copyright Copyright (c) 2026 Nexepic
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

#include "graph/query/execution/PhysicalOperator.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

	class CreateVectorIndexOperator : public PhysicalOperator {
	public:
		CreateVectorIndexOperator(std::shared_ptr<indexes::IndexManager> im, std::string name, std::string label,
								  std::string property, int dimension, std::string metric) :
			im_(std::move(im)), name_(std::move(name)), label_(std::move(label)), property_(std::move(property)),
			dimension_(dimension), metric_(std::move(metric)) {}

		void open() override {
			// No-op
		}

		std::optional<std::vector<Record>> next() override {
			if (executed_)
				return std::nullopt;

			// Delegate to IndexManager to handle creation and metadata registration
			bool success = im_->createVectorIndex(name_, label_, property_, dimension_, metric_);

			if (!success) {
				// Optionally log warning or throw, depending on desired behavior for "IF NOT EXISTS" logic
			}

			executed_ = true;
			return std::vector<Record>{}; // Return empty batch
		}

		void close() override {}

		std::vector<std::string> getOutputVariables() const override { return {}; }

		// Fix: Implement pure virtual function from PhysicalOperator
		std::string toString() const override {
			return "CreateVectorIndex(name=" + name_ + ", label=" + label_ + ", prop=" + property_ +
				   ", dim=" + std::to_string(dimension_) + ")";
		}

	private:
		std::shared_ptr<indexes::IndexManager> im_;
		std::string name_;
		std::string label_;
		std::string property_;
		int dimension_;
		std::string metric_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
