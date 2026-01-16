/**
 * @file CreateNodeOperator.hpp
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

	class CreateNodeOperator : public PhysicalOperator {
	public:
		CreateNodeOperator(std::shared_ptr<storage::DataManager> dm, std::string variable, std::string label,
						   std::unordered_map<std::string, PropertyValue> props) :
			dm_(std::move(dm)), variable_(std::move(variable)), label_(std::move(label)), props_(std::move(props)) {}

		void setChild(std::unique_ptr<PhysicalOperator> child) { child_ = std::move(child); }

		void open() override {
			executed_ = false;
			childExhausted_ = false;
			nodeBuffer_.clear();
			recordBuffer_.clear();
			if (child_)
				child_->open();
		}

		std::optional<RecordBatch> next() override {
			// Resolve Label ID once per call.
			// This ID will be used for all nodes created in this batch/call.
			int64_t labelId = dm_->getOrCreateLabelId(label_);

			// Case A: Pipeline (Chained to previous MATCH, UNWIND, or CREATE)
			if (child_) {
				if (childExhausted_) {
					return flushBuffer();
				}

				while (true) {
					auto batchOpt = child_->next();

					// Upstream is exhausted
					if (!batchOpt) {
						childExhausted_ = true;
						return flushBuffer();
					}

					RecordBatch &inputBatch = *batchOpt;

					if (inputBatch.empty())
						continue;

					for (auto &record: inputBatch) {

						if (auto existingNode = record.getNode(variable_)) {
							if (!nodeBuffer_.empty()) {
								auto flushedBatch = flushBuffer().value_or(RecordBatch{});
								flushedBatch.push_back(std::move(record));
								return flushedBatch;
							}

							RecordBatch singleBatch;
							singleBatch.push_back(std::move(record));
							return singleBatch;
						} else {
							// Variable unbound -> Queue for Batch Creation
							// Use ID constructor
							Node newNode(0, labelId);

							// Pre-set properties in memory
							if (!props_.empty()) {
								newNode.setProperties(props_);
							}

							nodeBuffer_.push_back(std::move(newNode));
							recordBuffer_.push_back(std::move(record));
						}
					}

					if (nodeBuffer_.size() >= BATCH_SIZE) {
						return flushBuffer();
					}
				}
			}

			// Case B: Source (Start of query, Single Create)
			if (executed_)
				return std::nullopt;

			Node newNode = performSingleCreate(labelId);
			Record record;
			record.setNode(variable_, newNode);
			RecordBatch batch;
			batch.push_back(std::move(record));

			executed_ = true;
			return batch;
		}

		void close() override {
			if (child_)
				child_->close();
			nodeBuffer_.clear();
			recordBuffer_.clear();
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto vars = child_ ? child_->getOutputVariables() : std::vector<std::string>{};

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

		[[nodiscard]] std::string toString() const override {
			return "CreateNode(var=" + variable_ + ", label=" + label_ + ")";
		}

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (child_) {
				return {child_.get()};
			}
			return {};
		}

	private:
		std::shared_ptr<storage::DataManager> dm_;
		std::unique_ptr<PhysicalOperator> child_;
		std::string variable_;
		std::string label_;
		std::unordered_map<std::string, PropertyValue> props_;

		// State for batching
		bool executed_ = false;
		bool childExhausted_ = false;
		std::vector<Node> nodeBuffer_;
		std::vector<Record> recordBuffer_;

		static constexpr size_t BATCH_SIZE = 1000;

		Node performSingleCreate(int64_t labelId) const {
			Node newNode(0, labelId);
			dm_->addNode(newNode);
			if (!props_.empty()) {
				dm_->addNodeProperties(newNode.getId(), props_);
			}
			newNode.setProperties(props_);
			return newNode;
		}

		std::optional<RecordBatch> flushBuffer() {
			if (nodeBuffer_.empty()) {
				return std::nullopt;
			}

			// 1. Bulk Insert into Storage
			// Nodes already have labelId set from constructor in next()
			dm_->addNodes(nodeBuffer_);

			// 2. Map back to Records
			RecordBatch outputBatch;
			outputBatch.reserve(nodeBuffer_.size());

			for (size_t i = 0; i < nodeBuffer_.size(); ++i) {
				Record r = std::move(recordBuffer_[i]);
				r.setNode(variable_, nodeBuffer_[i]);
				outputBatch.push_back(std::move(r));
			}

			nodeBuffer_.clear();
			recordBuffer_.clear();

			return outputBatch;
		}
	};
} // namespace graph::query::execution::operators
