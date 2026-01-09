/**
 * @file CartesianProductOperator.hpp
 * @author Nexepic
 * @date 2025/12/23
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

namespace graph::query::execution::operators {

	class CartesianProductOperator : public PhysicalOperator {
	public:
		CartesianProductOperator(std::unique_ptr<PhysicalOperator> left, std::unique_ptr<PhysicalOperator> right) :
			left_(std::move(left)), right_(std::move(right)) {}

		void open() override {
			if (left_)
				left_->open();
			if (right_)
				right_->open();

			// Materialize Right side (Blocking)
			rightBuffer_.clear();
			if (right_) {
				while (auto batch = right_->next()) {
					rightBuffer_.insert(rightBuffer_.end(), std::make_move_iterator(batch->begin()),
										std::make_move_iterator(batch->end()));
				}
			}

			currentLeftBatch_ = std::nullopt;
			leftIdx_ = 0;
			rightIdx_ = 0;
		}

		std::optional<RecordBatch> next() override {
			if (rightBuffer_.empty()) {
				return std::nullopt;
			}

			RecordBatch outputBatch;
			outputBatch.reserve(DEFAULT_BATCH_SIZE);

			while (outputBatch.size() < DEFAULT_BATCH_SIZE) {
				// Need a new left batch?
				if (!currentLeftBatch_ || leftIdx_ >= currentLeftBatch_->size()) {
					if (!left_)
						return std::nullopt;

					currentLeftBatch_ = left_->next();

					if (!currentLeftBatch_) {
						if (!outputBatch.empty())
							return outputBatch;
						return std::nullopt;
					}

					leftIdx_ = 0;
					rightIdx_ = 0;
				}

				const auto &leftRecord = (*currentLeftBatch_)[leftIdx_];

				while (rightIdx_ < rightBuffer_.size() && outputBatch.size() < DEFAULT_BATCH_SIZE) {
					const auto &rightRecord = rightBuffer_[rightIdx_];

					Record merged = leftRecord;
					merged.merge(rightRecord);

					outputBatch.push_back(std::move(merged));
					rightIdx_++;
				}

				if (rightIdx_ >= rightBuffer_.size()) {
					leftIdx_++;
					rightIdx_ = 0;
				}
			}
			return outputBatch;
		}

		void close() override {
			if (left_)
				left_->close();
			if (right_)
				right_->close();
			rightBuffer_.clear();
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			auto vars = left_->getOutputVariables();
			auto rVars = right_->getOutputVariables();
			vars.insert(vars.end(), rVars.begin(), rVars.end());
			return vars;
		}

		[[nodiscard]] std::string toString() const override { return "CartesianProduct"; }

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			return {left_.get(), right_.get()};
		}

	private:
		std::unique_ptr<PhysicalOperator> left_;
		std::unique_ptr<PhysicalOperator> right_;

		std::vector<Record> rightBuffer_;
		std::optional<RecordBatch> currentLeftBatch_;
		size_t leftIdx_ = 0;
		size_t rightIdx_ = 0;
	};
} // namespace graph::query::execution::operators
