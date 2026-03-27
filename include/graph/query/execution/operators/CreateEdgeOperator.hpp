/**
 * @file CreateEdgeOperator.hpp
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

#include "../PhysicalOperator.hpp"
#include "graph/storage/data/DataManager.hpp"

namespace graph::query::execution::operators {

	/**
	 * @class CreateEdgeOperator
	 * @brief Creates a relationship between two existing nodes in the pipeline.
	 */
	class CreateEdgeOperator : public PhysicalOperator {
	public:
		CreateEdgeOperator(std::shared_ptr<storage::DataManager> dm, std::string variable, std::string label,
						   std::unordered_map<std::string, PropertyValue> props, std::string sourceVar,
						   std::string targetVar) :
			dm_(std::move(dm)), variable_(std::move(variable)), label_(std::move(label)), props_(std::move(props)),
			sourceVar_(std::move(sourceVar)), targetVar_(std::move(targetVar)) {}

		void open() override {
			if (child_)
				child_->open();
		}

		std::optional<RecordBatch> next() override {
			if (!child_)
				return std::nullopt;

			auto batchOpt = child_->next();
			if (!batchOpt)
				return std::nullopt;

			RecordBatch &batch = *batchOpt;
			RecordBatch outputBatch;
			outputBatch.reserve(batch.size());

			// Optimization: Resolve Label ID once for the entire batch
			int64_t labelId = dm_->getOrCreateLabelId(label_);

			for (auto &record: batch) {
				// 2. Resolve endpoints
				auto srcNode = record.getNode(sourceVar_);
				auto tgtNode = record.getNode(targetVar_);

				if (srcNode && tgtNode) {
					// 3. Persist Edge
					// Use the ID-based constructor.
					// ID=0 means "allocate new ID".
					Edge newEdge(0, srcNode->getId(), tgtNode->getId(), labelId);

					// Set properties on edge before addEdge so constraint validation sees them
					newEdge.setProperties(props_);
					dm_->addEdge(newEdge);

					if (!props_.empty()) {
						dm_->addEdgeProperties(newEdge.getId(), props_);
					}

					// 4. Update Record
					record.setEdge(variable_, newEdge);
					outputBatch.push_back(std::move(record));
				}
			}

			return outputBatch;
		}

		void close() override {
			if (child_)
				child_->close();
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};

			// Deduplication
			bool exists = false;
			for (const auto &v: vars) {
				if (v == variable_) {
					exists = true;
					break;
				}
			}

			if (!exists) {
				vars.push_back(variable_);
			}

			return vars;
		}

		void setChild(std::unique_ptr<PhysicalOperator> child) { child_ = std::move(child); }

		[[nodiscard]] std::string toString() const override {
			return "CreateEdge(var=" + variable_ + ", type=" + label_ + ", src=" + sourceVar_ + ", tgt=" + targetVar_ +
				   ")";
		}

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (child_)
				return {child_.get()};
			return {};
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::unique_ptr<PhysicalOperator> child_;

		std::string variable_;
		std::string label_;
		std::unordered_map<std::string, PropertyValue> props_;
		std::string sourceVar_;
		std::string targetVar_;
	};
} // namespace graph::query::execution::operators