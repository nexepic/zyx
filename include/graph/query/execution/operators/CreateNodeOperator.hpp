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
			// Case A: Pipeline (Chained to previous MATCH, UNWIND, or CREATE)
			if (child_) {
				// If we previously drained the child but still have buffered data to flush
				if (childExhausted_) {
					return flushBuffer();
				}

				while (true) {
					auto batchOpt = child_->next();

					// Upstream is exhausted
					if (!batchOpt) {
						childExhausted_ = true;
						// Flush any remaining nodes
						return flushBuffer();
					}

					// Process input batch
					RecordBatch &inputBatch = *batchOpt;

					// Optimization: If input is empty, continue fetching
					if (inputBatch.empty())
						continue;

					for (auto &record: inputBatch) {
						// Check if variable is already bound (Reuse existing)
						auto existingNode = record.getNode(variable_);

						if (existingNode) {
							if (!nodeBuffer_.empty()) {
								// Flush what we have so far to preserve order,
								// then return this single existing record in next call?
								// That's complex state management.

								// Let's just perform immediate flush + emit existing.
								// (This might return a batch smaller than standard size)
								auto flushedBatch = flushBuffer().value_or(RecordBatch{});
								flushedBatch.push_back(std::move(record));
								return flushedBatch;
							}

							// Buffer empty, just return this one record immediately (or small batch)
							RecordBatch singleBatch;
							singleBatch.push_back(std::move(record));
							return singleBatch;
						} else {
							// Variable unbound -> Queue for Batch Creation
							Node newNode(0, label_);

							// Pre-set properties in memory so addNodes() can persist them
							if (!props_.empty()) {
								// Note: addNodes() handles persistence, but we need to set them on the object
								newNode.setProperties(props_);
							}

							nodeBuffer_.push_back(std::move(newNode));
							recordBuffer_.push_back(std::move(record));
						}
					}

					// If buffer is large enough, flush and return
					if (nodeBuffer_.size() >= BATCH_SIZE) {
						return flushBuffer();
					}

					// Otherwise, loop again to fetch more from child
				}
			}

			// Case B: Source (Start of query, Single Create)
			if (executed_)
				return std::nullopt;

			Node newNode = performSingleCreate();
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

		[[nodiscard]] std::string toString() const override {
			return "CreateNode(var=" + variable_ + ", label=" + label_ + ")";
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

		// Legacy helper for single create
		Node performSingleCreate() {
			Node newNode(0, label_);
			dm_->addNode(newNode);
			if (!props_.empty()) {
				dm_->addNodeProperties(newNode.getId(), props_);
			}
			newNode.setProperties(props_);
			return newNode;
		}

		// Helper to flush buffered nodes to storage and return a record batch
		std::optional<RecordBatch> flushBuffer() {
			if (nodeBuffer_.empty()) {
				return std::nullopt;
			}

			// 1. Bulk Insert into Storage
			// This assigns IDs and handles persistence efficiently
			dm_->addNodes(nodeBuffer_);

			// 2. Map back to Records
			RecordBatch outputBatch;
			outputBatch.reserve(nodeBuffer_.size());

			for (size_t i = 0; i < nodeBuffer_.size(); ++i) {
				// Take the context record
				Record r = std::move(recordBuffer_[i]);

				// Bind the newly created (and ID-assigned) node
				r.setNode(variable_, nodeBuffer_[i]);

				outputBatch.push_back(std::move(r));
			}

			// 3. Cleanup
			nodeBuffer_.clear();
			recordBuffer_.clear();

			return outputBatch;
		}
	};
} // namespace graph::query::execution::operators
