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
#include "graph/core/PropertyHeapRequestBytes.hpp"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <exception>
#include <limits>
#include <unordered_set>
#include <vector>
#include "CypherParserImpl.hpp"
#include "graph/debug/PerfTrace.hpp"
#include "graph/query/QueryGuard.hpp"
#include "graph/query/execution/MaterializedByteBudget.hpp"
#include "graph/query/logical/LogicalOperator.hpp"
#include "graph/query/logical/operators/LogicalCallProcedure.hpp"
#include "graph/query/logical/operators/LogicalTraversal.hpp"
#include "graph/query/optimization/LogicalPlanOptimizer.hpp"
#include "graph/query/logical/operators/LogicalNodeScan.hpp"
#include "graph/query/logical/operators/LogicalProject.hpp"

namespace graph::query {

	namespace {
        template<class Action>
        decltype(auto) withQueryMemoryPolicy(const NativeQueryResourcePolicy &policy, Action &&action) {
            try { return std::forward<Action>(action)(); }
            catch (const execution::detail::ResourceAdmissionDenied &) {
                if (policy.maximum_memory_mb > 0)
                    throw QueryMemoryExceededException::forResourceAdmission(policy.maximum_memory_mb);
                throw;
            }
        }

		std::shared_ptr<execution::detail::ResourceAccount> validatedQueryResourceAccount(
            const std::shared_ptr<execution::detail::ResourceAccount> &account, const storage::LogicalGraphReadBackend &backend,
			size_t maximumBytes, const NativeQueryResourcePolicy &policy) {
			auto domain = backend.queryResourceDomain();
			if (!domain) return account;
			maximumBytes = (std::min)(maximumBytes, domain->maximum());
			if (policy.maximum_memory_mb > 0 && static_cast<uint64_t>(policy.maximum_memory_mb) <=
				std::numeric_limits<size_t>::max() / (1024U * 1024U))
				maximumBytes = (std::min)(maximumBytes, static_cast<size_t>(policy.maximum_memory_mb) * 1024U * 1024U);
			if (account) {
				if (!account->belongsTo(*domain) ||
                    account->resourceClass() != execution::detail::ResourceClass::RCK_QUERY ||
                    account->maximum() > maximumBytes)
					throw std::invalid_argument("query resource account requires QUERY ownership within its database policy");
			} else {
				return std::make_shared<execution::detail::ResourceAccount>(domain,
					execution::detail::ResourceClass::RCK_QUERY, maximumBytes);
			}
            return account;
		}
        // Keep this lease before the copied context in declaration order: all
        // standard-container payloads must be destroyed before their refund.
        [[nodiscard]] std::optional<execution::detail::ResourceLease> admitQueryParameterCopy(
                const QueryContext &context,
                const std::shared_ptr<execution::detail::ResourceAccount> &account) {
            if (!account) return std::nullopt;
            using namespace utils::detail;
            size_t bytes = standardUnorderedMapStructuralHeapBytes(context.parameters);
            for (const auto &[key, value] : context.parameters) {
                bytes = addStandardHeapBytes(bytes, standardFreshStringHeapUpper(key.size()));
                bytes = addStandardHeapBytes(bytes, property_heap::copyRequestUpper(value));
            }
            execution::detail::ResourceLease credit(account);
            credit.set(bytes);
            return credit;
        }
		[[nodiscard]] bool isLogicalExplainPlan(const logical::LogicalOperator &root) {
			return root.getType() == logical::LogicalOpType::LOP_EXPLAIN;
		}

		[[nodiscard]] bool isLogicalProfilePlan(const logical::LogicalOperator &root) {
			return root.getType() == logical::LogicalOpType::LOP_PROFILE;
		}

