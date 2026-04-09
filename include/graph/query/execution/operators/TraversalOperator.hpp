/**
 * @file TraversalOperator.hpp
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

#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution::operators {

	class TraversalOperator : public PhysicalOperator {
	public:
		/**
		 * @brief Constructs a traversal operator.
		 *
		 * @param dm DataManager for retrieving edges and nodes.
		 * @param child Upstream operator providing source nodes.
		 * @param sourceVar Variable name of the start node (already in record).
		 * @param edgeVar Variable name for the edge to be found.
		 * @param targetVar Variable name for the target node to be found.
		 * @param edgeType Filter by edge type (empty means all types).
		 * @param direction "out", "in", or "both".
		 */
		TraversalOperator(std::shared_ptr<storage::DataManager> dm, std::unique_ptr<PhysicalOperator> child,
						  std::string sourceVar, std::string edgeVar, std::string targetVar, std::string edgeType,
						  std::string direction) :
			dm_(std::move(dm)), child_(std::move(child)), sourceVar_(std::move(sourceVar)),
			edgeVar_(std::move(edgeVar)), targetVar_(std::move(targetVar)), edgeType_(std::move(edgeType)),
			direction_(std::move(direction)) {}

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto vars = child_->getOutputVariables();
			vars.push_back(edgeVar_);
			vars.push_back(targetVar_);
			return vars;
		}

		[[nodiscard]] std::string toString() const override;

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (child_) return {child_.get()};
			return {};
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::unique_ptr<PhysicalOperator> child_;

		std::string sourceVar_;
		std::string edgeVar_;
		std::string targetVar_;
		std::string edgeType_;
		std::string direction_;

		int64_t edgeTypeId_ = 0; // Cached ID
	};
}