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
#include <chrono>
#include "CypherParserImpl.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/planner/PhysicalPlanConverter.hpp"
#include "graph/query/planner/QueryPlanner.hpp"
#include "graph/storage/constraints/ConstraintManager.hpp"
#include "graph/storage/indexes/IndexManager.hpp"

namespace graph::query {

	QueryEngine::QueryEngine(std::shared_ptr<storage::FileStorage> storage) : storage_(std::move(storage)) {

		auto dm = storage_->getDataManager();
		indexManager_ = std::make_shared<indexes::IndexManager>(storage_);
		indexManager_->initialize();
		dm->registerObserver(indexManager_);

		// Initialize ConstraintManager (pre-write validation)
		constraintManager_ = std::make_shared<storage::constraints::ConstraintManager>(storage_, indexManager_);
		constraintManager_->initialize();
		dm->registerValidator(constraintManager_);

		// Note: queryPlanner_ is now shared_ptr for the Builder
		queryPlanner_ = std::make_shared<QueryPlanner>(dm, indexManager_, constraintManager_);
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
		return execute(query, QueryContext{}, lang);
	}

	QueryResult QueryEngine::execute(const std::string &query, const QueryContext &ctx, const Language lang) {
		using Clock = std::chrono::steady_clock;

		auto parseStart = Clock::now();

		std::unique_ptr<execution::PhysicalOperator> planTree;

		// Try plan cache first
		auto cachedLogical = planCache_.get(query);
		if (cachedLogical) {
			// Cache hit: convert cached logical plan to physical
			auto dm = storage_->getDataManager();
			PhysicalPlanConverter converter(dm, indexManager_, constraintManager_,
			                                queryPlanner_->getProjectionManager(),
			                                planCache_.hits(), planCache_.misses());
			planTree = converter.convert(cachedLogical.get());
		} else {
			// Cache miss: parse to logical, cache if cacheable, then convert
			auto parser = getParser(lang);
			auto logicalPlan = parser->parseToLogical(query);

			if (logicalPlan) {
				// Determine if the plan is cacheable (skip DDL/transaction control)
				auto rootType = logicalPlan->getType();
				bool isCacheable = (rootType != logical::LogicalOpType::LOP_TRANSACTION_CONTROL &&
				                    rootType != logical::LogicalOpType::LOP_CREATE_INDEX &&
				                    rootType != logical::LogicalOpType::LOP_DROP_INDEX &&
				                    rootType != logical::LogicalOpType::LOP_SHOW_INDEXES &&
				                    rootType != logical::LogicalOpType::LOP_CREATE_VECTOR_INDEX &&
				                    rootType != logical::LogicalOpType::LOP_CREATE_CONSTRAINT &&
				                    rootType != logical::LogicalOpType::LOP_DROP_CONSTRAINT &&
				                    rootType != logical::LogicalOpType::LOP_SHOW_CONSTRAINTS &&
				                    rootType != logical::LogicalOpType::LOP_EXPLAIN &&
				                    rootType != logical::LogicalOpType::LOP_PROFILE);

				// DDL operations invalidate the cache (schema changes affect plans)
				bool isDDL = (rootType == logical::LogicalOpType::LOP_CREATE_INDEX ||
				              rootType == logical::LogicalOpType::LOP_DROP_INDEX ||
				              rootType == logical::LogicalOpType::LOP_CREATE_VECTOR_INDEX ||
				              rootType == logical::LogicalOpType::LOP_CREATE_CONSTRAINT ||
				              rootType == logical::LogicalOpType::LOP_DROP_CONSTRAINT);
				if (isDDL) {
					planCache_.clear();
				}

				if (isCacheable) {
					planCache_.put(query, logicalPlan.get());
				}

				// Convert logical to physical
				auto dm = storage_->getDataManager();
				PhysicalPlanConverter converter(dm, indexManager_, constraintManager_,
				                                queryPlanner_->getProjectionManager(),
				                                planCache_.hits(), planCache_.misses());
				planTree = converter.convert(logicalPlan.get());
			}
		}

		debug::PerfTrace::addDuration(
				"parse", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
															  parseStart)
													 .count()));

		if (!planTree) {
			return QueryResult{};
		}

		// Propagate thread pool to all operators in the tree
		if (threadPool_)
			propagateThreadPool(planTree.get(), threadPool_);

		// Propagate query context (parameters) to operators
		if (!ctx.parameters.empty())
			propagateQueryContext(planTree.get(), &ctx);

		// Execute
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
		for (auto *child : op->getChildren()) {
			propagateThreadPool(const_cast<execution::PhysicalOperator *>(child), pool);
		}
	}

	void QueryEngine::propagateQueryContext(execution::PhysicalOperator *op, const QueryContext *ctx) {
		if (!op)
			return;
		op->setQueryContext(ctx);
		for (auto *child : op->getChildren()) {
			propagateQueryContext(const_cast<execution::PhysicalOperator *>(child), ctx);
		}
	}

} // namespace graph::query
