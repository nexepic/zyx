/**
 * @file ListConfigOperator.hpp
 * @author Nexepic
 * @brief This source code is licensed under MIT License.
 * @date 2025/12/15
 *
 * @copyright Copyright (c) 2025 Nexepic
 *
 **/

#pragma once

#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"
#include "graph/storage/state/SystemStateManager.hpp"
#include "graph/storage/state/SystemStateKeys.hpp"

namespace graph::query::execution::operators {

	class ListConfigOperator : public PhysicalOperator {
	public:
		explicit ListConfigOperator(std::shared_ptr<storage::DataManager> dm, std::string filterKey = "")
			: dm_(std::move(dm)), filterKey_(std::move(filterKey)) {
			stateManager_ = std::make_unique<storage::state::SystemStateManager>(dm_);
		}

		void open() override { executed_ = false; }

		std::optional<RecordBatch> next() override {
			if (executed_) return std::nullopt;

			const std::string& stateKey = storage::state::keys::SYS_CONFIG;

			// Use generic getAll() to support mixed types
			auto allProps = stateManager_->getAll(stateKey);

			RecordBatch batch;
			for (const auto& [k, v] : allProps) {
				if (!filterKey_.empty() && k != filterKey_) continue;

				Record r;
				r.setValue("key", PropertyValue(k));
				r.setValue("value", v);
				batch.push_back(std::move(r));
			}

			executed_ = true;
			if (batch.empty()) return std::nullopt;
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
}