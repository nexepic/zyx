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
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>
#include <unordered_set>
#include <vector>
#include "CypherParserImpl.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/query/logical/LogicalOperator.hpp"

namespace graph::query {

	namespace {
		[[nodiscard]] bool isLogicalExplainPlan(const logical::LogicalOperator &root) {
			return root.getType() == logical::LogicalOpType::LOP_EXPLAIN;
		}

		[[nodiscard]] bool isLogicalProfilePlan(const logical::LogicalOperator &root) {
			return root.getType() == logical::LogicalOpType::LOP_PROFILE;
		}

		[[nodiscard]] const logical::LogicalOperator &requireObservabilityInner(
				const logical::LogicalOperator &root, logical::LogicalOpType expectedType) {
			if (root.getType() != expectedType) {
				throw std::logic_error("logical observability dispatcher received an unexpected operator");
			}
			const std::vector<logical::LogicalOperator *> children = root.getChildren();
			if (children.size() != 1U || children.front() == nullptr) {
				throw std::logic_error("logical EXPLAIN/PROFILE requires exactly one complete inner plan");
			}
			return *children.front();
		}

		[[nodiscard]] QueryResult explainLogicalPlan(const logical::LogicalOperator &root, size_t maximumRows,
													 const QueryContext &context) {
			if (maximumRows == 0U) {
				throw std::invalid_argument("logical EXPLAIN requires a positive plan-row bound");
			}
			QueryResult result;
			result.setColumns({"operator", "details"});

			struct PendingOperator {
				const logical::LogicalOperator *op = nullptr;
				size_t depth = 0U;
			};
			std::vector<PendingOperator> pending;
			pending.push_back({&root, 0U});
			std::unordered_set<const logical::LogicalOperator *> visited;
			visited.reserve(std::min(maximumRows, size_t{1024U}));
			constexpr size_t MAX_INDENT_DEPTH = 128U;

			while (!pending.empty()) {
				context.checkGuard();
				const PendingOperator current = pending.back();
				pending.pop_back();
				if (current.op == nullptr || !visited.insert(current.op).second) {
					throw std::logic_error("logical EXPLAIN received a cyclic or malformed plan tree");
				}
				if (result.rowCount() == maximumRows) {
					throw std::runtime_error("logical EXPLAIN exceeded its plan-row bound");
				}

				const size_t renderedDepth = std::min(current.depth, MAX_INDENT_DEPTH);
				std::string operatorName(renderedDepth * 2U, ' ');
				if (current.depth > MAX_INDENT_DEPTH) {
					operatorName += "... ";
				}
				operatorName += logical::toString(current.op->getType());
				result.addRow({{"operator", PropertyValue(std::move(operatorName))},
						   {"details", PropertyValue(current.op->toString())}});

				const std::vector<logical::LogicalOperator *> children = current.op->getChildren();
				if (current.depth == std::numeric_limits<size_t>::max()) {
					throw std::overflow_error("logical EXPLAIN plan depth overflowed");
				}
				for (auto child = children.rbegin(); child != children.rend(); ++child) {
					if (*child == nullptr) {
						throw std::logic_error("logical EXPLAIN received a plan with a null child");
					}
					if (pending.size() == maximumRows - visited.size()) {
						throw std::runtime_error("logical EXPLAIN exceeded its pending-plan bound");
					}
					pending.push_back({*child, current.depth + 1U});
				}
			}
			return result;
		}

		[[nodiscard]] QueryResult addLogicalProfileSummary(QueryResult result,
													 const std::chrono::steady_clock::time_point started,
													 const QueryContext &context) {
			context.checkGuard();
			const double elapsedMs = std::chrono::duration<double, std::milli>(
														std::chrono::steady_clock::now() - started)
														 .count();
			if (!std::isfinite(elapsedMs) || elapsedMs < 0.0) {
				throw std::runtime_error("logical PROFILE recorded an invalid elapsed duration");
			}
			result.setDuration(elapsedMs);
			// Match the legacy PROFILE shape: regular result rows remain intact and
			// the final row carries the execution summary.
			result.addRow({{"phase", PropertyValue("logical_plan")}, {"total_time_ms", PropertyValue(elapsedMs)},
					   {"calls", PropertyValue(int64_t{1})}});
			return result;
		}

		[[nodiscard]] bool isLogicalProcedurePlan(const logical::LogicalOperator &root) {
			const logical::LogicalOperator *current = &root;
			while (current->getType() == logical::LogicalOpType::LOP_PROJECT ||
				   current->getType() == logical::LogicalOpType::LOP_FILTER ||
				   current->getType() == logical::LogicalOpType::LOP_SKIP ||
				   current->getType() == logical::LogicalOpType::LOP_LIMIT ||
				   current->getType() == logical::LogicalOpType::LOP_SORT) {
				const std::vector<logical::LogicalOperator *> children = current->getChildren();
				if (children.size() != 1U || children.front() == nullptr) {
					return false;
				}
				current = children.front();
			}
			return current->getType() == logical::LogicalOpType::LOP_CALL_PROCEDURE;
		}

	} // namespace

