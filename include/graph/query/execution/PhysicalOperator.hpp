/**
 * @file PhysicalOperator.hpp
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

#include <memory>
#include <optional>
#include <utility>
#include <string>
#include <vector>
#include "Record.hpp"

namespace graph::concurrent {
	class ThreadPool;
}

namespace graph::query {
	struct QueryContext;
}

namespace graph::query::execution {

	class PhysicalOperator {
	public:
		using ExplainAttribute = std::pair<std::string, std::string>;

		virtual ~PhysicalOperator() = default;

		static constexpr size_t DEFAULT_BATCH_SIZE = 1000;

		/**
		 * @brief Prepares the operator for execution.
		 * Allocates resources, opens files, or initializes iterators.
		 */
		virtual void open() = 0;

		/**
		 * @brief Pulls the next batch of records from this operator.
		 *
		 * @return std::optional<RecordBatch> containing data, or std::nullopt if the stream is exhausted.
		 */
		virtual std::optional<RecordBatch> next() = 0;

		/**
		 * @brief Cleans up resources.
		 */
		virtual void close() = 0;

		/**
		 * @brief Returns the schema (variables) produced by this operator.
		 */
		[[nodiscard]] virtual std::vector<std::string> getOutputVariables() const = 0;

		/**
		 * @brief Returns a short description of this operator.
		 * e.g., "NodeScan(n:User)" or "Filter(age > 10)"
		 */
		[[nodiscard]] virtual std::string toString() const = 0;

		/**
		 * @brief Returns stable key/value details for explain/profile tooling.
		 */
		[[nodiscard]] virtual std::vector<ExplainAttribute> explainAttributes() const { return {}; }

		/**
		 * @brief Returns raw pointers to children operators.
		 * Used by the PlanVisualizer to traverse the tree.
		 * Default implementation returns empty (leaf node).
		 */
		[[nodiscard]] virtual std::vector<const PhysicalOperator *> getChildren() const { return {}; }

		/**
		 * @brief Provides a cardinality-preserving upper-bound hint from downstream LIMIT operators.
		 *
		 * This is an execution hint, not a semantic contract: only operators that preserve row cardinality
		 * should propagate it to children, and source operators may use it to cap work early.
		 */
		virtual void setOutputLimitHint(size_t /*limit*/) {}

		/**
		 * @brief Sets the thread pool for parallel execution within operators.
		 * Propagated to child operators automatically.
		 */
		void setThreadPool(concurrent::ThreadPool *pool) { threadPool_ = pool; }
		[[nodiscard]] concurrent::ThreadPool *getThreadPool() const { return threadPool_; }

		void setQueryContext(const query::QueryContext *ctx) { queryContext_ = ctx; }
		[[nodiscard]] const query::QueryContext *getQueryContext() const { return queryContext_; }

		/**
		 * @brief Sets a child operator. Used to dynamically inject children
		 * (e.g., RecordInjector for FOREACH). Default implementation does nothing.
		 */
		virtual void setChild(std::unique_ptr<PhysicalOperator> /*child*/) {}

	protected:
		concurrent::ThreadPool *threadPool_ = nullptr;
		const query::QueryContext *queryContext_ = nullptr;
	};

} // namespace graph::query::execution
