/**
 * @file QueryEngine.cpp
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

#include "graph/query/api/QueryEngine.hpp"
#include "CypherParserImpl.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query {

	QueryEngine::QueryEngine(std::shared_ptr<storage::FileStorage> storage) : storage_(std::move(storage)) {

		auto dm = storage_->getDataManager();
		indexManager_ = std::make_shared<indexes::IndexManager>(storage_);
		indexManager_->initialize();
		dm->registerObserver(indexManager_);

		// Note: queryPlanner_ is now shared_ptr for the Builder
		queryPlanner_ = std::make_shared<QueryPlanner>(dm, indexManager_);
		queryExecutor_ = std::make_unique<QueryExecutor>();
	}

	QueryEngine::~QueryEngine() = default;

	std::shared_ptr<parser::IQueryParser> QueryEngine::getParser(Language lang) {
		if (!parsers_.contains(lang)) {
			if (lang == Language::Cypher) {
				// Pass the planner to the parser so it can build trees
				parsers_[lang] = std::make_shared<parser::cypher::CypherParserImpl>(queryPlanner_);
			} else {
				throw std::runtime_error("Unsupported query language.");
			}
		}
		return parsers_[lang];
	}

	QueryResult QueryEngine::execute(const std::string &query, const Language lang) {
		// 1. Parse into Operator Tree
		auto planTree = getParser(lang)->parse(query);

		// Propagate thread pool to all operators in the tree
		if (threadPool_)
			propagateThreadPool(planTree.get(), threadPool_);

		// 2. Execute
		return queryExecutor_->execute(std::move(planTree));
	}

	QueryResult QueryEngine::execute(std::unique_ptr<execution::PhysicalOperator> plan) const {
		if (threadPool_)
			propagateThreadPool(plan.get(), threadPool_);
		return queryExecutor_->execute(std::move(plan));
	}

	void QueryEngine::propagateThreadPool(execution::PhysicalOperator *op, concurrent::ThreadPool *pool) {
		if (!op)
			return;
		op->setThreadPool(pool);
		// getChildren() returns const pointers; we need to traverse the mutable tree
		// Since operators own their children, we use const_cast for propagation only
		for (auto *child : op->getChildren()) {
			propagateThreadPool(const_cast<execution::PhysicalOperator *>(child), pool);
		}
	}

} // namespace graph::query
