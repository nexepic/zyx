/**
 * @file UnwindOperator.hpp
 * @author Nexepic
 * @date 2025/12/27
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

#include <string>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/query/expressions/Expression.hpp"
#include "graph/query/expressions/ExpressionEvaluationHelper.hpp"

namespace graph::query::execution::operators {

	class UnwindOperator : public PhysicalOperator {
	public:
		// Literal list constructor (compile-time known list)
		UnwindOperator(std::unique_ptr<PhysicalOperator> child, std::string alias, std::vector<PropertyValue> list) :
			child_(std::move(child)), alias_(std::move(alias)), list_(std::move(list)) {}

		// Expression-based constructor (runtime evaluated list)
		UnwindOperator(std::unique_ptr<PhysicalOperator> child, std::string alias,
		               std::shared_ptr<graph::query::expressions::Expression> listExpr) :
			child_(std::move(child)), alias_(std::move(alias)), listExpr_(std::move(listExpr)) {}

		void open() override {
			if (child_)
				child_->open();

			// Reset state
			currentChildBatch_ = std::nullopt;
			childRecordIndex_ = 0;
			listIndex_ = 0;
			currentList_.clear();
		}

		std::optional<RecordBatch> next() override {
			RecordBatch outputBatch;
			outputBatch.reserve(DEFAULT_BATCH_SIZE);

			// ====================================================
			// CASE A: Source Mode (No upstream child)
			// UNWIND [1,2] AS x
			// ====================================================
			if (!child_) {
				// For expression-based UNWIND without child, evaluate against empty record
				if (listExpr_ && currentList_.empty() && listIndex_ == 0) {
					Record emptyRecord;
					PropertyValue result = graph::query::expressions::ExpressionEvaluationHelper::evaluate(
						listExpr_.get(), emptyRecord);
					if (result.getType() == PropertyType::LIST) {
						currentList_ = result.getList();
					}
				} else if (!listExpr_ && currentList_.empty() && listIndex_ == 0) {
					currentList_ = list_;
				}

				if (listIndex_ >= currentList_.size())
					return std::nullopt;

				while (outputBatch.size() < DEFAULT_BATCH_SIZE && listIndex_ < currentList_.size()) {
					Record r;
					r.setValue(alias_, currentList_[listIndex_++]);
					outputBatch.push_back(std::move(r));
				}
				return outputBatch;
			}

			// ====================================================
			// CASE B: Pipeline Mode (Has upstream child)
			// MATCH (n) UNWIND [1,2] AS x RETURN n, x
			// Logic: For each Input Record, emit N records (one for each list item)
			// ====================================================
			while (outputBatch.size() < DEFAULT_BATCH_SIZE) {

				// 1. Fetch next input batch if needed
				if (!currentChildBatch_ || childRecordIndex_ >= currentChildBatch_->size()) {
					currentChildBatch_ = child_->next();

					if (!currentChildBatch_) {
						// Upstream exhausted
						if (!outputBatch.empty())
							return outputBatch;
						return std::nullopt;
					}

					// Reset counters for the new batch
					childRecordIndex_ = 0;
					listIndex_ = 0;
					needsEval_ = true;
				}

				// 2. Process current input record
				const auto &inputRecord = (*currentChildBatch_)[childRecordIndex_];

				// 3. Evaluate expression per input record if expression-based
				if (needsEval_ || listIndex_ == 0) {
					if (listExpr_) {
						PropertyValue result = graph::query::expressions::ExpressionEvaluationHelper::evaluate(
							listExpr_.get(), inputRecord);
						if (result.getType() == PropertyType::LIST) {
							currentList_ = result.getList();
						} else {
							// Non-list expression: treat as single-element list
							currentList_ = {result};
						}
					} else if (currentList_.empty()) {
						currentList_ = list_;
					}
					needsEval_ = false;
				}

				// 4. Expand list for this record
				while (listIndex_ < currentList_.size() && outputBatch.size() < DEFAULT_BATCH_SIZE) {
					// Copy original record
					Record expandedRecord = inputRecord;

					// Add new variable
					expandedRecord.setValue(alias_, currentList_[listIndex_]);

					outputBatch.push_back(std::move(expandedRecord));
					listIndex_++;
				}

				// 5. Check if we finished the list for the current record
				if (listIndex_ >= currentList_.size()) {
					childRecordIndex_++; // Move to next input record
					listIndex_ = 0; // Reset list for the next record
					needsEval_ = true;
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
			vars.push_back(alias_);
			return vars;
		}

		[[nodiscard]] std::string toString() const override {
			if (listExpr_) {
				return "Unwind(" + alias_ + ", expr=" + listExpr_->toString() + ")";
			}
			return "Unwind(" + alias_ + ", size=" + std::to_string(list_.size()) + ")";
		}

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (child_)
				return {child_.get()};
			return {};
		}

	private:
		std::unique_ptr<PhysicalOperator> child_;
		std::string alias_;
		std::vector<PropertyValue> list_; // Compile-time literal list
		std::shared_ptr<graph::query::expressions::Expression> listExpr_; // Runtime expression

		// State Machine Variables
		std::optional<RecordBatch> currentChildBatch_;
		std::vector<PropertyValue> currentList_; // Resolved list for current record
		size_t childRecordIndex_ = 0; // Index in the input batch
		size_t listIndex_ = 0; // Index in the UNWIND list
		bool needsEval_ = true; // Whether expression needs re-evaluation
	};

} // namespace graph::query::execution::operators
