/**
 * @file ShowConstraintsOperator.hpp
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

	class ShowConstraintsOperator : public PhysicalOperator {
	public:
		explicit ShowConstraintsOperator(std::shared_ptr<storage::constraints::ConstraintManager> cm) :
			constraintManager_(std::move(cm)) {}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;

			RecordBatch batch;
			auto constraints = constraintManager_->listConstraints();

			for (const auto &info : constraints) {
				Record r;
				r.setValue("name", PropertyValue(info.name));
				r.setValue("type", PropertyValue(info.constraintType));
				r.setValue("entity", PropertyValue(info.entityType));
				r.setValue("label", PropertyValue(info.label));
				r.setValue("properties", PropertyValue(info.properties));
				batch.push_back(std::move(r));
			}

			executed_ = true;

			if (batch.empty())
				return std::nullopt;
			return batch;
		}

		void close() override {}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"name", "type", "entity", "label", "properties"};
		}

		[[nodiscard]] std::string toString() const override { return "ShowConstraints()"; }

	private:
		std::shared_ptr<storage::constraints::ConstraintManager> constraintManager_;
		bool executed_ = false;
	};

} // namespace graph::query::execution::operators
