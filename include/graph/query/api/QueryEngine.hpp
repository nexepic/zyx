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

#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include "QueryResult.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/QueryContext.hpp"
#include "graph/query/NativeConfigurationService.hpp"
#include "graph/query/QueryPlan.hpp"
#include "graph/query/cache/PlanCache.hpp"
#include "graph/query/execution/LogicalPlanMutationRuntime.hpp"
#include "graph/query/execution/LogicalPlanProcedureRuntime.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/execution/LogicalPlanSchemaRuntime.hpp"
#include "graph/query/gds/LogicalProjectionCatalog.hpp"
#include "graph/storage/LogicalGraphRead.hpp"
#include "graph/storage/LogicalGraphAdministration.hpp"
#include "graph/storage/LogicalGraphSchema.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"
#include "query/parser/common/IQueryParser.hpp"

namespace graph::query {

	enum class Language { Cypher };

	// Bounded defaults for the first production read-only logical backend path.
	// Callers that need different operational limits can supply explicit values.
	struct LogicalReadExecutionOptions {
		execution::LogicalPlanReadRuntimeBounds bounds{
				.maximum_node_records = 1'000'000U,
				.maximum_node_page_records = 1'024U,
				.maximum_metadata_pages_per_node_page = 4'096U,
				.maximum_edges_per_node = 100'000U,
				.maximum_edge_records = 10'000'000U,
				.maximum_adjacency_page_records = 1'024U,
				.maximum_property_entries = 10'000U,
				.maximum_rows = 1'000'000U,
				.maximum_distinct_rows = 100'000U,
		};
		size_t maximum_vector_mapping_entries = 100'000U;
		size_t maximum_raw_vector_reads = 100'000U;
		size_t vector_candidate_search_budget = 1'024U;
		size_t vector_candidate_refinement_budget = 256U;
		// Training is an explicit write-side maintenance operation. Its independent
		// bounds prevent one CALL from turning a large graph into unbounded RAM or
		// derived-page allocation work.
		size_t maximum_vector_training_records = 100'000U;
		size_t maximum_vector_training_pages = 4'096U;
		// GDS procedures operate on an already bounded projection, but their CPU
		// work must have an independent cap so an iteration-heavy CALL cannot
		// monopolize an embedded database process.
		size_t maximum_gds_algorithm_edges = 10'000'000U;
		size_t maximum_gds_algorithm_iterations = 128U;
		size_t maximum_gds_algorithm_work_units = 50'000'000U;
		// Empty means disabled. Database wires a canonical database-directory
		// root; embedders can instead provide a narrower dedicated import root.
		std::optional<std::filesystem::path> csv_import_root;
	};

	class QueryEngine {
	public:
		explicit QueryEngine(std::shared_ptr<storage::LogicalGraphReadBackend> logicalBackend,
							 LogicalReadExecutionOptions options = {});
		QueryEngine(std::shared_ptr<storage::LogicalGraphReadBackend> logicalReadBackend,
					std::shared_ptr<storage::LogicalGraphWriteBackend> logicalWriteBackend,
					LogicalReadExecutionOptions options = {});
		~QueryEngine();

		// --- Main Execution ---
		QueryResult execute(const std::string &query, Language lang = Language::Cypher);
		QueryResult execute(const std::string &query, const QueryContext &ctx, Language lang = Language::Cypher);

		/**
		 * @brief Builds a QueryPlan from a query string (parse + optimize + cache).
		 *        Does NOT execute. Use executePlan() to run the result.
		 */
		QueryPlan buildPlan(const std::string &query, Language lang = Language::Cypher);

		/**
		 * @brief Executes a pre-built QueryPlan with access control checks.
		 *        Checks execMode against plan mutation flags.
		 * @throws std::runtime_error if the plan violates the execution mode.
		 */
		QueryResult executePlan(QueryPlan plan, const QueryContext &ctx);
		// Executes through one caller-owned logical transaction overlay. Reads see
		// its staged final state and writes remain unpublished until that owner
		// commits, rather than opening an independent auto-commit snapshot.
		QueryResult executePlanWithLogicalTransactionOverlay(QueryPlan plan, const QueryContext &ctx,
														 storage::LogicalGraphReadBackend &readBackend,
														 storage::LogicalGraphWriteBackend &writeBackend,
														 gds::LogicalProjectionCatalog *transactionProjectionCatalog = nullptr);
		// A public Transaction may execute only through an engine attached to its
		// own logical backend. This prevents an overlay from being combined with a
		// different Database's policy, catalog, or lifecycle state.
		[[nodiscard]] bool usesLogicalReadBackend(const storage::LogicalGraphReadBackend &backend) const noexcept;
		void setThreadPool(concurrent::ThreadPool *pool) { threadPool_ = pool; }
		// Database close ends the process-local lifetime of derived projections.
		// They must never become visible after the same file is reopened.
		void resetEphemeralState() noexcept;

		[[nodiscard]] cache::PlanCache &getPlanCache() { return planCache_; }

	private:
        QueryResult executePlanWithAccount(QueryPlan plan, const QueryContext &ctx,
            std::shared_ptr<execution::detail::ResourceAccount> account);
		std::shared_ptr<storage::LogicalGraphReadBackend> logicalBackend_;
		std::shared_ptr<storage::LogicalGraphWriteBackend> logicalWriteBackend_;
		std::shared_ptr<storage::LogicalGraphSchemaBackend> logicalSchemaBackend_;
		std::shared_ptr<storage::LogicalGraphVectorSearchBackend> logicalVectorSearchBackend_;
		std::shared_ptr<storage::LogicalGraphVectorMaintenanceBackend> logicalVectorMaintenanceBackend_;
		std::shared_ptr<storage::LogicalGraphAdministrationBackend> logicalAdministrationBackend_;
		std::shared_ptr<NativeConfigurationService> nativeConfigurationService_;
		// Projections are process-local derived views. Keeping the catalog here gives
		// every Database-owned QueryEngine an isolated lifetime without persisting
		// derived graph state in the database file.
		std::shared_ptr<gds::LogicalProjectionCatalog> logicalProjectionCatalog_;
		LogicalReadExecutionOptions logicalReadOptions_;
		std::unordered_map<Language, std::shared_ptr<parser::IQueryParser>> parsers_;
		concurrent::ThreadPool *threadPool_ = nullptr;
		cache::PlanCache planCache_;
		std::optional<storage::LogicalGraphSchemaIdentity> planCacheIdentity_;
		bool planCacheIdentityInitialized_ = false;

		std::shared_ptr<parser::IQueryParser> getParser(Language lang);
        QueryPlan buildPlanWithAccount(const std::string &query, Language lang,
            std::shared_ptr<execution::detail::ResourceAccount> account);

	};

} // namespace graph::query
