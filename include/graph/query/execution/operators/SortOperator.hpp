/**
 * @file SortOperator.hpp
 * @author Nexepic
 * @date 2025/12/22
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

#include <string>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/query/expressions/Expression.hpp"

namespace graph::query::execution::operators {

	struct SortItem {
		std::shared_ptr<graph::query::expressions::Expression> expression; // Expression AST
		bool ascending = true;

		// Legacy constructor for backward compatibility
		SortItem([[maybe_unused]] std::string var, [[maybe_unused]] std::string prop, bool asc = true)
			: ascending(asc) {
			// This is a temporary bridge - text-based expressions will be replaced by AST
		}

		// New constructor for expression AST
		SortItem(std::shared_ptr<graph::query::expressions::Expression> expr, bool asc = true)
			: expression(std::move(expr)), ascending(asc) {}

		// Default constructor
		SortItem() = default;
	};

	class SortOperator : public PhysicalOperator {
	public:
		SortOperator(std::unique_ptr<PhysicalOperator> child, std::vector<SortItem> sortItems) :
			child_(std::move(child)), sortItems_(std::move(sortItems)) {}

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return child_->getOutputVariables();
		}

		[[nodiscard]] std::string toString() const override;

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override { return {child_.get()}; }

	private:
		std::unique_ptr<PhysicalOperator> child_;
		std::vector<SortItem> sortItems_;

		// Buffer
		std::vector<Record> sortedRecords_;
		size_t currentOutputIndex_ = 0;
		bool isSorted_ = false;
		static constexpr size_t BATCH_SIZE = 1000;

		void performSort();
	};

} // namespace graph::query::execution::operators
