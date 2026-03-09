/**
 * @file ReadingClauseHandler.hpp
 * @author Nexepic
 * @date 2025/12/9
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

#include <memory>
#include <string>
#include <vector>
#include "generated/CypherParser.h"
#include "graph/core/Types.hpp"
#include "graph/query/execution/PhysicalOperator.hpp"

namespace graph::query {
	class QueryPlanner;
}

namespace graph::parser::cypher::clauses {

/**
 * @class ReadingClauseHandler
 * @brief Handles Cypher reading clauses: MATCH, CALL, UNWIND.
 *
 * This class processes clauses that read data from the database or
 * transform existing data streams without modification.
 */
class ReadingClauseHandler {
public:
	/**
	 * @brief Handle a MATCH statement.
	 *
	 * @param ctx The match statement context
	 * @param rootOp The current root operator (may be null)
	 * @param planner The query planner for creating operators
	 * @return The resulting operator after processing MATCH
	 */
	static std::unique_ptr<query::execution::PhysicalOperator> handleMatch(
		CypherParser::MatchStatementContext *ctx,
		std::unique_ptr<query::execution::PhysicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle a standalone CALL statement (not in a query pipeline).
	 *
	 * @param ctx The standalone call statement context
	 * @param rootOp The current root operator (may be null)
	 * @param planner The query planner for creating operators
	 * @return The resulting operator after processing CALL
	 */
	static std::unique_ptr<query::execution::PhysicalOperator> handleStandaloneCall(
		CypherParser::StandaloneCallStatementContext *ctx,
		std::unique_ptr<query::execution::PhysicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle an in-query CALL statement (with YIELD).
	 *
	 * @param ctx The in-query call statement context
	 * @param rootOp The current root operator (may be null)
	 * @param planner The query planner for creating operators
	 * @return The resulting operator after processing CALL with YIELD
	 */
	static std::unique_ptr<query::execution::PhysicalOperator> handleInQueryCall(
		CypherParser::InQueryCallStatementContext *ctx,
		std::unique_ptr<query::execution::PhysicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle an UNWIND clause.
	 *
	 * @param ctx The unwind statement context
	 * @param rootOp The current root operator (may be null)
	 * @param planner The query planner for creating operators
	 * @return The resulting operator after processing UNWIND
	 */
	static std::unique_ptr<query::execution::PhysicalOperator> handleUnwind(
		CypherParser::UnwindStatementContext *ctx,
		std::unique_ptr<query::execution::PhysicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);

	/**
	 * @brief Handle an OPTIONAL MATCH statement.
	 *
	 * OPTIONAL MATCH provides left outer join semantics. If the pattern
	 * doesn't match, it returns NULL for unmatched variables instead of
	 * filtering out the row.
	 *
	 * @param ctx The match statement context (with OPTIONAL flag)
	 * @param rootOp The current root operator (may be null)
	 * @param planner The query planner for creating operators
	 * @return The resulting operator after processing OPTIONAL MATCH
	 */
	static std::unique_ptr<query::execution::PhysicalOperator> handleOptionalMatch(
		CypherParser::MatchStatementContext *ctx,
		std::unique_ptr<query::execution::PhysicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);
};

} // namespace graph::parser::cypher::clauses
