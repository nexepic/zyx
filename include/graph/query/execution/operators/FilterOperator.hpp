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

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>
#include "../PhysicalOperator.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/debug/PerfTrace.hpp"

namespace graph::query::execution::operators {

	class FilterOperator : public PhysicalOperator {
	public:
		using Predicate = std::function<bool(const Record &)>;

		static constexpr size_t PARALLEL_FILTER_THRESHOLD = 4096;

		/**
		 * @brief Constructs a FilterOperator.
		 *
		 * @param child The upstream operator.
		 * @param predicate The generic lambda function for filtering.
		 * @param predicateStr A string representation of the logic (for debugging/visualization).
		 */
		FilterOperator(std::unique_ptr<PhysicalOperator> child, Predicate predicate, std::string predicateStr) :
			child_(std::move(child)), predicate_(std::move(predicate)), predicateStr_(std::move(predicateStr)) {}

		void open() override {
			if (child_)
				child_->open();
		}

		std::optional<RecordBatch> next() override {
			while (true) {
				auto batchOpt = child_->next();
				if (!batchOpt)
					return std::nullopt;

				RecordBatch &inputBatch = *batchOpt;

				using Clock = std::chrono::steady_clock;
				auto filterStart = Clock::now();

				RecordBatch outputBatch;

				if (threadPool_ && !threadPool_->isSingleThreaded()
					&& inputBatch.size() >= PARALLEL_FILTER_THRESHOLD) {
					// Parallel two-pass: evaluate predicates then compact
					std::vector<uint8_t> keep(inputBatch.size());
					threadPool_->parallelFor(0, inputBatch.size(), [&](size_t i) {
						keep[i] = predicate_(inputBatch[i]) ? 1 : 0;
					});
					outputBatch.reserve(inputBatch.size());
					for (size_t i = 0; i < inputBatch.size(); ++i) {
						if (keep[i])
							outputBatch.push_back(std::move(inputBatch[i]));
					}
				} else {
					// Sequential path
					outputBatch.reserve(inputBatch.size());
					for (auto &record : inputBatch) {
						if (predicate_(record))
							outputBatch.push_back(std::move(record));
					}
				}

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
		std::string predicateStr_;
	};

} // namespace graph::query::execution::operators