		class BackendLogicalStatisticsProvider final : public optimization::LogicalStatisticsProvider {
		public:
			explicit BackendLogicalStatisticsProvider(const storage::LogicalGraphStatisticsBackend &backend,
				const logical::LogicalOperator &root, bool allowAnchors) : backend_(backend) {
				try {
					std::vector<std::vector<std::string>> labels;
					collectJoinLabels(root, false, 0U, labels);
					if (allowAnchors) collectAnchorLabels(root, 0U, labels);
					auto batch = backend.readLogicalPlanningStatistics(labels);
					statistics_ = batch.graph;
					if (batch.node_counts.size() == labels.size()) {
						for (size_t i = 0U; i < labels.size(); ++i)
							label_estimates_.emplace_back(std::move(labels[i]), batch.node_counts[i]);
					}
				} catch (...) {
					// Statistics are optional: a malformed or unavailable record must not
					// trigger a planning-time graph scan.
					statistics_.reset();
				}
			}

			[[nodiscard]] std::optional<optimization::LogicalCardinalityEstimate>
			estimate(const logical::LogicalOperator &root) const noexcept override {
				return estimateCardinality(root, false);
			}
			[[nodiscard]] std::optional<optimization::LogicalCardinalityEstimate>
			estimateJoinInput(const logical::LogicalOperator &root) const noexcept override {
				return estimateCardinality(root, true);
			}
			[[nodiscard]] std::optional<uint64_t>
			estimateAnchorScan(const logical::LogicalOperator &root) const noexcept override {
				try {
					const auto *scan = dynamic_cast<const logical::LogicalNodeScan *>(&root);
					if (!statistics_ || !scan || scan->getLabels().size() > 1U ||
						!scan->getPropertyPredicates().empty() || !scan->getParameterPropertyPredicates().empty() ||
						!scan->getRangePredicates().empty() || scan->getCompositeEquality() ||
						(scan->getPreferredScanHint() != logical::LogicalScanHint::LSH_FULL &&
						 scan->getPreferredScanHint() != logical::LogicalScanHint::LSH_LABEL) ||
						(statistics_->node_count == 0U && statistics_->edge_count != 0U)) return std::nullopt;
					if (scan->getLabels().empty()) return statistics_->node_count;
					if (scan->getLabels().front().empty()) return std::nullopt;
					const auto count = estimateLabels(scan->getLabels());
					return count && *count <= statistics_->node_count ? count : std::nullopt;
				} catch (...) { return std::nullopt; }
			}
		private:
			[[nodiscard]] std::optional<optimization::LogicalCardinalityEstimate>
			estimateCardinality(const logical::LogicalOperator &root, bool refineLabels) const noexcept {
				try {
					if (!statistics_.has_value()) {
						return std::nullopt;
					}
					const storage::LogicalGraphCardinalityStatistics &statistics = *statistics_;
					const logical::LogicalOperator *current = &root;
					std::vector<const logical::LogicalTraversal *> traversals;
					for (size_t depth = 0U; depth < 64U; ++depth) {
						if (current->getType() == logical::LogicalOpType::LOP_NODE_SCAN) {
							// Labels and property predicates can only reduce a Node scan.
							// Optional label statistics refine costs; bounds remain conservative.
							optimization::LogicalCardinalityEstimate estimate{
									0U, statistics.node_count, statistics.node_count};
							const auto *scan = dynamic_cast<const logical::LogicalNodeScan *>(current);
							// Catalog label counts bound a constrained scan, but do not
							// estimate its property selectivity. Keep JOIN order when unknown.
							if (refineLabels && scan != nullptr &&
								(!scan->getPropertyPredicates().empty() || !scan->getParameterPropertyPredicates().empty() ||
								 !scan->getRangePredicates().empty() || scan->getCompositeEquality())) return std::nullopt;
							if (refineLabels && scan != nullptr && !scan->getLabels().empty()) {
								try {
									const auto labeled = estimateLabels(scan->getLabels());
									if (labeled && *labeled <= statistics.node_count) estimate.expected_rows = *labeled;
								} catch (...) {
									// Optional detailed statistics must retain the global fallback.
								}
							}

							if (statistics.node_count == 0U && statistics.edge_count != 0U) {
								// A published graph cannot expose an Edge without a Node. Do not
								// turn a malformed Catalog record into an optimizer decision.
								return std::nullopt;
							}
							for (auto traversal = traversals.rbegin(); traversal != traversals.rend(); ++traversal) {
								if (statistics.node_count == 0U) {
									estimate = {0U, 0U, 0U};
									continue;
								}
								const uint64_t averageAdjacency =
										statistics.edge_count / statistics.node_count +
										(static_cast<uint64_t>(statistics.edge_count % statistics.node_count != 0U));
								// The runtime deduplicates a `both` traversal by Edge ID, so each
								// input row has at most the global Edge count in every direction.
								estimate.expected_rows = saturatingMultiply(estimate.expected_rows, averageAdjacency);
								estimate.upper_bound = saturatingMultiply(estimate.upper_bound, statistics.edge_count);
							}
							return estimate;
						}
						switch (current->getType()) {
							case logical::LogicalOpType::LOP_TRAVERSAL: {
								const auto *traversal = dynamic_cast<const logical::LogicalTraversal *>(current);
								if (traversal == nullptr || (traversal->getDirection() != "out" &&
																	traversal->getDirection() != "in" &&
																	traversal->getDirection() != "both")) {
									return std::nullopt;
								}
								traversals.push_back(traversal);
								break;
							}
							case logical::LogicalOpType::LOP_FILTER:
							case logical::LogicalOpType::LOP_PROJECT:
							case logical::LogicalOpType::LOP_SORT:
							case logical::LogicalOpType::LOP_LIMIT:
							case logical::LogicalOpType::LOP_SKIP:
							case logical::LogicalOpType::LOP_EXPLAIN:
							case logical::LogicalOpType::LOP_PROFILE:
								break;
							default:
								return std::nullopt;
						}
						const std::vector<logical::LogicalOperator *> children = current->getChildren();
						if (children.size() != 1U || children.front() == nullptr) {
							return std::nullopt;
						}
						current = children.front();
					}
				} catch (...) {
					// A malformed logical plan must never make planning infer a value by
					// scanning the graph.
					return std::nullopt;
				}
				return std::nullopt;
			}

