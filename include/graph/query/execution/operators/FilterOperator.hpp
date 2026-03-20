/**
 * @file FilterOperator.hpp
 * @author Nexepic
 * @date 2025/12/10
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

#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"

namespace graph::query::execution::operators {

	class FilterOperator : public PhysicalOperator {
	public:
		using Predicate = std::function<bool(const Record &)>;

		/**
		 * @brief Constructs a FilterOperator.
		 *
		 * @param child The upstream operator.
		 * @param predicate The generic lambda function for filtering.
		 * @param predicateStr A string representation of the logic (for debugging/visualization).
		 */
		FilterOperator(std::unique_ptr<PhysicalOperator> child, Predicate predicate, std::string predicateStr) :
			child_(std::move(child)), predicate_(std::move(predicate)), predicateStr_(std::move(predicateStr)) {}

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
		}

		// --- Visualization ---
		[[nodiscard]] std::string toString() const override;

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (child_)
				return {child_.get()};
			return {};
		}

	private:
		std::unique_ptr<PhysicalOperator> child_;
		Predicate predicate_;
		std::string predicateStr_;
	};

} // namespace graph::query::execution::operators
