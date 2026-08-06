/**
 * @file LogicalPlanMutationRuntime.hpp
 * @brief Declarative relocatable logical-plan mutation compiler.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include "graph/query/QueryContext.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/query/execution/LogicalPlanReadRuntime.hpp"
#include "graph/storage/LogicalGraphWrite.hpp"

namespace graph::query::logical {
	class LogicalOperator;
}

namespace graph::query::execution {

	// Compiles supported write-only logical plans into one format-independent
	// mutation batch. It deliberately has no legacy physical-plan fallback.
	class LogicalPlanMutationRuntime final {
	public:
		LogicalPlanMutationRuntime(const storage::LogicalGraphReadBackend &readBackend,
								   storage::LogicalGraphWriteBackend &writeBackend) :
			read_backend_(readBackend), write_backend_(writeBackend) {}

		[[nodiscard]] QueryResult execute(const logical::LogicalOperator &root,
										  const LogicalPlanReadRuntimeBounds &bounds,
										  const QueryContext &context) const;

	private:
		const storage::LogicalGraphReadBackend &read_backend_;
		storage::LogicalGraphWriteBackend &write_backend_;
	};

} // namespace graph::query::execution