		private:
			[[nodiscard]] static uint64_t saturatingMultiply(uint64_t left, uint64_t right) noexcept {
				if (left == 0U || right == 0U) {
					return 0U;
				}
				if (left > std::numeric_limits<uint64_t>::max() / right) {
					return std::numeric_limits<uint64_t>::max();
				}
				return left * right;
			}

			static void collectJoinLabels(const logical::LogicalOperator &root, bool insideJoin, size_t depth,
				std::vector<std::vector<std::string>> &labels) {
				if (depth >= 64U || labels.size() >= 64U) return;
				insideJoin = insideJoin || root.getType() == logical::LogicalOpType::LOP_JOIN;
				if (insideJoin) {
					const auto *scan = dynamic_cast<const logical::LogicalNodeScan *>(&root);
					if (scan && !scan->getLabels().empty() &&
						std::find(labels.begin(), labels.end(), scan->getLabels()) == labels.end())
						labels.push_back(scan->getLabels());
				}
				for (const auto *child : root.getChildren())
					if (child) collectJoinLabels(*child, insideJoin, depth + 1U, labels);
			}

			static void collectAnchorLabels(const logical::LogicalOperator &root, size_t depth,
				std::vector<std::vector<std::string>> &labels) {
				if (depth >= 64U || labels.size() >= 64U) return;
				if (const auto *project = dynamic_cast<const logical::LogicalProject *>(&root)) {
					const auto children = project->getChildren();
					const auto *traversal = children.size() == 1U ? dynamic_cast<const logical::LogicalTraversal *>(children.front()) : nullptr;
					const auto inputs = traversal ? traversal->getChildren() : std::vector<logical::LogicalOperator *>{};
					const auto *scan = inputs.size() == 1U ? dynamic_cast<const logical::LogicalNodeScan *>(inputs.front()) : nullptr;
					if (scan && (!scan->getLabels().empty() || traversal->getDirection() == "both") &&
						traversal->getTargetProperties().empty() && traversal->getTargetLabels().size() == 1U &&
						!traversal->getTargetLabels().front().empty()) {
						for (const auto &set : {scan->getLabels(), traversal->getTargetLabels()})
							if (!set.empty() && labels.size() < 64U && std::find(labels.begin(), labels.end(), set) == labels.end()) labels.push_back(set);
					}
				}
				for (const auto *child : root.getChildren()) if (child) collectAnchorLabels(*child, depth + 1U, labels);
			}

