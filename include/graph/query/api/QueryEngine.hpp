/**
 * @file QueryEngine.hpp
 * @author Nexepic
 * @date 2025/3/20
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
#include <string>
#include "QueryBuilder.hpp"
#include "QueryResult.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/execution/QueryExecutor.hpp"
#include "graph/storage/FileStorage.hpp"
#include "query/parser/common/IQueryParser.hpp"

namespace graph::storage::constraints {
	class ConstraintManager;
}

namespace graph::query {

	enum class Language { Cypher };

	class QueryEngine {
	public:
		explicit QueryEngine(std::shared_ptr<storage::FileStorage> storage);
		~QueryEngine();

		// --- Main Execution ---
		QueryResult execute(const std::string &query, Language lang = Language::Cypher);

		/**
		 * @brief Executes a pre-built physical plan directly.
		 *        Useful for internal API calls or optimized paths bypassing the parser.
		 */
		[[nodiscard]] QueryResult execute(std::unique_ptr<execution::PhysicalOperator> plan) const;

		// Returns a builder to construct queries programmatically
		[[nodiscard]] QueryBuilder query() const { return QueryBuilder(queryPlanner_); }

		[[nodiscard]] std::shared_ptr<indexes::IndexManager> getIndexManager() const { return indexManager_; }
		[[nodiscard]] std::shared_ptr<storage::constraints::ConstraintManager> getConstraintManager() const { return constraintManager_; }

		void setThreadPool(concurrent::ThreadPool *pool) { threadPool_ = pool; }

	private:
		std::shared_ptr<storage::FileStorage> storage_;
		std::shared_ptr<indexes::IndexManager> indexManager_;
		std::shared_ptr<storage::constraints::ConstraintManager> constraintManager_;
		std::shared_ptr<QueryPlanner> queryPlanner_; // Shared so Builder can use it
		std::unique_ptr<QueryExecutor> queryExecutor_;

		std::unordered_map<Language, std::shared_ptr<parser::IQueryParser>> parsers_;
		concurrent::ThreadPool *threadPool_ = nullptr;

		std::shared_ptr<parser::IQueryParser> getParser(Language lang);

		static void propagateThreadPool(execution::PhysicalOperator *op, concurrent::ThreadPool *pool);
	};

} // namespace graph::query
