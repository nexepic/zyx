/**
 * @file CreateConstraintOperator.hpp
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

	class CreateConstraintOperator : public PhysicalOperator {
	public:
		CreateConstraintOperator(std::shared_ptr<storage::constraints::ConstraintManager> cm,
								 std::string name, std::string entityType,
								 std::string constraintType, std::string label,
								 std::vector<std::string> properties, std::string options) :
			constraintManager_(std::move(cm)), name_(std::move(name)),
			entityType_(std::move(entityType)), constraintType_(std::move(constraintType)),
			label_(std::move(label)), properties_(std::move(properties)),
			options_(std::move(options)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;

			constraintManager_->createConstraint(name_, entityType_, constraintType_,
												  label_, properties_, options_);

			Record record;
			record.setValue("result", PropertyValue("Constraint created"));
			RecordBatch batch;
			batch.push_back(std::move(record));
			executed_ = true;
			return batch;
		}

		void close() override {}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"result"}; }

		[[nodiscard]] std::string toString() const override {
			return "CreateConstraint(name=" + name_ + ", type=" + constraintType_ + ")";
		}

	private:
		std::shared_ptr<storage::constraints::ConstraintManager> constraintManager_;
		std::string name_, entityType_, constraintType_, label_;
		std::vector<std::string> properties_;
		std::string options_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