			[[nodiscard]] std::optional<uint64_t> estimateLabels(const std::vector<std::string> &labels) const {
				for (const auto &entry : label_estimates_) {
					if (entry.first == labels) return entry.second;
				}
				const auto estimate = backend_.estimateLogicalNodeCardinality(labels);
				// Rewrites can revisit the same scan. Keep a small planning-local memo;
				// it dies with this provider and cannot outlive a catalog publication.
				if (label_estimates_.size() < 64U) label_estimates_.emplace_back(labels, estimate);
				return estimate;
			}

			mutable std::vector<std::pair<std::vector<std::string>, std::optional<uint64_t>>> label_estimates_;
			const storage::LogicalGraphStatisticsBackend &backend_;
			std::optional<storage::LogicalGraphCardinalityStatistics> statistics_;
		};

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

		[[nodiscard]] bool isQueryRuntimeStatisticsAdministrationPlan(const logical::LogicalOperator &root) {
			const logical::LogicalOperator *current = &root;
			while (current->getType() == logical::LogicalOpType::LOP_EXPLAIN ||
				   current->getType() == logical::LogicalOpType::LOP_PROFILE ||
				   current->getType() == logical::LogicalOpType::LOP_PROJECT ||
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
			const auto *call = dynamic_cast<const logical::LogicalCallProcedure *>(current);
			return call != nullptr && (call->getProcedureName() == "dbms.showStats" ||
									call->getProcedureName() == "dbms.resetStats");
		}

		[[nodiscard]] bool isSlowQueryAdministrationPlan(const logical::LogicalOperator &root) {
			const logical::LogicalOperator *current = &root;
			while (current->getType() == logical::LogicalOpType::LOP_EXPLAIN ||
					current->getType() == logical::LogicalOpType::LOP_PROFILE ||
					current->getType() == logical::LogicalOpType::LOP_PROJECT ||
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
			const auto *call = dynamic_cast<const logical::LogicalCallProcedure *>(current);
			return call != nullptr && (call->getProcedureName() == "dbms.listSlowQueries" ||
									call->getProcedureName() == "dbms.clearSlowQueries");
		}

		class QueryExecutionFinalizer final {
		public:
			QueryExecutionFinalizer(NativeConfigurationService *service, const QueryPlan &plan) noexcept :
				service_(service), root_(plan.root.get()),
				started_(std::chrono::steady_clock::now()), uncaught_at_construction_(std::uncaught_exceptions()) {}

			~QueryExecutionFinalizer() noexcept {
				if (service_ == nullptr || root_ == nullptr) {
					return;
				}
				const uint64_t elapsedMs = static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(
																	std::chrono::steady_clock::now() - started_).count());
				const bool succeeded = std::uncaught_exceptions() == uncaught_at_construction_;
				const std::string logicalPlanType = logical::toString(root_->getType());
				if (!isQueryRuntimeStatisticsAdministrationPlan(*root_)) {
					service_->recordQueryExecution(elapsedMs, succeeded);
				}
				if (!isSlowQueryAdministrationPlan(*root_)) {
					service_->recordSlowQuery(logicalPlanType, elapsedMs, succeeded);
				}
			}

		private:
			NativeConfigurationService *service_ = nullptr;
			const logical::LogicalOperator *root_ = nullptr;
			std::chrono::steady_clock::time_point started_;
			int uncaught_at_construction_ = 0;
		};

		void applyDatabaseResourcePolicy(QueryContext &context, const NativeQueryResourcePolicy &policy) {
			if (policy.maximum_var_length_depth < 0 ||
				policy.maximum_var_length_depth > std::numeric_limits<int>::max()) {
				throw std::logic_error("native query resource policy has an invalid variable-length depth");
			}
			const int databaseDepth = static_cast<int>(policy.maximum_var_length_depth);
			if (!context.maxVarLengthDepth.has_value()) {
				context.maxVarLengthDepth = databaseDepth;
				return;
			}
			context.maxVarLengthDepth = std::min(*context.maxVarLengthDepth, databaseDepth);
		}

	} // namespace

