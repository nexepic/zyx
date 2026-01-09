/**
 * @file SetConfigOperator.hpp
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

	class SetConfigOperator : public PhysicalOperator {
	public:
		SetConfigOperator(std::shared_ptr<storage::DataManager> dm, std::string key, PropertyValue value) :
			dm_(std::move(dm)), key_(std::move(key)), value_(std::move(value)) {
			stateManager_ = std::make_unique<storage::state::SystemStateManager>(dm_);
		}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_)
				return std::nullopt;

			// Use the constant key from SystemStateKeys
			const std::string &stateKey = storage::state::keys::SYS_CONFIG;

			// Dispatch based on value type to call the correct typed set()
			std::visit(
					[&]<typename T0>(T0 &&arg) {
						using T = std::decay_t<T0>;
						if constexpr (!std::is_same_v<T, std::monostate>) {
							stateManager_->set(stateKey, key_, arg);
						}
					},
					value_.getVariant());

			// Return feedback
			Record r;
			r.setValue("key", PropertyValue(key_));
			r.setValue("value", value_);
			r.setValue("status", PropertyValue("OK"));

			RecordBatch batch;
			batch.push_back(std::move(r));
			executed_ = true;
			return batch;
		}

		void close() override {}
		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return {"key", "value", "status"};
		}
		[[nodiscard]] std::string toString() const override { return "SetConfig(" + key_ + ")"; }

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::unique_ptr<storage::state::SystemStateManager> stateManager_;
		std::string key_;
		PropertyValue value_;
		bool executed_ = false;
	};
} // namespace graph::query::execution::operators