	QueryEngine::QueryEngine(std::shared_ptr<storage::LogicalGraphReadBackend> logicalBackend,
							 LogicalReadExecutionOptions options) :
		logicalBackend_(std::move(logicalBackend)), logicalReadOptions_(std::move(options)) {
		if (!logicalBackend_) {
			throw std::invalid_argument("logical query engine requires a read backend");
		}
		logicalSchemaBackend_ = std::dynamic_pointer_cast<storage::LogicalGraphSchemaBackend>(logicalBackend_);
		logicalVectorSearchBackend_ =
				std::dynamic_pointer_cast<storage::LogicalGraphVectorSearchBackend>(logicalBackend_);
		logicalVectorMaintenanceBackend_ =
				std::dynamic_pointer_cast<storage::LogicalGraphVectorMaintenanceBackend>(logicalBackend_);
	}

	QueryEngine::QueryEngine(std::shared_ptr<storage::LogicalGraphReadBackend> logicalReadBackend,
							 std::shared_ptr<storage::LogicalGraphWriteBackend> logicalWriteBackend,
							 LogicalReadExecutionOptions options) :
		logicalBackend_(std::move(logicalReadBackend)), logicalWriteBackend_(std::move(logicalWriteBackend)),
		logicalReadOptions_(std::move(options)) {
		if (!logicalBackend_ || !logicalWriteBackend_) {
			throw std::invalid_argument("logical read-write query engine requires both logical backend boundaries");
		}
		logicalSchemaBackend_ = std::dynamic_pointer_cast<storage::LogicalGraphSchemaBackend>(logicalBackend_);
		logicalVectorSearchBackend_ =
				std::dynamic_pointer_cast<storage::LogicalGraphVectorSearchBackend>(logicalBackend_);
		logicalVectorMaintenanceBackend_ =
				std::dynamic_pointer_cast<storage::LogicalGraphVectorMaintenanceBackend>(logicalBackend_);
	}

	QueryEngine::~QueryEngine() = default;