	QueryEngine::QueryEngine(std::shared_ptr<storage::LogicalGraphReadBackend> logicalBackend,
							 LogicalReadExecutionOptions options) :
		logicalBackend_(std::move(logicalBackend)), logicalReadOptions_(std::move(options)),
        planCache_(128U, logicalBackend_ ? logicalBackend_->logicalPlanCacheMemoryOwner() : nullptr) {
		if (!logicalBackend_) {
			throw std::invalid_argument("logical query engine requires a read backend");
		}
		logicalSchemaBackend_ = std::dynamic_pointer_cast<storage::LogicalGraphSchemaBackend>(logicalBackend_);
		logicalVectorSearchBackend_ =
				std::dynamic_pointer_cast<storage::LogicalGraphVectorSearchBackend>(logicalBackend_);
		logicalVectorMaintenanceBackend_ =
				std::dynamic_pointer_cast<storage::LogicalGraphVectorMaintenanceBackend>(logicalBackend_);
		logicalAdministrationBackend_ =
				std::dynamic_pointer_cast<storage::LogicalGraphAdministrationBackend>(logicalBackend_);
		if (logicalAdministrationBackend_) {
			nativeConfigurationService_ =
					std::make_shared<NativeConfigurationService>(logicalAdministrationBackend_);
		}
		logicalProjectionCatalog_ = std::make_shared<gds::LogicalProjectionCatalog>();
	}

	QueryEngine::QueryEngine(std::shared_ptr<storage::LogicalGraphReadBackend> logicalReadBackend,
							 std::shared_ptr<storage::LogicalGraphWriteBackend> logicalWriteBackend,
							 LogicalReadExecutionOptions options) :
		logicalBackend_(std::move(logicalReadBackend)), logicalWriteBackend_(std::move(logicalWriteBackend)),
		logicalReadOptions_(std::move(options)),
        planCache_(128U, logicalBackend_ ? logicalBackend_->logicalPlanCacheMemoryOwner() : nullptr) {
		if (!logicalBackend_ || !logicalWriteBackend_) {
			throw std::invalid_argument("logical read-write query engine requires both logical backend boundaries");
		}
		logicalSchemaBackend_ = std::dynamic_pointer_cast<storage::LogicalGraphSchemaBackend>(logicalBackend_);
		logicalVectorSearchBackend_ =
				std::dynamic_pointer_cast<storage::LogicalGraphVectorSearchBackend>(logicalBackend_);
		logicalVectorMaintenanceBackend_ =
				std::dynamic_pointer_cast<storage::LogicalGraphVectorMaintenanceBackend>(logicalBackend_);
		logicalAdministrationBackend_ =
				std::dynamic_pointer_cast<storage::LogicalGraphAdministrationBackend>(logicalBackend_);
		if (logicalAdministrationBackend_) {
			nativeConfigurationService_ =
					std::make_shared<NativeConfigurationService>(logicalAdministrationBackend_);
		}
		logicalProjectionCatalog_ = std::make_shared<gds::LogicalProjectionCatalog>();
	}

	QueryEngine::~QueryEngine() = default;

