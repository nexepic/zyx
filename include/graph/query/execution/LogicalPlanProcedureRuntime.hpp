/**
 * @file LogicalPlanProcedureRuntime.hpp
 * @brief Bounded logical stored-procedure runtime for format-independent backends.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <cstddef>
#include <string_view>

#include "graph/query/QueryContext.hpp"
#include "graph/query/NativeConfigurationService.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/concurrent/ThreadPool.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/query/gds/LogicalProjectionCatalog.hpp"
#include "graph/storage/LogicalGraphVectorSearch.hpp"

namespace graph::query::logical {
	class LogicalOperator;
}

namespace graph::query::execution {

	enum class LogicalProcedureExecutionScope {
		LPES_AUTOCOMMIT,
		LPES_TRANSACTION_OVERLAY,
	};

	// Procedure support is deliberately independent from the legacy physical
	// registry. Each supported logical procedure validates its own arguments and
	// must materialize every entity from the same snapshot that performed the
	// procedure's storage read.
	class LogicalPlanProcedureRuntime final {
	public:
		LogicalPlanProcedureRuntime(const storage::LogicalGraphReadBackend &readBackend,
									const storage::LogicalGraphVectorSearchBackend *vectorBackend,
									storage::LogicalGraphVectorMaintenanceBackend *vectorMaintenanceBackend,
									NativeConfigurationService *configurationService = nullptr,
									gds::LogicalProjectionCatalog *projectionCatalog = nullptr,
									concurrent::ThreadPool *threadPool = nullptr,
                                    std::shared_ptr<std::pmr::memory_resource> projectionMemoryOwner = {}) : // ZYX_COV_EXCL_LINE ZYX_COV_EXCL_FUNCTION: header-only
													   // constructor is executed through its linked definition; another
													   // inline instantiation has no independent counter.
			read_backend_(readBackend), vector_backend_(vectorBackend),
			vector_maintenance_backend_(vectorMaintenanceBackend), configuration_service_(configurationService),
			projection_catalog_(projectionCatalog),
			thread_pool_(threadPool), projection_memory_owner_(std::move(projectionMemoryOwner)) {}

		[[nodiscard]] QueryResult
		execute(const logical::LogicalOperator &root, const LogicalPlanReadRuntimeBounds &readBounds,
				size_t maximumVectorMappingEntries, size_t maximumRawVectorReads, size_t vectorCandidateSearchBudget,
				size_t vectorCandidateRefinementBudget, size_t maximumVectorTrainingRecords,
				size_t maximumVectorTrainingPages, size_t maximumGdsAlgorithmEdges,
				size_t maximumGdsAlgorithmIterations, size_t maximumGdsAlgorithmWorkUnits, const QueryContext &context,
				LogicalProcedureExecutionScope scope = LogicalProcedureExecutionScope::LPES_AUTOCOMMIT) const;

		[[nodiscard]] static bool supportsNativeProcedure(std::string_view name) noexcept;

	private:
		const storage::LogicalGraphReadBackend &read_backend_;
		const storage::LogicalGraphVectorSearchBackend *vector_backend_ = nullptr;
		storage::LogicalGraphVectorMaintenanceBackend *vector_maintenance_backend_ = nullptr;
		NativeConfigurationService *configuration_service_ = nullptr;
		gds::LogicalProjectionCatalog *projection_catalog_ = nullptr;
		concurrent::ThreadPool *thread_pool_ = nullptr;
        std::shared_ptr<std::pmr::memory_resource> projection_memory_owner_;
	};

} // namespace graph::query::execution
