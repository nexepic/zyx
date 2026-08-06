/**
 * @file LogicalPlanSchemaRuntime.hpp
 * @brief Fail-closed logical DDL runtime for schema-capable storage backends.
 *
 * Copyright (c) 2026 Nexepic
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include "graph/query/QueryContext.hpp"
#include "graph/query/api/QueryResult.hpp"
#include "graph/storage/LogicalGraphSchema.hpp"

namespace graph::query::logical {
	class LogicalOperator;
}

namespace graph::query::execution {

	// DDL never falls back to a legacy physical operator. A logical backend must
	// explicitly opt into this schema surface, otherwise its unsupported schema
	// operators fail before any mutation reaches a different storage format.
	class LogicalPlanSchemaRuntime final {
	public:
		explicit LogicalPlanSchemaRuntime(storage::LogicalGraphSchemaBackend &backend) : backend_(backend) {}

		[[nodiscard]] QueryResult execute(const logical::LogicalOperator &root, size_t maximumIndexes,
													  const QueryContext &context) const;

	private:
		storage::LogicalGraphSchemaBackend &backend_;
	};

} // namespace graph::query::execution