	void QueryEngine::resetEphemeralState() noexcept {
		planCache_.releaseMemoryOwner();
		planCacheIdentity_.reset();
		planCacheIdentityInitialized_ = false;
		if (logicalProjectionCatalog_) {
			static_cast<void>(logicalProjectionCatalog_->dropAll());
		}
	}

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
        const auto policy = nativeConfigurationService_ ? nativeConfigurationService_->queryResourcePolicy()
            : NativeQueryResourcePolicy{};
        return withQueryMemoryPolicy(policy, [&] {
            QueryContext context;
            if (logicalBackend_) context.resource_account = validatedQueryResourceAccount(context.resource_account,
                *logicalBackend_, logicalReadOptions_.bounds.maximum_materialized_bytes, policy);
            return buildPlanWithAccount(query, lang, std::move(context.resource_account));
        });
    }

    QueryPlan QueryEngine::buildPlanWithAccount(const std::string &query, Language lang,
            std::shared_ptr<execution::detail::ResourceAccount> account) {
        // Keep private operator allocations charged until the escaped plan's
        // last node is destroyed. Cache insertion temporarily selects CACHE.
        const bool managedPlan = static_cast<bool>(account);
        std::optional<utils::OwnedAllocationScope> allocationScope;
        if (account) allocationScope.emplace(std::make_shared<execution::MaterializedByteBudget>(
            logicalReadOptions_.bounds.maximum_materialized_bytes, std::move(account), std::pmr::new_delete_resource()));
        const auto cacheOwner = logicalBackend_ ? logicalBackend_->logicalPlanCacheMemoryOwner() : nullptr;
        bool cacheAllowed = !managedPlan || static_cast<bool>(cacheOwner);
        if (!cacheAllowed) planCache_.releaseMemoryOwner();
        else {
            try { planCache_.bindMemoryOwner(cacheOwner); }
            catch (const execution::detail::ResourceAdmissionDenied &) { cacheAllowed = false; }
            catch (const execution::MaterializedByteLimitExceeded &) { cacheAllowed = false; }
        }
		// Cache only unbound logical IR; executable slots, parameters and page
		// references are always rebuilt against the execution snapshot.
		const auto refreshCacheIdentity = [&]() {
			const auto observed = logicalBackend_ ? logicalBackend_->logicalPlanCacheIdentity() : std::nullopt;
			if (planCacheIdentityInitialized_ && observed != planCacheIdentity_) {
				planCache_.clear();
			}
			planCacheIdentity_ = observed;
			planCacheIdentityInitialized_ = true;
		};
		auto cached = cacheAllowed ? planCache_.get(query) : std::optional<QueryPlan>{};
		if (cached) {
			const auto previousIdentity = planCacheIdentity_;
			refreshCacheIdentity();
			if (previousIdentity == planCacheIdentity_) return std::move(*cached);
		}

		// Cache miss: parse to logical plan
		auto parser = getParser(lang);
		auto plan = parser->parseToLogical(query);

		if (plan.root) {
			const bool cacheable = cacheAllowed && plan.cacheable && !isLogicalExplainPlan(*plan.root);
			if (cacheable) refreshCacheIdentity();
			const auto compilationIdentity = planCacheIdentity_;
			if (plan.optimizable) {
				optimization::LogicalOptimizerOptions optimizerOptions;
				optimizerOptions.allow_read_traversal_anchor = !plan.mutatesData && !plan.mutatesSchema;
				optimization::LogicalPlanOptimizer optimizer(optimizerOptions);
				const auto statisticsBackend =
						std::dynamic_pointer_cast<storage::LogicalGraphStatisticsBackend>(logicalBackend_);
				std::optional<BackendLogicalStatisticsProvider> statistics;
				if (statisticsBackend) {
					statistics.emplace(*statisticsBackend, *plan.root, optimizerOptions.allow_read_traversal_anchor);
				}
				plan.root = optimizer.optimize(std::move(plan.root),
						statistics.has_value() ? std::addressof(*statistics) : nullptr).root;
			}

			// DDL operations invalidate the cache (schema changes affect plans)
			if (plan.mutatesSchema) {
				planCache_.clear();
			}

			if (cacheable) {
				refreshCacheIdentity();
				// Concurrent publication cannot stamp older optimizer observations
				// with a newer identity. Execute the unbound IR without caching it.
				if (compilationIdentity == planCacheIdentity_) {
                    // A cache admission refusal is optional; source QUERY
                    // admission, bad_alloc and semantic errors still propagate.
                    try { planCache_.put(query, plan); }
                    catch (const execution::detail::ResourceAdmissionDenied &) {}
                    catch (const execution::MaterializedByteLimitExceeded &) {}
                }
			}
		}

		return plan;
	}

