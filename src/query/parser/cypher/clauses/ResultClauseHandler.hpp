/**
 * @file ResultClauseHandler.hpp
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
#include "generated/CypherParser.h"
#include "graph/query/execution/PhysicalOperator.hpp"

namespace graph::query {
	class QueryPlanner;
}

namespace graph::parser::cypher::clauses {

/**
 * @class ResultClauseHandler
 * @brief Handles Cypher result clauses: RETURN.
 *
 * This class processes clauses that shape and return query results,
 * including projection, sorting, and pagination (LIMIT/SKIP).
 */
class ResultClauseHandler {
public:
	/**
	 * @brief Handle a RETURN statement.
	 *
	 * Processes:
	 * - Projection (SELECT)
	 * - ORDER BY
	 * - SKIP
	 * - LIMIT
	 *
	 * @param ctx The return statement context
	 * @param rootOp The current root operator (may be null, injects SingleRowOperator if needed)
	 * @param planner The query planner for creating operators
	 * @return The resulting operator after processing RETURN
	 */
	static std::unique_ptr<query::execution::PhysicalOperator> handleReturn(
		CypherParser::ReturnStatementContext *ctx,
		std::unique_ptr<query::execution::PhysicalOperator> rootOp,
		const std::shared_ptr<query::QueryPlanner> &planner);
};

} // namespace graph::parser::cypher::clauses
