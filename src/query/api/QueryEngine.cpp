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
#include "graph/query/QueryGuard.hpp"
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

	QueryPlan QueryEngine::buildPlan(const std::string &query, const Language lang) {
		// Try plan cache first
		auto cached = planCache_.get(query);
		if (cached) {
			return std::move(*cached);
		}

		// Cache miss: parse to logical plan
		auto parser = getParser(lang);
		auto plan = parser->parseToLogical(query);

		if (plan.root) {
			// DDL operations invalidate the cache (schema changes affect plans)
			if (plan.mutatesSchema) {
				planCache_.clear();
			}

			if (plan.cacheable) {
				planCache_.put(query, plan);
			}
		}

		return plan;
	}

	QueryResult QueryEngine::executePlan(QueryPlan plan, const QueryContext &ctx) {
		using Clock = std::chrono::steady_clock;

		// ExecMode access control checks
		if (ctx.execMode == ExecMode::EM_READ_ONLY && (plan.mutatesData || plan.mutatesSchema)) {
			throw std::runtime_error("Read-only transaction cannot execute write queries");
		}
		if (ctx.execMode == ExecMode::EM_READ_WRITE && plan.mutatesSchema) {
			throw std::runtime_error("Read-write transaction cannot execute schema-modifying queries");
		}

		if (!plan.root) {
			return QueryResult{};
		}

		// Convert logical to physical
		auto dm = storage_->getDataManager();
		PhysicalPlanConverter converter(dm, indexManager_, constraintManager_,
		                                queryPlanner_->getProjectionManager(),
		                                planCache_.hits(), planCache_.misses());
		auto planTree = converter.convert(plan.root.get());

		if (!planTree) {
			return QueryResult{};
		}

		// Create QueryGuard from config
		int64_t timeoutMs = 30000;
		int64_t memoryMb = 0;
		int maxVarLengthDepth = 50;
		if (configManager_) {
			timeoutMs = configManager_->getQueryTimeoutMs();
			memoryMb = configManager_->getQueryMaxMemoryMb();
			maxVarLengthDepth = static_cast<int>(configManager_->getQueryMaxVarLengthDepth());
		}
		QueryGuard guard(timeoutMs, memoryMb);

		// Build enriched context with guard
		QueryContext enrichedCtx = ctx;
		enrichedCtx.guard = &guard;
		enrichedCtx.maxVarLengthDepth = maxVarLengthDepth;

		// Propagate thread pool to all operators in the tree
		if (threadPool_)
			propagateThreadPool(planTree.get(), threadPool_);

		// Propagate query context (parameters + guard) to operators
		propagateQueryContext(planTree.get(), &enrichedCtx);

		// Execute
		return queryExecutor_->execute(std::move(planTree));
	}

	QueryResult QueryEngine::execute(const std::string &query, const QueryContext &ctx, const Language lang) {
		using Clock = std::chrono::steady_clock;

		auto parseStart = Clock::now();

		auto plan = buildPlan(query, lang);

		debug::PerfTrace::addDuration(
				"parse", static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() -
															  parseStart)
													 .count()));

		return executePlan(std::move(plan), ctx);
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