	QueryResult QueryEngine::executePlan(QueryPlan plan, const QueryContext &ctx) {
        return executePlanWithAccount(std::move(plan), ctx, ctx.resource_account);
    }

    QueryResult QueryEngine::executePlanWithAccount(QueryPlan plan, const QueryContext &ctx,
            std::shared_ptr<execution::detail::ResourceAccount> suppliedAccount) {
		if (!plan.root) {
			return QueryResult{};
		}
		QueryExecutionFinalizer queryExecutionFinalizer(nativeConfigurationService_.get(), plan);
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
			const NativeQueryResourcePolicy resourcePolicy = nativeConfigurationService_
					? nativeConfigurationService_->queryResourcePolicy()
					: NativeQueryResourcePolicy{};
            return withQueryMemoryPolicy(resourcePolicy, [&]() -> QueryResult {
			QueryGuard defaultGuard(resourcePolicy.timeout_ms, resourcePolicy.maximum_memory_mb);
			auto account = validatedQueryResourceAccount(suppliedAccount, *logicalBackend_,
					logicalReadOptions_.bounds.maximum_materialized_bytes, resourcePolicy);
			auto parameterCredit = admitQueryParameterCopy(ctx, account);
			QueryContext logicalContext = ctx;
			logicalContext.resource_account = std::move(account);
			if (logicalContext.guard == nullptr) {
				logicalContext.guard = &defaultGuard;
			}
			applyDatabaseResourcePolicy(logicalContext, resourcePolicy);
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
					return runtime.execute(executionRoot, logicalReadOptions_.bounds.maximum_rows, logicalContext,
						logicalReadOptions_.bounds.maximum_materialized_bytes);
				}
				// Legacy planning marks some procedures as data mutations. Dispatch
				// them before the mutation runtime so unsupported procedures fail at
				// the logical procedure boundary instead of being misclassified as CREATE.
				if (isLogicalProcedurePlan(executionRoot)) {
					execution::LogicalPlanProcedureRuntime runtime(*logicalBackend_, logicalVectorSearchBackend_.get(),
														logicalVectorMaintenanceBackend_.get(),
														nativeConfigurationService_.get(),
														logicalProjectionCatalog_.get(), threadPool_);
					return runtime.execute(executionRoot, logicalReadOptions_.bounds,
										   logicalReadOptions_.maximum_vector_mapping_entries,
										   logicalReadOptions_.maximum_raw_vector_reads,
										   logicalReadOptions_.vector_candidate_search_budget,
													 logicalReadOptions_.vector_candidate_refinement_budget,
													 logicalReadOptions_.maximum_vector_training_records,
													 logicalReadOptions_.maximum_vector_training_pages,
													 logicalReadOptions_.maximum_gds_algorithm_edges,
													 logicalReadOptions_.maximum_gds_algorithm_iterations,
													 logicalReadOptions_.maximum_gds_algorithm_work_units, logicalContext);
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
            });
		}

