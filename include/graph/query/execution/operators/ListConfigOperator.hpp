/**
 * @file ListConfigOperator.hpp
 * @author Nexepic
 * @date 2025/12/15
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
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"
#include "graph/storage/state/SystemStateManager.hpp"

namespace graph::query::execution::operators {

	class ListConfigOperator : public PhysicalOperator {
	public:
		explicit ListConfigOperator(std::shared_ptr<storage::DataManager> dm, std::string filterKey = "") :
			dm_(std::move(dm)), filterKey_(std::move(filterKey)) {
			stateManager_ = std::make_unique<storage::state::SystemStateManager>(dm_);
		}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;

			const std::string &stateKey = storage::state::keys::SYS_CONFIG;

			// Use generic getAll() to support mixed types
			auto allProps = stateManager_->getAll(stateKey);

			RecordBatch batch;
			for (const auto &[k, v]: allProps) {
				if (!filterKey_.empty() && k != filterKey_)
					continue;

				Record r;
				r.setValue("key", PropertyValue(k));
				r.setValue("value", v);
				batch.push_back(std::move(r));
			}

			executed_ = true;
			if (batch.empty())
				return std::nullopt;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override { return {"key", "value"}; }
		[[nodiscard]] std::string toString() const override { return "ListConfig()"; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::unique_ptr<storage::state::SystemStateManager> stateManager_;
		std::string filterKey_;
		bool executed_ = false;
	};
} // namespace graph::query::execution::operators
