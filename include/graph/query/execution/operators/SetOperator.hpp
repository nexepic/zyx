/**
 * @file SetOperator.hpp
 * @author Nexepic
 * @date 2025/12/18
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
#include "graph/query/expressions/Expression.hpp"

#include <memory>
#include <string>
#include <vector>

namespace graph::storage {
	class DataManager;
}

namespace graph::query::execution::operators {

	enum class SetActionType { PROPERTY, LABEL, MAP_MERGE };

	struct SetItem {
		SetActionType type; // Distinguish Property vs Label
		std::string variable;
		std::string key; // Property Key OR Label Name
		std::shared_ptr<graph::query::expressions::Expression> expression; // Expression AST (ignored if type == LABEL)

		// Constructor for expression AST
		SetItem(SetActionType t, std::string var, std::string k, std::shared_ptr<graph::query::expressions::Expression> expr)
			: type(t), variable(std::move(var)), key(std::move(k)), expression(std::move(expr)) {}

		// Default constructor
		SetItem() = default;
	};

	class SetOperator : public PhysicalOperator {
	public:
		SetOperator(std::shared_ptr<storage::DataManager> dm, std::unique_ptr<PhysicalOperator> child,
					std::vector<SetItem> items) :
			dm_(std::move(dm)), child_(std::move(child)), items_(std::move(items)) {}

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return child_->getOutputVariables();
		}

		[[nodiscard]] std::string toString() const override;

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override { return {child_.get()}; }

		void setChild(std::unique_ptr<PhysicalOperator> child) override { child_ = std::move(child); }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::unique_ptr<PhysicalOperator> child_;
		std::vector<SetItem> items_;
	};
} // namespace graph::query::execution::operators