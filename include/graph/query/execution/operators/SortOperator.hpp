/**
 * @file SortOperator.hpp
 * @author Nexepic
 * @date 2025/12/22
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

#include <algorithm>
#include <future>
#include <string>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/expressions/Expression.hpp"

namespace graph::query::execution::operators {

	struct SortItem {
		std::shared_ptr<graph::query::expressions::Expression> expression; // Expression AST
		bool ascending = true;

		// Legacy constructor for backward compatibility
		SortItem([[maybe_unused]] std::string var, [[maybe_unused]] std::string prop, bool asc = true)
			: ascending(asc) {
			// This is a temporary bridge - text-based expressions will be replaced by AST
		}

		// New constructor for expression AST
		SortItem(std::shared_ptr<graph::query::expressions::Expression> expr, bool asc = true)
			: expression(std::move(expr)), ascending(asc) {}

		// Default constructor
		SortItem() = default;
	};

	class SortOperator : public PhysicalOperator {
	public:
		SortOperator(std::unique_ptr<PhysicalOperator> child, std::vector<SortItem> sortItems) :
			child_(std::move(child)), sortItems_(std::move(sortItems)) {}

		void open() override;
		std::optional<RecordBatch> next() override;
		void close() override;

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return child_->getOutputVariables();
		}

		[[nodiscard]] std::string toString() const override;

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override { return {child_.get()}; }

	private:
		std::unique_ptr<PhysicalOperator> child_;
		std::vector<SortItem> sortItems_;

		// Buffer
		std::vector<Record> sortedRecords_;
		size_t currentOutputIndex_ = 0;
		bool isSorted_ = false;
		static constexpr size_t BATCH_SIZE = 1000;

		void performSort() {
			auto comparator = [this](const Record &a, const Record &b) -> bool {
				for (const auto &item : sortItems_) {
					PropertyValue valA, valB;

					if (item.expression) {
						graph::query::expressions::EvaluationContext contextA(a);
						graph::query::expressions::ExpressionEvaluator evaluatorA(contextA);
						valA = evaluatorA.evaluate(item.expression.get());

						graph::query::expressions::EvaluationContext contextB(b);
						graph::query::expressions::ExpressionEvaluator evaluatorB(contextB);
						valB = evaluatorB.evaluate(item.expression.get());
					}

					if (valA != valB) {
						if (item.ascending)
							return valA < valB;
						else
							return valA > valB;
					}
				}
				return false;
			};

			// Parallel merge sort for large datasets
			static constexpr size_t PARALLEL_SORT_THRESHOLD = 10000;
			if (threadPool_ && !threadPool_->isSingleThreaded() &&
				sortedRecords_.size() >= PARALLEL_SORT_THRESHOLD) {
				parallelMergeSort(sortedRecords_, comparator);
			} else {
				std::sort(sortedRecords_.begin(), sortedRecords_.end(), comparator);
			}
		}

		template <typename Comp>
		void parallelMergeSort(std::vector<Record> &data, Comp comp) {
			size_t n = data.size();
			size_t numChunks = threadPool_->getThreadCount();
			size_t chunkSize = n / numChunks;

			// 1. Sort each chunk in parallel
			std::vector<std::future<void>> futures;
			futures.reserve(numChunks);

			for (size_t c = 0; c < numChunks; ++c) {
				size_t start = c * chunkSize;
				size_t end = (c == numChunks - 1) ? n : start + chunkSize;
				futures.push_back(threadPool_->submit([&data, start, end, &comp]() {
					std::sort(data.begin() + start, data.begin() + end, comp);
				}));
			}
			for (auto &f : futures)
				f.get();

			// 2. Iterative merge of sorted chunks
			std::vector<Record> buffer(n);
			for (size_t width = chunkSize; width < n; width *= 2) {
				for (size_t i = 0; i < n; i += 2 * width) {
					size_t mid = std::min(i + width, n);
					size_t right = std::min(i + 2 * width, n);
					std::merge(std::make_move_iterator(data.begin() + i),
							   std::make_move_iterator(data.begin() + mid),
							   std::make_move_iterator(data.begin() + mid),
							   std::make_move_iterator(data.begin() + right),
							   buffer.begin() + i, comp);
				}
				std::swap(data, buffer);
			}
		}
	};

} // namespace graph::query::execution::operators