		throw std::logic_error("native query engine has no logical storage backend");
	}

	QueryResult QueryEngine::executePlanWithLogicalTransactionOverlay(
			QueryPlan plan, const QueryContext &ctx, storage::LogicalGraphReadBackend &readBackend,
			storage::LogicalGraphWriteBackend &writeBackend, gds::LogicalProjectionCatalog *transactionProjectionCatalog) {
		if (!plan.root) {
			return QueryResult{};
		}
		QueryExecutionFinalizer queryExecutionFinalizer(nativeConfigurationService_.get(), plan);
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

		const NativeQueryResourcePolicy resourcePolicy = nativeConfigurationService_
				? nativeConfigurationService_->queryResourcePolicy()
				: NativeQueryResourcePolicy{};
        return withQueryMemoryPolicy(resourcePolicy, [&]() -> QueryResult {
		QueryGuard defaultGuard(resourcePolicy.timeout_ms, resourcePolicy.maximum_memory_mb);
		auto account = validatedQueryResourceAccount(ctx.resource_account, *logicalBackend_,
				logicalReadOptions_.bounds.maximum_materialized_bytes, resourcePolicy);
		auto parameterCredit = admitQueryParameterCopy(ctx, account);
		QueryContext logicalContext = ctx;
		logicalContext.resource_account = std::move(account);
		if (logicalContext.guard == nullptr) {
			logicalContext.guard = &defaultGuard;
		}
		applyDatabaseResourcePolicy(logicalContext, resourcePolicy);
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
				// The runtime applies the manifest's explicit scope classification before
				// opening a snapshot or invoking a procedure side effect. A permitted
				// procedure therefore reads this transaction's overlay, never the engine's
				// autocommit backend.
				execution::LogicalPlanProcedureRuntime runtime(
						readBackend, dynamic_cast<const storage::LogicalGraphVectorSearchBackend *>(&readBackend),
						dynamic_cast<storage::LogicalGraphVectorMaintenanceBackend *>(&writeBackend),
						nativeConfigurationService_.get(),
						transactionProjectionCatalog, threadPool_);
				return runtime.execute(executionRoot, logicalReadOptions_.bounds,
									   logicalReadOptions_.maximum_vector_mapping_entries,
									   logicalReadOptions_.maximum_raw_vector_reads,
									   logicalReadOptions_.vector_candidate_search_budget,
									   logicalReadOptions_.vector_candidate_refinement_budget,
									   logicalReadOptions_.maximum_vector_training_records,
									   logicalReadOptions_.maximum_vector_training_pages,
									   logicalReadOptions_.maximum_gds_algorithm_edges,
									   logicalReadOptions_.maximum_gds_algorithm_iterations,
									   logicalReadOptions_.maximum_gds_algorithm_work_units, logicalContext,
									   execution::LogicalProcedureExecutionScope::LPES_TRANSACTION_OVERLAY);
			}
			if (plan.mutatesData) {
				// Cypher transaction batches commit independently and cannot be nested
				// inside this caller-owned overlay.
				execution::LogicalPlanMutationRuntime runtime(readBackend, writeBackend, false);
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
        });
	}

	bool QueryEngine::usesLogicalReadBackend(const storage::LogicalGraphReadBackend &backend) const noexcept {
		return logicalBackend_.get() == &backend;
	}

	QueryResult QueryEngine::execute(const std::string &query, const QueryContext &ctx, const Language lang) {
        using Clock = std::chrono::steady_clock;
        const auto parseStart = Clock::now();
        const auto policy = nativeConfigurationService_ ? nativeConfigurationService_->queryResourcePolicy()
            : NativeQueryResourcePolicy{};
        return withQueryMemoryPolicy(policy, [&] {
            auto account = ctx.resource_account;
            if (logicalBackend_) account = validatedQueryResourceAccount(ctx.resource_account, *logicalBackend_,
                logicalReadOptions_.bounds.maximum_materialized_bytes, policy);
            auto plan = buildPlanWithAccount(query, lang, account);
            debug::PerfTrace::addDuration("parse", static_cast<uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - parseStart).count()));
            return executePlanWithAccount(std::move(plan), ctx, std::move(account));
        });
    }

} // namespace graph::query