	std::shared_ptr<parser::IQueryParser> QueryEngine::getParser(Language lang) {
		if (!parsers_.contains(lang)) {
			if (lang == Language::Cypher) {
				parsers_[lang] = std::make_shared<parser::cypher::CypherParserImpl>();
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
		if (!plan.root) {
			return QueryResult{};
		}
		const bool explainPlan = isLogicalExplainPlan(*plan.root);
		const bool profilePlan = isLogicalProfilePlan(*plan.root);

		// EXPLAIN never opens a snapshot or publishes work, including when the
		// inspected inner plan would otherwise mutate data or schema.
		if (ctx.execMode == ExecMode::EM_READ_ONLY && (plan.mutatesData || plan.mutatesSchema) && !explainPlan) {
			throw std::runtime_error("Read-only transaction cannot execute write queries");
		}
		if (ctx.execMode == ExecMode::EM_READ_WRITE && plan.mutatesSchema && !explainPlan) {
			throw std::runtime_error("Read-write transaction cannot execute schema-modifying queries");
		}

		if (logicalBackend_) {
			QueryGuard defaultGuard(30000, 0);
			QueryContext logicalContext = ctx;
			if (logicalContext.guard == nullptr) {
				logicalContext.guard = &defaultGuard;
			}
			if (!logicalContext.csv_import_policy.import_root.has_value()) {
				logicalContext.csv_import_policy.import_root = logicalReadOptions_.csv_import_root;
			}
			if (explainPlan) {
				return explainLogicalPlan(
						requireObservabilityInner(*plan.root, logical::LogicalOpType::LOP_EXPLAIN),
						logicalReadOptions_.bounds.maximum_rows, logicalContext);
			}

			const logical::LogicalOperator &executionRoot =
					profilePlan ? requireObservabilityInner(*plan.root, logical::LogicalOpType::LOP_PROFILE) : *plan.root;
			const auto executeLogicalRoot = [&]() -> QueryResult {
				const bool isLogicalSchemaRead =
						executionRoot.getType() == logical::LogicalOpType::LOP_SHOW_INDEXES ||
						executionRoot.getType() == logical::LogicalOpType::LOP_SHOW_CONSTRAINTS;
				if (plan.mutatesSchema || isLogicalSchemaRead) {
					if (!logicalSchemaBackend_) {
						throw std::runtime_error("logical backend does not support schema mutations");
					}
					execution::LogicalPlanSchemaRuntime runtime(*logicalSchemaBackend_);
					return runtime.execute(executionRoot, logicalReadOptions_.bounds.maximum_rows, logicalContext);
				}
				// Legacy planning marks some procedures as data mutations. Dispatch
				// them before the mutation runtime so unsupported procedures fail at
				// the logical procedure boundary instead of being misclassified as CREATE.
				if (isLogicalProcedurePlan(executionRoot)) {
					execution::LogicalPlanProcedureRuntime runtime(*logicalBackend_, logicalVectorSearchBackend_.get(),
																						 logicalVectorMaintenanceBackend_.get());
					return runtime.execute(executionRoot, logicalReadOptions_.bounds,
										   logicalReadOptions_.maximum_vector_mapping_entries,
										   logicalReadOptions_.maximum_raw_vector_reads,
										   logicalReadOptions_.vector_candidate_search_budget,
										   logicalReadOptions_.vector_candidate_refinement_budget,
										   logicalReadOptions_.maximum_vector_training_records,
										   logicalReadOptions_.maximum_vector_training_pages, logicalContext);
				}
				if (plan.mutatesData) {
					if (!logicalWriteBackend_) {
						throw std::runtime_error("logical read backend does not support data mutations");
					}
					execution::LogicalPlanMutationRuntime runtime(*logicalBackend_, *logicalWriteBackend_);
					return runtime.execute(executionRoot, logicalReadOptions_.bounds, logicalContext);
				}
				execution::LogicalPlanReadRuntime runtime(*logicalBackend_);
				return runtime.executeToQueryResult(executionRoot, logicalReadOptions_.bounds, logicalContext);
			};
			if (!profilePlan) {
				return executeLogicalRoot();
			}
			const auto started = std::chrono::steady_clock::now();
			return addLogicalProfileSummary(executeLogicalRoot(), started, logicalContext);
		}

		throw std::logic_error("native query engine has no logical storage backend");
	}

	QueryResult QueryEngine::executePlanWithLogicalTransactionOverlay(
			QueryPlan plan, const QueryContext &ctx, storage::LogicalGraphReadBackend &readBackend,
			storage::LogicalGraphWriteBackend &writeBackend) {
		if (!plan.root) {
			return QueryResult{};
		}
		const bool explainPlan = isLogicalExplainPlan(*plan.root);
		const bool profilePlan = isLogicalProfilePlan(*plan.root);
		if (ctx.execMode == ExecMode::EM_READ_ONLY && (plan.mutatesData || plan.mutatesSchema) && !explainPlan) {
			throw std::runtime_error("Read-only transaction cannot execute write queries");
		}
		if (ctx.execMode == ExecMode::EM_READ_WRITE && plan.mutatesSchema && !explainPlan) {
			throw std::runtime_error("Read-write transaction cannot execute schema-modifying queries");
		}
		if (!logicalBackend_) {
			throw std::logic_error("logical transaction overlay requires a logical query engine");
		}
		if (plan.mutatesSchema && !explainPlan) {
			throw std::runtime_error("logical transaction overlays do not support schema mutations");
		}

		QueryGuard defaultGuard(30000, 0);
		QueryContext logicalContext = ctx;
		if (logicalContext.guard == nullptr) {
			logicalContext.guard = &defaultGuard;
		}
		if (!logicalContext.csv_import_policy.import_root.has_value()) {
			logicalContext.csv_import_policy.import_root = logicalReadOptions_.csv_import_root;
		}
		if (explainPlan) {
			return explainLogicalPlan(requireObservabilityInner(*plan.root, logical::LogicalOpType::LOP_EXPLAIN),
								  logicalReadOptions_.bounds.maximum_rows, logicalContext);
		}
		const logical::LogicalOperator &executionRoot =
				profilePlan ? requireObservabilityInner(*plan.root, logical::LogicalOpType::LOP_PROFILE) : *plan.root;
		const auto executeOverlayRoot = [&]() -> QueryResult {
			if (isLogicalProcedurePlan(executionRoot)) {
				throw std::runtime_error("logical transaction overlays do not support stored procedures");
			}
			if (plan.mutatesData) {
				execution::LogicalPlanMutationRuntime runtime(readBackend, writeBackend);
				return runtime.execute(executionRoot, logicalReadOptions_.bounds, logicalContext);
			}
			execution::LogicalPlanReadRuntime runtime(readBackend);
			return runtime.executeToQueryResult(executionRoot, logicalReadOptions_.bounds, logicalContext);
		};
		if (!profilePlan) {
			return executeOverlayRoot();
		}
		const auto started = std::chrono::steady_clock::now();
		return addLogicalProfileSummary(executeOverlayRoot(), started, logicalContext);
	}

	QueryResult QueryEngine::execute(const std::string &query, const QueryContext &ctx, const Language lang) {
		using Clock = std::chrono::steady_clock;

		auto parseStart = Clock::now();

		auto plan = buildPlan(query, lang);

		debug::PerfTrace::addDuration(
				"parse",
				static_cast<uint64_t>(
						std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - parseStart).count()));

		return executePlan(std::move(plan), ctx);
	}

} // namespace graph::query
