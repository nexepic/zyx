/**
 * @file FilterOperator.hpp
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

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/concurrent/ParallelOperatorExecutor.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/expressions/Expression.hpp"

namespace graph::storage { class DataManager; }
namespace graph::query { struct QueryContext; }

namespace graph::query::execution::operators {

	class FilterOperator : public PhysicalOperator {
	public:
		using Predicate = std::function<bool(const Record &)>;

		static constexpr size_t PARALLEL_FILTER_THRESHOLD = 4096;

		/**
		 * @brief Constructs a FilterOperator with a predicate lambda.
		 */
		FilterOperator(std::unique_ptr<PhysicalOperator> child, Predicate predicate, std::string predicateStr) :
			child_(std::move(child)), predicate_(std::move(predicate)), predicateStr_(std::move(predicateStr)) {}

		/**
		 * @brief Constructs a FilterOperator with an expression AST (supports parameters).
		 */
		FilterOperator(std::unique_ptr<PhysicalOperator> child,
		               std::shared_ptr<expressions::Expression> expr,
		               storage::DataManager *dm,
		               std::string predicateStr) :
			child_(std::move(child)), filterExpr_(std::move(expr)),
			dataManager_(dm), predicateStr_(std::move(predicateStr)) {}

		void open() override {
			if (child_)
				child_->open();
		}

		std::optional<RecordBatch> next() override {
			while (true) {
				if (queryContext_) queryContext_->checkGuard();
				auto batchOpt = child_->next();
				if (!batchOpt)
					return std::nullopt;

				RecordBatch &inputBatch = *batchOpt;

				using Clock = std::chrono::steady_clock;
				auto filterStart = Clock::now();

				RecordBatch outputBatch;

				struct FilterPartitionState {
					RecordBatch records;
				};
				const graph::concurrent::ParallelOperatorOptions options{
						.phase = "filter.parallel",
						.workloadKind = graph::concurrent::ParallelWorkloadKind::PWK_CPU_BOUND,
						.estimatedItems = inputBatch.size(),
						.minPartitions = 2,
						.minItems = PARALLEL_FILTER_THRESHOLD,
						.minItemsPerWorker = std::max<size_t>(1, PARALLEL_FILTER_THRESHOLD / 4)};
				outputBatch.reserve(inputBatch.size());
				(void) graph::concurrent::ParallelOperatorExecutor::runRangePartitions<FilterPartitionState>(
						0,
						inputBatch.size(),
						threadPool_,
						options,
						[&](const graph::concurrent::ParallelRangePartition &range, FilterPartitionState &state) {
							state.records.reserve(range.size());
							for (size_t i = range.begin; i < range.end; ++i) {
								if (evaluateRecord(inputBatch[i])) {
									state.records.push_back(std::move(inputBatch[i]));
								}
							}
						},
						[&](size_t, FilterPartitionState &state) {
							for (auto &record: state.records) {
								outputBatch.push_back(std::move(record));
							}
						});

				debug::PerfTrace::addDuration(
						"filter",
						static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
													filterStart)
												 .count()));

				if (!outputBatch.empty())
					return outputBatch;
			}
		}

		void close() override {
			if (child_)
				child_->close();
		}

		[[nodiscard]] std::vector<std::string> getOutputVariables() const override {
			return child_ ? child_->getOutputVariables() : std::vector<std::string>{};
		}

		// --- Visualization ---
		[[nodiscard]] std::string toString() const override;

		[[nodiscard]] std::vector<const PhysicalOperator *> getChildren() const override {
			if (child_)
				return {child_.get()};
			return {};
		}

	private:
		std::unique_ptr<PhysicalOperator> child_;
		Predicate predicate_;
		std::shared_ptr<expressions::Expression> filterExpr_;
		storage::DataManager *dataManager_ = nullptr;
		std::string predicateStr_;

		bool evaluateRecord(const Record &record) const;
	};

} // namespace graph::query::execution::operators
