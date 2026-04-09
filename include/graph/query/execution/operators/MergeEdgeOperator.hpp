/**
 * @file MergeEdgeOperator.hpp
 * @author ZYX Contributors
 * @date 2025
 *
 * @copyright Copyright (c) 2025
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
#include "graph/storage/IDAllocator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query::execution::operators {

	/**
	 * @class MergeEdgeOperator
	 * @brief Implements MERGE (a)-[r:TYPE {props}]->(b) for edge patterns.
	 *
	 * This operator expects source and target nodes to already be resolved
	 * in the input record (via preceding MergeNodeOperator calls).
	 * It then merges (finds or creates) the edge between them.
	 */
	class MergeEdgeOperator : public PhysicalOperator {
	public:
		MergeEdgeOperator(std::shared_ptr<storage::DataManager> dm,
		                  std::shared_ptr<indexes::IndexManager> im,
		                  std::string sourceVar, std::string edgeVar, std::string targetVar,
		                  std::string edgeType,
		                  std::unordered_map<std::string, PropertyValue> matchProps,
		                  std::string direction,
		                  std::vector<SetItem> onCreateItems,
		                  std::vector<SetItem> onMatchItems) :
			dm_(std::move(dm)), im_(std::move(im)),
			sourceVar_(std::move(sourceVar)), edgeVar_(std::move(edgeVar)),
			targetVar_(std::move(targetVar)), edgeType_(std::move(edgeType)),
			matchProps_(std::move(matchProps)), direction_(std::move(direction)),
			onCreateItems_(std::move(onCreateItems)), onMatchItems_(std::move(onMatchItems)) {}

		void setChild(std::unique_ptr<PhysicalOperator> child) override { child_ = std::move(child); }

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};
			if (!edgeVar_.empty()) vars.push_back(edgeVar_);
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

		std::string sourceVar_;
		std::string edgeVar_;
		std::string targetVar_;
		std::string edgeType_;
		std::unordered_map<std::string, PropertyValue> matchProps_;
		std::string direction_;

		std::vector<SetItem> onCreateItems_;
		std::vector<SetItem> onMatchItems_;

		bool executed_ = false;
		int64_t edgeTypeId_ = 0;

		void processMergeEdge(Record &record);
		void applyEdgeUpdates(int64_t edgeId, const std::vector<SetItem> &items, Record &record);
	};

} // namespace graph::query::execution::operators
