/**
 * @file PreparedQueryInternal.hpp
 * @brief Private bridge between the public typed query API and logical plans.
 *
 * Licensed under the Apache License, Version 2.0.
 */

#pragma once

#include <memory>

#include "graph/query/QueryContext.hpp"
#include "graph/query/QueryPlan.hpp"
#include "zyx/zyx.hpp"

namespace zyx::detail {

	class PreparedQueryImpl final {
	public:
		graph::query::QueryPlan plan;
		graph::query::QueryContext context;
	};

	class QueryBuilderImpl;

	[[nodiscard]] graph::query::QueryPlan clonePreparedQueryPlan(const PreparedQueryImpl &prepared);
	[[nodiscard]] graph::query::QueryContext preparedQueryContext(const PreparedQueryImpl &prepared);

} // namespace zyx::detail
