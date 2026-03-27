/**
 * @file DropConstraintOperator.hpp
 * @author Nexepic
 * @date 2026/3/27
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

#include "../PhysicalOperator.hpp"
#include "graph/storage/constraints/ConstraintManager.hpp"

namespace graph::query::execution::operators {

	class DropConstraintOperator : public PhysicalOperator {
	public:
		DropConstraintOperator(std::shared_ptr<storage::constraints::ConstraintManager> cm,
							   std::string name, bool ifExists = false) :
			constraintManager_(std::move(cm)), name_(std::move(name)), ifExists_(ifExists) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;

			executed_ = true;
			bool dropped = constraintManager_->dropConstraint(name_);

			if (!dropped && !ifExists_) {
				throw std::runtime_error("Constraint '" + name_ + "' does not exist");
			}

			if (!dropped)
				return std::nullopt;

			Record record;
			record.setValue("result", PropertyValue("Constraint dropped"));
			RecordBatch batch;
			batch.push_back(std::move(record));
			return batch;
		}

		void close() override {}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"result"}; }

		[[nodiscard]] std::string toString() const override {
			return "DropConstraint(name=" + name_ + ")";
		}

	private:
		std::shared_ptr<storage::constraints::ConstraintManager> constraintManager_;
		std::string name_;
		bool ifExists_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
