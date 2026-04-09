/**
 * @file MergeNodeOperator.hpp
 * @author Nexepic
 * @date 2025/12/21
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
#include "SetOperator.hpp"

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace graph::storage {
	class DataManager;
} // namespace graph::storage

namespace graph::query::indexes {
	class IndexManager;
} // namespace graph::query::indexes

namespace graph::query::execution::operators {

	class MergeNodeOperator : public PhysicalOperator {
	public:
		// Constructor (multi-label)
		MergeNodeOperator(std::shared_ptr<storage::DataManager> dm, std::shared_ptr<indexes::IndexManager> im,
						  std::string variable, std::vector<std::string> labels,
						  std::unordered_map<std::string, PropertyValue> matchProps, std::vector<SetItem> onCreateItems,
						  std::vector<SetItem> onMatchItems) :
			dm_(std::move(dm)), im_(std::move(im)), variable_(std::move(variable)), labels_(std::move(labels)),
			matchProps_(std::move(matchProps)), onCreateItems_(std::move(onCreateItems)),
			onMatchItems_(std::move(onMatchItems)) {}

		void setChild(std::unique_ptr<PhysicalOperator> child) override { child_ = std::move(child); }

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};
			vars.push_back(variable_);
			return vars;
		}

		[[nodiscard]] std::string toString() const override;

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (child_) return {child_.get()};
			return {};
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::shared_ptr<indexes::IndexManager> im_;
		std::unique_ptr<PhysicalOperator> child_;

		std::string variable_;
		std::vector<std::string> labels_;
		std::unordered_map<std::string, PropertyValue> matchProps_;

		std::vector<SetItem> onCreateItems_;
		std::vector<SetItem> onMatchItems_;

		bool executed_ = false;
		std::vector<int64_t> targetLabelIds_;

		void processMerge(Record &record);
		void applyUpdates(int64_t nodeId, const std::vector<SetItem> &items, Record &record);
	};
}
